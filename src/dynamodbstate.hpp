//
// Created by Lukas Welsch on 10.04.26.
//

#include "duckdb/function/table_function.hpp"
#include <aws/core/Aws.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/model/QueryRequest.h>
#include <mutex>


namespace duckdb {
// ─────────────────────────────────────────────
// Scan operation decided at init time
// ─────────────────────────────────────────────
enum class DynamoOperation {
	GET_ITEM,   // exact PK+SK lookup → 1 RCU
	QUERY,      // PK (+ optional SK range) → partition scan
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

	// Parallel scan bookkeeping (SCAN mode only)
	int total_segments = 1;
	std::atomic<int> next_segment{0}; // threads claim segments atomically

	// For QUERY/QUERY_GSI: single pagination cursor (single thread)
	Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> last_evaluated_key;
	std::mutex cursor_mutex;

	idx_t MaxThreads() const override { return (idx_t)total_segments; }
};

// ─────────────────────────────────────────────
// Local scan state — one per thread
// ─────────────────────────────────────────────
struct DynamoLocalState : duckdb::LocalTableFunctionState {
	// Buffered items from the last fetched page not yet emitted to DuckDB
	std::vector<Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>> item_buffer;
	idx_t buffer_offset = 0;

	// Which segment this thread owns (SCAN mode)
	int my_segment = -1;
	Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> segment_cursor;
	bool segment_done = false;
};

