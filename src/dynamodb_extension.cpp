// dynamodb_extension.cpp
// Registers both table functions with DuckDB:
//   dynamodb_scan('table', ...)  — typed columns + _extra JSON for rare attrs
//   dynamodb_json('table', ...)  — every row is a raw JSON VARCHAR

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
//       endpoint='http://localhost:8000',   -- DynamoDB Local
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
	cfg.region = get_str("region", "us-east-1");
	cfg.schema_mode = get_str("schema_mode", "hybrid");
	cfg.pk_name = get_str("pk", "");
	cfg.sk_name = get_str("sk", "");
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
	bind_data->config = ParseTableConfig(table_name, input.named_parameters);

	bind_data->aws_client = make_shared_ptr<AWSClientWrapper>(bind_data->config);

	// Auto-discover PK/SK from DescribeTable if not provided by user
	if (bind_data->config.pk_name.empty()) {
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
	const auto &bind_data  = input.bind_data->Cast<DynamoBindData>();
	auto state = make_uniq<DynamoScanState>();

	if (!input.column_ids.empty()) {
		state->projected_col_indices = vector<idx_t>(input.column_ids.begin(), input.column_ids.end());
	} else {
		for (idx_t i = 0; i < bind_data.schema.columns.size(); i++) {
			state->projected_col_indices.push_back(i);
		}
	}

	if (!bind_data.pk_value.empty()) {
		state->key_condition_expr = "#" + bind_data.config.pk_name +
									" = :" + bind_data.config.pk_name + "val";
		state->expr_attr_names["#" + bind_data.config.pk_name] = bind_data.config.pk_name;
		state->expr_attr_values[":" + bind_data.config.pk_name + "val"] = bind_data.pk_value;
		state->operation = DynamoOperation::QUERY;
		state->total_segments = 1;

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

	auto &global = global_p->Cast<DynamoScanState>();
	auto local = make_uniq<DynamoLocalState>();

	if (global.operation == DynamoOperation::SCAN) {
		// Each thread claims the next available segment atomically
		local->current_segment = -1;
		local->segment_done    = false;
		return std::move(local);
	}

	return std::move(local);
}

// ─────────────────────────────────────────────
// SCAN — called repeatedly per thread until done.
// Fills one DataChunk (~2048 rows) per call.
//
// GROUP BY acceleration:
//   DuckDB's vectorised hash-aggregate operator sits above this
//   and ingests chunks in parallel from all scan threads.
//   Each thread independently scans its DynamoDB segment, so
//   GROUP BY across a full scan is automatically parallelised.
//   DuckDB merges partial aggregates from threads at the top.
// ─────────────────────────────────────────────
static void DynamoScanFunction(ClientContext &ctx, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<DynamoBindData>();
	auto &global = input.global_state->Cast<DynamoScanState>();
	auto &local = input.local_state->Cast<DynamoLocalState>();

	// Check if all consumed, check that the local buffer is also emptied
	std::lock_guard<std::mutex> lock(global.cursor_mutex);
	if (global.done && global.operation != DynamoOperation::SCAN) {
		output.SetCardinality(0);
		return;
	}


	auto &aws = *(bind_data.aws_client);

	// Columns DuckDB actually needs (projection pushdown)
	std::vector<std::string> needed_cols;
	for (idx_t ci : global.projected_col_indices) {
		if (ci < bind_data.schema.columns.size()) {
			needed_cols.push_back(bind_data.schema.columns[ci].name);
		}
	}

	DynamoPage page;

	// ── Fetch next page depending on operation ─────────────────────────────
	switch (global.operation) {
	case DynamoOperation::GET_ITEM: {
		// Extract PK and SK values from pushed key expressions
		// (simplified — real impl parses expr_attr_values)
		std::string pk_val = global.expr_attr_values.at(":" + bind_data.config.pk_name + "val");
		std::string sk_val = global.expr_attr_values.at(":" + bind_data.config.sk_name + "val");
		page = aws.GetItem(bind_data.config, pk_val, sk_val, needed_cols);
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
			DynamoPage local_page = aws.Query(bind_data.config, global.key_condition_expr, global.filter_expr, global.index_name,
						 needed_cols, global.expr_attr_values, global.expr_attr_names, cursor);
			local.item_buffer   = std::move(local_page.items);
			local.buffer_offset = 0;
			global.last_evaluated_key = local_page.next_cursor;
			if (local_page.next_cursor.empty()) global.done = true;
		}
		break;
	}

	case DynamoOperation::SCAN: {
		// Each thread independently paginates through its own segment.
		// This is the key to parallel GROUP BY performance:
		//   N threads × independent DynamoDB segments → N×  throughput
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
				local.segment_cursor  = {};
				local.segment_done    = false;
			}

			DynamoPage local_page = aws.Scan(bind_data.config, needed_cols,
									   local.segment_cursor,
									   local.current_segment,
									   global.total_segments);
			local.item_buffer   = std::move(local_page.items);
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
		AppendItemToChunk(local.item_buffer[local.buffer_offset],
						  bind_data.schema,
						  global.projected_col_indices,
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
// REPLACEMENT SCAN
// Allows:  FROM 'dynamodb://my-table'
// instead of: FROM dynamodb_scan('my-table')
// ─────────────────────────────────────────────
/*static unique_ptr<TableRef> DynamoReplacementScan(ClientContext &ctx, ReplacementScanInput &input,
                                                  optional_ptr<ReplacementScanData> data) {
	string table_name = ReplacementScan::GetFullPath(input);
	if (!StringUtil::StartsWith(table_name, "dynamodb://")) {
		return nullptr; // not ours
	}
	auto actual_name = table_name.substr(11); // strip "dynamodb://"
	auto table_func = make_uniq<TableFunctionRef>();
	auto func_name = make_uniq<FunctionExpression>(
		"dynamodb_scan",
		vector<unique_ptr<ParsedExpression>>{}
	);

	func_name->children.push_back(
		make_uniq<ConstantExpression>(Value(actual_name))
	);

	table_func->function = std::move(func_name);
	return std::move(table_func);
}*/

// ─────────────────────────────────────────────
// EXTENSION LOAD
// ─────────────────────────────────────────────
void LoadInternal(ExtensionLoader &loader) {
	Aws::InitAPI({});

	// ── dynamodb_scan — typed columns + optional _extra JSON ──────────────
	TableFunction scan_func("dynamodb_scan", {LogicalType::VARCHAR}, // positional arg: table name
	                        DynamoScanFunction, DynamoBindFunction, DynamoInitGlobal, DynamoInitLocal);

	// Named parameters
	scan_func.named_parameters["endpoint"] = LogicalType::VARCHAR;
	scan_func.named_parameters["region"] = LogicalType::VARCHAR;
	scan_func.named_parameters["pk"] = LogicalType::VARCHAR;
	scan_func.named_parameters["sk"] = LogicalType::VARCHAR;
	scan_func.named_parameters["allow_full_scan"] = LogicalType::BOOLEAN;
	scan_func.named_parameters["parallel_segments"] = LogicalType::INTEGER;
	scan_func.named_parameters["sample_size"] = LogicalType::INTEGER;
	scan_func.named_parameters["schema_mode"] = LogicalType::VARCHAR;
	scan_func.named_parameters["hybrid_threshold"] = LogicalType::DOUBLE;

	// Optimisation flags
	scan_func.filter_prune = true;
	scan_func.projection_pushdown = true;
	scan_func.pushdown_complex_filter = [](ClientContext &ctx, LogicalGet &get,
	FunctionData *bind_data_p, vector<unique_ptr<Expression>> &filters) {

		auto &bind_data = bind_data_p->Cast<DynamoBindData>();
		vector<unique_ptr<Expression>> remaining;

		for (auto &filter : filters) {
			if (IsPKEqualityFilter(filter, bind_data.config)) {
				bind_data.pk_value = ExtractPKValue(filter);} else {
				// Keep for DuckDB to apply vectorized
				remaining.push_back(std::move(filter));
			}
		}
		filters = std::move(remaining);
	};


	loader.RegisterFunction(scan_func);

	// ── dynamodb_json — raw JSON per row, no schema inference ─────────────
	// Usage: SELECT raw->>'$.amount' FROM dynamodb_json('orders')
	TableFunction json_func("dynamodb_json", {LogicalType::VARCHAR},
	                        DynamoScanFunction, // same scan logic
	                        DynamoBindFunction, // bind forces schema_mode="json"
	                        DynamoInitGlobal, DynamoInitLocal);

	json_func.named_parameters = scan_func.named_parameters;
	json_func.filter_pushdown = true;
	json_func.projection_pushdown = false; // JSON mode: always fetch full item

	loader.RegisterFunction(json_func);

	// ── Replacement scan: FROM 'dynamodb://table-name' ────────────────────
	// @todo fix this replacement usage
	// db.instance->config.replacement_scans.emplace_back(DynamoReplacementScan);
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
