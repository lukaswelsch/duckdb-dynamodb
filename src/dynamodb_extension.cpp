#include "dynamodb_extension.hpp"
#include "dynamodbstate.hpp"

#include "aws_client.hpp"
#include "schema_inference.hpp"
#include "filter_pushdown.hpp"

#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"

#include <aws/core/Aws.h>

namespace duckdb {

// ─────────────────────────────────────────────
// Parse named parameters from the SQL call.
//
// Usage:
//   dynamodb_scan('orders',
//       endpoint='http://localhost:8000',
//       allow_full_scan=true,
//       schema_mode='hybrid',
//       parallel_segments=8)
// ─────────────────────────────────────────────
static TableConfig ParseTableConfig(const std::string &table_name, const named_parameter_map_t &params) {
	TableConfig cfg;
	cfg.table_name = table_name;

	auto get_str = [&](const std::string &key, const std::string &def) -> std::string {
		auto it = params.find(key);
		return (it != params.end()) ? it->second.GetValue<std::string>() : def;
	};
	auto get_bool = [&](const std::string &key, bool def) -> bool {
		auto it = params.find(key);
		return (it != params.end()) ? it->second.GetValue<bool>() : def;
	};
	auto get_int = [&](const std::string &key, int def) -> int {
		auto it = params.find(key);
		return (it != params.end()) ? (int)it->second.GetValue<int64_t>() : def;
	};

	cfg.endpoint_url = get_str("endpoint", "");
	cfg.schema_mode = get_str("schema_mode", "hybrid");
	cfg.allow_full_scan = get_bool("allow_full_scan", false);
	cfg.parallel_segments = get_int("parallel_segments", 4);
	cfg.sample_size = get_int("sample_size", 200);
	cfg.hybrid_threshold =
	    get_str("hybrid_threshold", "0.8").empty() ? 0.8 : std::stod(get_str("hybrid_threshold", "0.8"));

	return cfg;
}

// ─────────────────────────────────────────────
// BIND — runs once at query planning time.
// Determines schema and registers output columns with DuckDB.
// ─────────────────────────────────────────────
static unique_ptr<FunctionData> DynamoBindFunction(ClientContext &ctx, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	auto table_name = input.inputs[0].GetValue<std::string>();
	auto bind_data = make_uniq<DynamoBindData>();

	auto table_config = ParseTableConfig(table_name, input.named_parameters);
	;
	bind_data->config = ParseTableConfig(table_name, input.named_parameters);
	bind_data->secret_config = LoadDynamoSecret(ctx, table_config.secret_name);

	bind_data->aws_client = make_shared_ptr<AWSClientWrapper>(bind_data->config, bind_data->secret_config);

	// Auto-discover PK/SK from DescribeTable if not provided by user
	auto desc = bind_data->aws_client->DescribeTable(table_name);
	auto &key_schema = desc.GetTable().GetKeySchema();
	for (auto &key : key_schema) {
		if (key.GetKeyType() == Aws::DynamoDB::Model::KeyType::HASH) {
			bind_data->config.pk_name = key.GetAttributeName();
		} else {
			bind_data->config.sk_name = key.GetAttributeName();
		}
	}
	// Also load GSI definitions
	for (auto &gsi : desc.GetTable().GetGlobalSecondaryIndexes()) {
		GSIConfig g;
		g.index_name = gsi.GetIndexName();
		for (auto &k : gsi.GetKeySchema()) {
			if (k.GetKeyType() == Aws::DynamoDB::Model::KeyType::HASH) {
				g.pk_name = k.GetAttributeName();
			} else {
				g.sk_name = k.GetAttributeName();
			}
		}
		bind_data->config.gsis.push_back(g);
	}

	// Infer or construct schema
	bind_data->schema = InferSchema(*(bind_data->aws_client), bind_data->config);

	for (auto &col : bind_data->schema.columns) {
		names.push_back(col.name);
		return_types.push_back(col.duckdb_type);
	}

	for (idx_t i = 0; i < bind_data->schema.columns.size(); i++) {
		bind_data->projected_col_indices.push_back(i);
	}

	return std::move(bind_data);
}

// ─────────────────────────────────────────────
// INIT — runs once before scanning starts.
// Decides QUERY vs SCAN and sets up pagination state.
// ─────────────────────────────────────────────
static unique_ptr<GlobalTableFunctionState> DynamoInitGlobal(ClientContext &ctx, TableFunctionInitInput &input) {
	const auto &bind_data = input.bind_data->Cast<DynamoBindData>();
	auto state = make_uniq<DynamoScanState>();

	if (!input.column_ids.empty()) {
		state->projected_col_indices = vector<column_t>(input.column_ids.begin(), input.column_ids.end());
	} else {
		for (idx_t i = 0; i < bind_data.schema.columns.size(); i++) {
			state->projected_col_indices.push_back(i);
		}
	}

	if (!bind_data.pk_value.empty()) {
		state->key_condition_expr = "#" + bind_data.config.pk_name + " = :" + bind_data.config.pk_name + "val";
		state->expr_attr_names["#" + bind_data.config.pk_name] = bind_data.config.pk_name;
		state->expr_attr_values[":" + bind_data.config.pk_name + "val"] = bind_data.pk_value;
		state->operation = DynamoOperation::QUERY;
		state->total_segments = 1;

		return std::move(state);
	}

	if (!bind_data.config.gsis.empty() && !bind_data.config.gsis[0].gsi_value.empty()) {
		auto &gsi = bind_data.config.gsis[0];
		state->key_condition_expr = "#" + gsi.pk_name + " = :" + gsi.pk_name + "val";
		state->expr_attr_names["#" + gsi.pk_name] = gsi.pk_name;
		state->expr_attr_values[":" + gsi.pk_name + "val"] = gsi.gsi_value;
		state->operation = DynamoOperation::QUERY_GSI;
		state->total_segments = 1;
		state->index_name = gsi.index_name;

		return std::move(state);
	}

	// Inspect filters the DuckDB planner wants applied
	DynamoOperation op = ResolveBestOperation(bind_data, *state, input.filters.get(), ctx);

	state->operation = op;

	if (op == DynamoOperation::SCAN) {
		state->total_segments = bind_data.config.parallel_segments;
		Printer::Print("⚠  DynamoDB: full table scan on '" + bind_data.config.table_name +
		               "' — this consumes RCUs proportional "
		               "to the entire table size.\n");
	} else {
		state->total_segments = 1;
	}

	return std::move(state);
}

// Per-thread local state initialisation
static unique_ptr<LocalTableFunctionState> DynamoInitLocal(ExecutionContext &ctx, TableFunctionInitInput &input,
                                                           GlobalTableFunctionState *global_p) {
	const auto &bind_data = input.bind_data->Cast<DynamoBindData>();
	auto &global = global_p->Cast<DynamoScanState>();
	auto local = make_uniq<DynamoLocalState>();

	local->aws_client = make_uniq<AWSClientWrapper>(bind_data.config, bind_data.secret_config);

	if (global.operation == DynamoOperation::SCAN) {
		// Each thread claims the next available segment atomically
		local->current_segment = -1;
		local->segment_done = false;
		return std::move(local);
	}

	return std::move(local);
}

// ─────────────────────────────────────────────
// SCAN — called repeatedly per thread until done.
// Fills one DataChunk (~2048 rows) per call.
// ─────────────────────────────────────────────
static void DynamoScanFunction(ClientContext &ctx, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<DynamoBindData>();
	auto &global = input.global_state->Cast<DynamoScanState>();
	auto &local = input.local_state->Cast<DynamoLocalState>();

	// Check if all consumed, check that the local buffer is also emptied
	if (global.operation != DynamoOperation::SCAN) {
		std::lock_guard<std::mutex> lock(global.cursor_mutex);
		if (global.done && local.buffer_offset >= local.item_buffer.size()) {
			output.SetCardinality(0);
			return;
		}
	}

	auto &aws = *(local.aws_client);

	// Columns DuckDB actually needs (projection pushdown)
	std::vector<std::string> needed_cols;
	for (idx_t ci : global.projected_col_indices) {
		if (ci < bind_data.schema.columns.size()) {
			needed_cols.push_back(bind_data.schema.columns[ci].name);
		}
	}

	// ── Fetch next page depending on operation ─────────────────────────────
	// @todo check why this function isn't called
	switch (global.operation) {
	case DynamoOperation::GET_ITEM: {
		// Extract PK and SK values from pushed key expressions
		std::string pk_val = global.expr_attr_values.at(":" + bind_data.config.pk_name + "val");
		std::string sk_val = global.expr_attr_values.at(":" + bind_data.config.sk_name + "val");
		DynamoPage local_page = aws.GetItem(bind_data.config, pk_val, sk_val, needed_cols);
		local.item_buffer = std::move(local_page.items);
		local.segment_done = true; // GetItem is always a single result
		break;
	}

	case DynamoOperation::QUERY:
	case DynamoOperation::QUERY_GSI: {
		if (local.buffer_offset >= local.item_buffer.size()) {
			Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> cursor;
			std::lock_guard<std::mutex> lock(global.cursor_mutex);
			if (global.done) {
				output.SetCardinality(0);
				return;
			}
			cursor = global.last_evaluated_key;
			DynamoPage local_page = aws.Query(bind_data.config, global.key_condition_expr, global.index_name,
			                                  needed_cols, global.expr_attr_values, global.expr_attr_names, cursor);
			local.item_buffer = std::move(local_page.items);
			local.buffer_offset = 0;
			global.last_evaluated_key = local_page.next_cursor;
			if (local_page.next_cursor.empty())
				global.done = true;
		}
		break;
	}

	case DynamoOperation::SCAN: {
		// Each thread independently paginates through its own segment.
		// This is the key to parallel GROUP BY performance:
		//   N threads × independent DynamoDB segments -> N×  throughput
		while (local.buffer_offset >= local.item_buffer.size()) {
			// Current segment exhausted — claim the next one
			if (local.segment_done || local.current_segment == -1) {
				int next = global.next_segment.fetch_add(1);
				if (next >= global.total_segments) {
					// All segments claimed — this thread is done
					output.SetCardinality(0);
					return;
				}
				local.current_segment = next;
				local.segment_cursor = {};
				local.segment_done = false;
			}

			DynamoPage local_page = aws.Scan(bind_data.config, needed_cols, local.segment_cursor, local.current_segment,
			                                 global.total_segments);
			local.item_buffer = std::move(local_page.items);
			local.buffer_offset = 0;
			local.segment_cursor = local_page.next_cursor;
			if (local_page.next_cursor.empty()) {
				local.segment_done = true;
			}
		}
		break;
	}
	}

	// ── Materialise items into DuckDB columnar output ──────────────────────
	idx_t row = 0;
	while (row < STANDARD_VECTOR_SIZE && local.buffer_offset < local.item_buffer.size()) {
		AppendItemToChunk(local.item_buffer[local.buffer_offset], bind_data.schema, global.projected_col_indices,
		                  output, row);
		local.buffer_offset++;
		row++;
	}
	output.SetCardinality(row);

	// Signal EOF when buffer empty AND no more pages
	if (row == 0) {
		output.SetCardinality(0);
	}

	output.SetCardinality(row);
}

// ─────────────────────────────────────────────
// EXTENSION LOAD
// ─────────────────────────────────────────────
void LoadInternal(ExtensionLoader &loader) {
	// ── dynamodb_scan — typed columns + optional _extra JSON ──────────────
	TableFunction scan_func("dynamodb_scan", {LogicalType::VARCHAR}, // positional arg: table name
	                        DynamoScanFunction, DynamoBindFunction, DynamoInitGlobal, DynamoInitLocal);

	// Named parameters
	scan_func.named_parameters["endpoint"] = LogicalType::VARCHAR;
	scan_func.named_parameters["allow_full_scan"] = LogicalType::BOOLEAN;
	scan_func.named_parameters["parallel_segments"] = LogicalType::INTEGER;
	scan_func.named_parameters["sample_size"] = LogicalType::INTEGER;
	scan_func.named_parameters["schema_mode"] = LogicalType::VARCHAR;
	scan_func.named_parameters["hybrid_threshold"] = LogicalType::DOUBLE;

	// Optimisation flags
	scan_func.filter_prune = true;
	scan_func.projection_pushdown = true;
	scan_func.pushdown_complex_filter = [](ClientContext &ctx, LogicalGet &get, FunctionData *bind_data_p,
	                                       vector<unique_ptr<Expression>> &filters) {
		/*
		 *    if pk_filter is set then: return filters without pk_filter
		 *    if we have a gsi filter: return filters without the first gsi_filter
		 *    else return all filters
		 */
		auto &bind_data = bind_data_p->Cast<DynamoBindData>();
		int remove_idx = -1;

		for (int i = 0; i < (int)filters.size(); i++) {
			auto &filter = filters[i];
			if (GetPKColname(filter) == bind_data.config.pk_name) {
				bind_data.pk_value = ExtractPKValue(filter);
				remove_idx = i;
			} else if (IsGSIFilter(filter, bind_data.config)) {
				auto pk_name = GetPKColname(filter);
				auto it = std::find_if(bind_data.config.gsis.begin(), bind_data.config.gsis.end(),
				                       [&pk_name](GSIConfig &gsi) { return gsi.pk_name == pk_name; });
				if (it != bind_data.config.gsis.end()) {
					it->gsi_value = ExtractPKValue(filter);
				}
				if (remove_idx == -1) {
					remove_idx = i;
				}
			}
		}

		if (remove_idx != -1) {
			filters.erase(filters.begin() + remove_idx);
		}
	};

	loader.RegisterFunction(scan_func);

	// Register Secrets for dynamodb
	SecretType secret_type;
	secret_type.name = "dynamodb";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "credential_chain";
	secret_type.extension = "dynamodb";
	loader.RegisterSecretType(secret_type);

	CreateSecretFunction dynamo_secret_fun = {"dynamodb", "credential_chain", CreateDynamoSecret};
	dynamo_secret_fun.named_parameters["access_key_id"] = LogicalType::VARCHAR;
	dynamo_secret_fun.named_parameters["secret_access_key"] = LogicalType::VARCHAR;
	dynamo_secret_fun.named_parameters["session_token"] = LogicalType::VARCHAR;
	dynamo_secret_fun.named_parameters["region"] = LogicalType::VARCHAR;
	dynamo_secret_fun.named_parameters["endpoint_url"] = LogicalType::VARCHAR;
	dynamo_secret_fun.named_parameters["provider"] = LogicalType::VARCHAR;
	loader.RegisterFunction(dynamo_secret_fun);

	Aws::InitAPI({});
}

void DynamodbExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string DynamodbExtension::Name() {
	return "dynamodb";
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(dynamodb, loader) {
	duckdb::LoadInternal(loader);
}
}
