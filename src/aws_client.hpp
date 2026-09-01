#pragma once

#include "dynamodb_secret.hpp"
#include "include/dynamodb_extension.hpp"
#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/model/ScanRequest.h>
#include <aws/dynamodb/model/QueryRequest.h>
#include <aws/dynamodb/model/GetItemRequest.h>
#include <aws/dynamodb/model/DescribeTableRequest.h>
#include <stdexcept>

namespace duckdb {

// ─────────────────────────────────────────────
// One page of items returned from DynamoDB
// ─────────────────────────────────────────────
struct DynamoPage {
	std::vector<Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>> items;
	// Empty map = no more pages
	Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> next_cursor;
};

// ─────────────────────────────────────────────
// AWSClientWrapper — constructed once per scan,
// holds the SDK client configured for the right endpoint/region
// ─────────────────────────────────────────────
class AWSClientWrapper {
public:
	explicit AWSClientWrapper(
		const TableConfig &config,
		const DynamoDBSecretConfig &secret_config) {

		Aws::Client::ClientConfiguration cfg;

		if (!config.endpoint_url.empty()) {
			cfg.endpointOverride = config.endpoint_url;
		}

		if (!secret_config.region.empty()) {
			cfg.region = secret_config.region;
		}

		if (secret_config.provider == "credential_chain") {
			auto provider = Aws::MakeShared<Aws::Auth::DefaultAWSCredentialsProviderChain>("dynamodb");

			client_ = std::make_unique<Aws::DynamoDB::DynamoDBClient>(provider, cfg);
		} else {
			Aws::Auth::AWSCredentials creds(
				secret_config.access_key_id,
				secret_config.secret_access_key,
				secret_config.session_token
			);

			client_ = std::make_unique<Aws::DynamoDB::DynamoDBClient>(creds, cfg);
		}
	}

	// ── DescribeTable ─────────────────────────────────────────────────────
	// Used at bind time to discover PK/SK and GSI definitions automatically.
	Aws::DynamoDB::Model::DescribeTableResult DescribeTable(const std::string &table_name) {
		Aws::DynamoDB::Model::DescribeTableRequest req;
		req.SetTableName(table_name.c_str());
		auto outcome = client_->DescribeTable(req);
		if (!outcome.IsSuccess()) {
			throw std::runtime_error("DescribeTable failed: " + outcome.GetError().GetMessage());
		}
		return outcome.GetResult();
	}

	// ── Scan — full table or single segment ───────────────────────────────
	// segment_index / total_segments enable DynamoDB Parallel Scan.
	// Each thread calls this with its own segment index independently.
	DynamoPage Scan(const TableConfig &config, const std::vector<std::string> &projection_cols,
	                const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> &start_key, int segment_index,
	                int total_segments) {
		Aws::DynamoDB::Model::ScanRequest req;
		Aws::Map<Aws::String, Aws::String> expr_attr_names;
		req.SetTableName(config.table_name);
		req.SetSegment(segment_index);
		req.SetTotalSegments(total_segments);

		std::string proj = BuildProjection(projection_cols, expr_attr_names);
		if (!proj.empty()) {
			req.SetProjectionExpression(proj);
		}

		if (!expr_attr_names.empty()) {
			req.SetExpressionAttributeNames(expr_attr_names);
		}

		// Resume from where this segment left off
		if (!start_key.empty()) {
			req.SetExclusiveStartKey(start_key);
		}

		auto outcome = client_->Scan(req);
		if (!outcome.IsSuccess()) {
			// Includes throttling (ProvisionedThroughputExceededException)
			// Caller should implement exponential backoff and retry
			throw std::runtime_error("Scan failed: " + outcome.GetError().GetMessage());
		}

		DynamoPage page;
		page.items = outcome.GetResult().GetItems();
		page.next_cursor = outcome.GetResult().GetLastEvaluatedKey(); // empty = done
		return page;
	}

	// ── Convert DuckDB Data Types back to DynamoDB Types ───────────────────
	Aws::DynamoDB::Model::AttributeValue ConvertDuckDBToDynamo(const Value &val) {
		Aws::DynamoDB::Model::AttributeValue av;

		if (val.IsNull()) {
			av.SetNull(true);
			return av;
		}

		auto &type = val.type();

		if (type == LogicalType::VARCHAR) {
			av.SetS(val.GetValue<std::string>());
		}
		else if (type.IsNumeric()) {
			av.SetN(val.ToString());
		}
		else if (type == LogicalType::BOOLEAN) {
			av.SetBool(val.GetValue<bool>());
		}
		else if (type == LogicalType::BLOB) {
			auto blob_str = val.GetValueUnsafe<std::string>();
			av.SetB(Aws::Utils::ByteBuffer(
				reinterpret_cast<const unsigned char*>(blob_str.data()),
				blob_str.size()
			));
		}
		else {
			// Fallback for timestamps, dates, ..
			av.SetS(val.ToString());
		}

		return av;
	}


	// ── Query — PK/SK key condition, or through a GSI ─────────────────────
	DynamoPage Query(const TableConfig &config, const string &key_condition_expr, const string &index_name,
	                 const std::vector<string> &projection_cols, const unordered_map<string, string> &expr_attr_values,
	                 const unordered_map<string, string> &expr_attr_names_in,
	                 const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> &start_key) {
		Aws::DynamoDB::Model::QueryRequest req;
		Aws::Map<Aws::String, Aws::String> expr_attr_names;
		std::string proj = BuildProjection(projection_cols, expr_attr_names);

		req.SetTableName(config.table_name);
		req.SetKeyConditionExpression(key_condition_expr);

		if (!index_name.empty()) {
			req.SetIndexName(index_name);
		}

		for (auto it = expr_attr_names_in.begin(); it != expr_attr_names_in.end(); ++it) {
			expr_attr_names[it->first] = it->second;
		}

		if (!proj.empty()) {
			req.SetProjectionExpression(proj);
		}

		if (!expr_attr_names.empty()) {
			req.SetExpressionAttributeNames(expr_attr_names);
		}

		// Convert DuckDB Values directly to DynamoDB AttributeValues
		for (const auto &[k, duck_val] : expr_attr_values) {
			req.AddExpressionAttributeValues(k, ConvertDuckDBToDynamo(duck_val));
		}

		if (!start_key.empty()) {
			req.SetExclusiveStartKey(start_key);
		}

		auto outcome = client_->Query(req);
		if (!outcome.IsSuccess()) {
			throw std::runtime_error("Query failed: " + outcome.GetError().GetMessage());
		}

		DynamoPage page;
		page.items = outcome.GetResult().GetItems();
		page.next_cursor = outcome.GetResult().GetLastEvaluatedKey();
		return page;
	}

	// ── GetItem — exact PK+SK, cheapest possible read (1 RCU) ─────────────
	DynamoPage GetItem(const TableConfig &config, const std::string &pk_value, const std::string &sk_value,
	                   const std::vector<std::string> &projection_cols) {
		Aws::DynamoDB::Model::GetItemRequest req;
		Aws::Map<Aws::String, Aws::String> expr_attr_names;
		req.SetTableName(config.table_name);

		Aws::DynamoDB::Model::AttributeValue pk_av;
		pk_av.SetS(pk_value);
		req.AddKey(config.pk_name, pk_av);

		if (!config.sk_name.empty() && !sk_value.empty()) {
			Aws::DynamoDB::Model::AttributeValue sk_av;
			sk_av.SetS(sk_value);
			req.AddKey(config.sk_name, sk_av);
		}

		if (!projection_cols.empty()) {
			req.SetProjectionExpression(BuildProjection(projection_cols, expr_attr_names));
		}

		auto outcome = client_->GetItem(req);
		if (!outcome.IsSuccess()) {
			throw std::runtime_error("GetItem failed: " + outcome.GetError().GetMessage());
		}

		DynamoPage page;
		auto item = outcome.GetResult().GetItem();
		if (!item.empty()) {
			page.items.push_back(item);
		}
		return page;
	}

private:
	std::unique_ptr<Aws::DynamoDB::DynamoDBClient> client_;

	// Comma-separated projection expression from column names
	static std::string BuildProjection(const std::vector<std::string> &cols,
	                                   Aws::Map<Aws::String, Aws::String> &expr_attr_names) {
		for (const auto &col : cols) {
			if (col == "_extra") {
				return "";
			}
		}
		std::string expr;
		for (const auto &col : cols) {
			if (col == "_extra")
				continue;
			if (!expr.empty())
				expr += ", ";
			std::string alias = "#proj_" + col;
			expr += alias;
			expr_attr_names[alias] = col;
		}
		return expr;
	}
};

} // namespace duckdb
