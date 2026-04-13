#pragma once

#include "duckdb.hpp"

#include <memory>
#include <string>
#include <vector>

namespace duckdb {
class AWSClientWrapper;
enum class DynamoOperation;

// ─────────────────────────────────────────────
// GSI metadata declared by user or auto-detected
// ─────────────────────────────────────────────
struct GSIConfig {
	std::string index_name;
	std::string pk_name;
	std::string sk_name; // optional
};

// ─────────────────────────────────────────────
// Column schema inferred from sampling
// ─────────────────────────────────────────────
struct ColumnInfo {
	std::string name;
	LogicalType duckdb_type;
	bool always_present; // false → may be NULL in some rows → goes into _extra
};

struct SchemaInfo {
	std::vector<ColumnInfo> columns;      // becomes real DuckDB columns
	std::vector<std::string> extra_attrs; // rare attrs serialised into _extra JSON
};

// ─────────────────────────────────────────────
// Per-table configuration
// ─────────────────────────────────────────────
struct TableConfig {
	std::string table_name;
	std::string pk_name;
	std::string sk_name; // empty if table has no sort key
	std::vector<GSIConfig> gsis;
	bool allow_full_scan = false;
	int parallel_segments = 4; // parallelism for full scans
	int sample_size = 200;     // items sampled for schema inference
	std::string endpoint_url;  // e.g. http://localhost:8000 for DynamoDB Local
	std::string region = "us-east-1";
	// "infer"  → union schema from sample, NULLs for missing attrs
	// "json"   → single raw JSON column per row (dynamodb_json function)
	// "hybrid" → real columns for common attrs + _extra JSON for the rest
	std::string schema_mode = "hybrid";
	double hybrid_threshold = 0.8; // attr must appear in >80% of samples to be a column
};

// ─────────────────────────────────────────────
// Bind data — lives from planning through execution
// ─────────────────────────────────────────────
struct DynamoBindData : FunctionData {
	TableConfig config;
	SchemaInfo schema;
	vector<column_t> projected_col_indices;
	std::string pk_value;

	shared_ptr<AWSClientWrapper> aws_client;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DynamoBindData>(*this);
	}
	bool Equals(const FunctionData &) const override {
		return false;
	}
};

// ─────────────────────────────────────────────
// Extension entry point
// ─────────────────────────────────────────────
class DynamodbExtension : public Extension {
public:
	void Load(ExtensionLoader &db) override;
	std::string Name() override;
};

} // namespace duckdb
