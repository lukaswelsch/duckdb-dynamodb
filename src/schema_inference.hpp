// schema_inference.hpp
// Samples N items from DynamoDB and builds a SchemaInfo:
//   - Attributes present in >= threshold% of items → real DuckDB columns
//   - Rarer attributes → serialised into a "_extra" JSON VARCHAR column
//
// Handles heterogeneous rows (the normal DynamoDB case) gracefully.

#pragma once

#include "dynamodb_extension.hpp"
#include "aws_client.hpp"
#include <aws/dynamodb/model/AttributeValue.h>
#include <unordered_map>
#include <string>

namespace duckdb {

// ─────────────────────────────────────────────
// Map a DynamoDB AttributeValue type tag to a DuckDB LogicalType
// ─────────────────────────────────────────────
LogicalType AttributeTypeToLogical(const Aws::DynamoDB::Model::AttributeValue &av) {
    using AV = Aws::DynamoDB::Model::AttributeValue;

    if (av.GetType() == Aws::DynamoDB::Model::ValueType::STRING)  return LogicalType::VARCHAR;
    if (av.GetType() == Aws::DynamoDB::Model::ValueType::NUMBER)  return LogicalType::DOUBLE;
    if (av.GetType() == Aws::DynamoDB::Model::ValueType::BOOL)    return LogicalType::BOOLEAN;
    if (av.GetType() == Aws::DynamoDB::Model::ValueType::NULLVALUE) return LogicalType::VARCHAR;

    // Lists (L) and Maps (M) → JSON string, DuckDB can shred with -> operator
    //if (av.GetType() == Aws::DynamoDB::Model::ValueType::ARRAY)   return LogicalType::VARCHAR;
    if (av.GetType() == Aws::DynamoDB::Model::ValueType::ATTRIBUTE_MAP) return LogicalType::VARCHAR;

    // Binary sets, string sets, number sets → VARCHAR (comma-joined or JSON)
    return LogicalType::VARCHAR;
}

// ─────────────────────────────────────────────
// Convert a single DynamoDB AttributeValue to a string for the _extra JSON blob.
// Full implementation would produce proper JSON with nested objects/arrays.
// ─────────────────────────────────────────────
std::string AttributeValueToString(const Aws::DynamoDB::Model::AttributeValue &av) {
    switch (av.GetType()) {
    case Aws::DynamoDB::Model::ValueType::STRING:
        return av.GetS();
    case Aws::DynamoDB::Model::ValueType::NUMBER:
        return av.GetN();
    case Aws::DynamoDB::Model::ValueType::BOOL:
        return av.GetBool() ? "true" : "false";
    case Aws::DynamoDB::Model::ValueType::NULLVALUE:
        return "null";
    default:
        // For Maps, Lists, Sets — serialize to JSON recursively (simplified here)
        return "<complex>";
    }
}

// ─────────────────────────────────────────────
// Main schema inference function.
//
// Strategy:
//   1. Scan sample_size items from the table
//   2. Count how often each attribute name appears across all items
//   3. Attributes appearing in >= hybrid_threshold fraction of items
//      become real DuckDB columns (typed)
//   4. The rest are listed in schema.extra_attrs and will be packed
//      into a "_extra" JSON VARCHAR column at scan time
// ─────────────────────────────────────────────
SchemaInfo InferSchema(AWSClientWrapper &aws,
                       const TableConfig &config) {

    // ── "json" mode: skip inference, return a single raw JSON column ──────
    if (config.schema_mode == "json") {
        SchemaInfo s;
        s.columns.push_back({"raw", LogicalType::VARCHAR, true});
        return s;
    }

    // ── Sample items ───────────────────────────────────────────────────────
    // We do a small Scan with no filter just to get a representative sample.
    // For very large tables consider sampling multiple segments.
    DynamoPage sample = aws.Scan(config,
                                 /*projection=*/{}, // fetch everything for schema inference
                                 /*start_key=*/{},
                                 /*segment=*/0,
                                 /*total_segments=*/1);

    auto &items = sample.items;
    if (items.empty()) {
        // Empty table — return a minimal schema
        SchemaInfo s;
        s.columns.push_back({"raw", LogicalType::VARCHAR, false});
        return s;
    }

    int n = std::min((int)items.size(), config.sample_size);

    // ── Count attribute occurrence frequency ──────────────────────────────
    // attr_name → (appearance_count, observed_type)
    std::map<std::string, int> attr_count;
    std::map<std::string, LogicalType> attr_type;

    for (int i = 0; i < n; i++) {
        for (auto &[attr_name, attr_val] : items[i]) {
            attr_count[attr_name]++;
            if (attr_type.find(attr_name) == attr_type.end()) {
                attr_type[attr_name] = AttributeTypeToLogical(attr_val);
            } else {
                // Type conflict resolution: widen to VARCHAR
                // e.g. if one item has "amount" as N and another as S → VARCHAR
                LogicalType observed = AttributeTypeToLogical(attr_val);
                if (attr_type[attr_name] != observed) {
                    attr_type[attr_name] = LogicalType::VARCHAR;
                }
            }
        }
    }

    // ── Partition into columns vs _extra based on frequency threshold ──────
    SchemaInfo schema;
    double threshold = config.hybrid_threshold; // default 0.8 = must appear in 80% of rows

    for (auto &[attr_name, count] : attr_count) {
        double freq = (double)count / n;
        if (config.schema_mode == "infer" || freq >= threshold) {
            // Frequent enough → becomes a real DuckDB column
            ColumnInfo col;
            col.name           = attr_name;
            col.duckdb_type    = attr_type[attr_name];
            col.always_present = (count == n); // true → never NULL in sample
            schema.columns.push_back(col);
        } else {
            // Infrequent → goes into _extra JSON blob
            schema.extra_attrs.push_back(attr_name);
        }
    }

    // In "hybrid" mode, add the _extra catch-all column if there are rare attrs
    if (config.schema_mode == "hybrid" && !schema.extra_attrs.empty()) {
        ColumnInfo extra_col;
        extra_col.name           = "_extra";
        extra_col.duckdb_type    = LogicalType::VARCHAR; // JSON string
        extra_col.always_present = false;
        schema.columns.push_back(extra_col);
    }

    return schema;
}

// ─────────────────────────────────────────────
// Materialise one DynamoDB item into a DuckDB output chunk row.
//
// For each column in the schema:
//   - If the item has the attribute → write the value
//   - If not → write NULL
//   - For "_extra" → serialize all rare attributes as a JSON object
// ─────────────────────────────────────────────
void AppendItemToChunk(
    const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue> &item,
    const SchemaInfo &schema,
    const vector<idx_t> &projected_col_indices,
    DataChunk &output,
    idx_t row) {

	idx_t output_count = MinValue<idx_t>(projected_col_indices.size(), output.ColumnCount());
	for (idx_t out_idx = 0; out_idx < output_count; out_idx++) {
		idx_t schema_idx = projected_col_indices[out_idx];
    	auto &col = schema.columns[schema_idx];
    	auto &vec = output.data[out_idx];

        // ── _extra column: build JSON from rare attributes ─────────────────
        if (col.name == "_extra") {
            std::string json = "{";
            bool first = true;
            for (auto &extra_attr : schema.extra_attrs) {
                auto it = item.find(extra_attr);
                if (it != item.end()) {
                    if (!first) json += ",";
                    json += "\"" + extra_attr + "\":\"" +
                            AttributeValueToString(it->second) + "\"";
                    first = false;
                }
            }
            json += "}";
            if (json == "{}") {
                FlatVector::SetNull(vec, row, true); // no extra attrs on this item
            } else {
                FlatVector::GetData<string_t>(vec)[row] =
                    StringVector::AddString(vec, json);
            }
            continue;
        }

        // ── Regular column ─────────────────────────────────────────────────
        auto it = item.find(col.name);
        if (it == item.end()) {
            FlatVector::SetNull(vec, row, true); // attribute missing on this item
            continue;
        }

        auto &av = it->second;
        switch (col.duckdb_type.id()) {
        case LogicalTypeId::VARCHAR:
            FlatVector::GetData<string_t>(vec)[row] =
                StringVector::AddString(vec, AttributeValueToString(av));
            break;
        case LogicalTypeId::DOUBLE:
            FlatVector::GetData<double>(vec)[row] = std::stod(av.GetN());
            break;
        case LogicalTypeId::BOOLEAN:
            FlatVector::GetData<bool>(vec)[row] = av.GetBool();
            break;
        default:
            FlatVector::GetData<string_t>(vec)[row] =
                StringVector::AddString(vec, AttributeValueToString(av));
            break;
        }
    }
}

} // namespace duckdb
