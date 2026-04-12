#include <aws/core/Aws.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/model/QueryRequest.h>
#include <mutex>


namespace duckdb {

enum class DynamoOperation {
	GET_ITEM,   // exact PK+SK lookup
	QUERY,      // PK (+ optional SK range)
	QUERY_GSI,  // Query routed through a GSI
	SCAN,       // full table scan — requires allow_full_scan=true
};

}
// ─────────────────────────────────────────────
// Global scan state — shared across threads
// ─────────────────────────────────────────────
struct DynamoScanState : duckdb::GlobalTableFunctionState {
	duckdb::DynamoOperation operation;
	bool done = false;

	duckdb::vector<unsigned long long> projected_col_indices;

	// Parallel scan bookkeeping (SCAN mode only)
	int total_segments = 1;
	std::atomic<int> next_segment{0}; // threads claim segments atomically

	// For QUERY/QUERY_GSI: single pagination cursor (single thread)
	Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> last_evaluated_key;
	std::mutex cursor_mutex;

	std::string key_condition_expr;
	std::string filter_expr;
	std::string index_name;
	std::unordered_map<std::string, std::string> expr_attr_names;
	std::unordered_map<std::string, std::string> expr_attr_values;

	std::string pk_value;

	idx_t MaxThreads() const override { return (idx_t)total_segments; }
};

// ─────────────────────────────────────────────
// Local scan state — one per thread
// ─────────────────────────────────────────────
struct DynamoLocalState : duckdb::LocalTableFunctionState {
	// Buffered items from the last fetched page not yet emitted to DuckDB
	std::vector<Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>> item_buffer;
	idx_t buffer_offset = 0;

	Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> segment_cursor;
	int current_segment = -1;
	bool segment_done = false;
};

