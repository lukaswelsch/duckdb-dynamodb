#pragma once

// aws_client.hpp
// Thin wrapper around the AWS SDK DynamoDB client.
// Swap the endpoint_url to point at DynamoDB Local for testing.

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
    explicit AWSClientWrapper(const TableConfig &config) {
        Aws::Client::ClientConfiguration cfg;
        cfg.region = config.region;

        // ← Point at DynamoDB Local when endpoint_url is set
        // e.g.  endpoint_url = "http://localhost:8000"
        if (!config.endpoint_url.empty()) {
            cfg.endpointOverride = config.endpoint_url;
        }

        client_ = std::make_unique<Aws::DynamoDB::DynamoDBClient>(cfg);
    }

    // ── DescribeTable ─────────────────────────────────────────────────────
    // Used at bind time to discover PK/SK and GSI definitions automatically.
    Aws::DynamoDB::Model::DescribeTableResult DescribeTable(const std::string &table_name) {
        Aws::DynamoDB::Model::DescribeTableRequest req;
        req.SetTableName(table_name.c_str());
        auto outcome = client_->DescribeTable(req);
        if (!outcome.IsSuccess()) {
            throw std::runtime_error("DescribeTable failed: " +
                                     outcome.GetError().GetMessage());
        }
        return outcome.GetResult();
    }

    // ── Scan — full table or single segment ───────────────────────────────
    // segment_index / total_segments enable DynamoDB Parallel Scan.
    // Each thread calls this with its own segment index independently.
    DynamoPage Scan(const TableConfig &config,
                    const std::vector<std::string> &projection_cols,
                    const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> &start_key,
                    int segment_index,
                    int total_segments) {

			fprintf(stderr, "DynamPage Aws wrapper scanning\n");


        Aws::DynamoDB::Model::ScanRequest req;
    	Aws::Map<Aws::String, Aws::String> expr_attr_names;
        req.SetTableName(config.table_name);
        req.SetSegment(segment_index);
        req.SetTotalSegments(total_segments);

        // Projection pushdown — fetch only the columns DuckDB needs
        if (!projection_cols.empty()) {
            req.SetProjectionExpression(BuildProjection(projection_cols, expr_attr_names));
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
            throw std::runtime_error("Scan failed: " +
                                     outcome.GetError().GetMessage());
        }

    	fprintf(stderr, "DYNAMO SCAN: segment=%d/%d → %zu items returned, has_more=%s\n",
			segment_index, total_segments,
			outcome.GetResult().GetItems().size(),
			outcome.GetResult().GetLastEvaluatedKey().empty() ? "no" : "yes");

        DynamoPage page;
        page.items      = outcome.GetResult().GetItems();
        page.next_cursor = outcome.GetResult().GetLastEvaluatedKey(); // empty = done
        return page;
    }

    // ── Query — PK/SK key condition, or through a GSI ─────────────────────
    DynamoPage Query(const TableConfig &config,
                     const std::string &key_condition_expr,
                     const std::string &filter_expr,        // may be empty
                     const std::string &index_name,         // may be empty
                     const std::vector<std::string> &projection_cols,
                     const std::unordered_map<std::string, std::string> &expr_attr_values,
                     const std::unordered_map<std::string, std::string> &expr_attr_names_in,
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


        // Bind :placeholder → value mappings
        for (auto &[k, v] : expr_attr_values) {
            Aws::DynamoDB::Model::AttributeValue av;
            av.SetS(v); // simplified — real impl handles N, BOOL, L, M, etc.
            req.AddExpressionAttributeValues(k, av);
        }

        if (!start_key.empty()) {
            req.SetExclusiveStartKey(start_key);
        }

        auto outcome = client_->Query(req);
        if (!outcome.IsSuccess()) {
            throw std::runtime_error("Query failed: " +
                                     outcome.GetError().GetMessage());
        }

    	fprintf(stderr, "DYNAMO QUERY: key='%s' → %zu items returned, has_more=%s\n",
				key_condition_expr.c_str(),
				outcome.GetResult().GetItems().size(),
				outcome.GetResult().GetLastEvaluatedKey().empty() ? "no" : "yes");

        DynamoPage page;
        page.items       = outcome.GetResult().GetItems();
        page.next_cursor = outcome.GetResult().GetLastEvaluatedKey();
        return page;
    }

    // ── GetItem — exact PK+SK, cheapest possible read (1 RCU) ─────────────
    DynamoPage GetItem(const TableConfig &config,
                       const std::string &pk_value,
                       const std::string &sk_value,
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
            throw std::runtime_error("GetItem failed: " +
                                     outcome.GetError().GetMessage());
        }

        DynamoPage page;
        auto item = outcome.GetResult().GetItem();
        if (!item.empty()) {
            page.items.push_back(item);
        }
        // next_cursor left empty → signals end of "pagination"
        return page;
    }

private:
    std::unique_ptr<Aws::DynamoDB::DynamoDBClient> client_;

    // Comma-separated projection expression from column names
	static std::string BuildProjection(
		const std::vector<std::string> &cols,
		Aws::Map<Aws::String, Aws::String> &expr_attr_names) {

		std::string expr;
		for (const auto &col : cols) {
			if (col == "_extra") continue;
			if (!expr.empty()) expr += ", ";
			std::string alias = "#proj_" + col;
			expr += alias;
			expr_attr_names[alias] = col;
		}
		return expr;
	}
};

} // namespace duckdb
