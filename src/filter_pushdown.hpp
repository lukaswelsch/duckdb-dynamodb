// filter_pushdown.hpp
// Translates DuckDB filter expressions into DynamoDB
// KeyConditionExpression / FilterExpression strings.
//
// Returns the subset of filters that were NOT pushed (DuckDB handles those).

#pragma once

#include "dynamodb_extension.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"

namespace duckdb {

// ─────────────────────────────────────────────
// Result of attempting to push down a filter
// ─────────────────────────────────────────────
enum class PushdownResult {
    PUSHED_PK,   // became a KeyConditionExpression on the primary key
    PUSHED_SK,   // became a KeyConditionExpression on the sort key
    PUSHED_GSI,  // became a KeyConditionExpression on a GSI key
    PUSHED_FILTER, // became a FilterExpression (server-side, costs RCUs)
    NOT_PUSHED,  // DuckDB keeps this one and applies it after scan
};

// ─────────────────────────────────────────────
// Check if a column name is the table PK or SK
// ─────────────────────────────────────────────
static bool IsPKColumn(const std::string &col, const TableConfig &config) {
    return col == config.pk_name;
}

static bool IsSKColumn(const std::string &col, const TableConfig &config) {
    return !config.sk_name.empty() && col == config.sk_name;
}

// Returns the matching GSI config if the column is a GSI partition key
static const GSIConfig *MatchGSI(const std::string &col, const TableConfig &config) {
    for (auto &gsi : config.gsis) {
        if (gsi.pk_name == col) return &gsi;
    }
    return nullptr;
}

// ─────────────────────────────────────────────
// Convert DuckDB expression comparison type to DynamoDB operator string
// ─────────────────────────────────────────────
static std::string ComparisonToExpr(ExpressionType type,
                                    const std::string &attr_ref,   // e.g. "#pk"
                                    const std::string &val_ref) {  // e.g. ":pkval"
    switch (type) {
    case ExpressionType::COMPARE_EQUAL:
        return attr_ref + " = " + val_ref;
    case ExpressionType::COMPARE_LESSTHAN:
        return attr_ref + " < " + val_ref;
    case ExpressionType::COMPARE_LESSTHANOREQUALTO:
        return attr_ref + " <= " + val_ref;
    case ExpressionType::COMPARE_GREATERTHAN:
        return attr_ref + " > " + val_ref;
    case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
        return attr_ref + " >= " + val_ref;
    default:
        return ""; // unsupported → not pushed
    }
}

// ─────────────────────────────────────────────
// Core pushdown logic — processes one filter at a time.
//
// DuckDB calls this via the filter_pushdown callback.
// We accumulate pushed expressions into bind_data and return
// the filters DuckDB should keep (the unpushed ones).
// ─────────────────────────────────────────────
PushdownResult TryPushFilter(const std::string &col_name,
                             const TableFilter &filter,
                             DynamoBindData &bind_data) {

    auto &config = bind_data.config;

    // Only ConstantFilter is safe to push (column OP constant)
    // ConjunctionFilter (AND/OR) is handled recursively by DuckDB
    if (filter.filter_type != TableFilterType::CONSTANT_COMPARISON) {
        return PushdownResult::NOT_PUSHED;
    }

    auto &cf = (const ConstantFilter &)filter;

    // Build safe placeholder names to avoid DynamoDB reserved word conflicts
    // e.g. "status" is a reserved word in DynamoDB → use "#status" + ExpressionAttributeNames
    std::string attr_ref = "#" + col_name;
    std::string val_ref  = ":" + col_name + "val";

    std::string expr = ComparisonToExpr(cf.comparison_type, attr_ref, val_ref);
    if (expr.empty()) return PushdownResult::NOT_PUSHED;

    // Register the attribute name alias (handles reserved words automatically)
    bind_data.expr_attr_names[attr_ref] = col_name;
    // Register the value placeholder
    bind_data.expr_attr_values[val_ref] = cf.constant.ToString();

    // ── PK exact match → KeyConditionExpression (mandatory for Query) ──────
    if (IsPKColumn(col_name, config) &&
        cf.comparison_type == ExpressionType::COMPARE_EQUAL) {
        bind_data.key_condition_expr = expr;
        return PushdownResult::PUSHED_PK;
    }

    // ── SK range or equality → appended to KeyConditionExpression ──────────
    if (IsSKColumn(col_name, config)) {
        if (!bind_data.key_condition_expr.empty()) {
            bind_data.key_condition_expr += " AND " + expr;
        }
        // If PK hasn't been pushed yet this becomes a FilterExpression instead
        // (you can't query on SK alone without PK in DynamoDB)
        return PushdownResult::PUSHED_SK;
    }

    // ── GSI partition key match → route to GSI ─────────────────────────────
    if (cf.comparison_type == ExpressionType::COMPARE_EQUAL) {
        const GSIConfig *gsi = MatchGSI(col_name, config);
        if (gsi) {
            bind_data.index_name      = gsi->index_name;
            bind_data.key_condition_expr = expr;
            return PushdownResult::PUSHED_GSI;
        }
    }

    // ── Non-key attribute → DynamoDB FilterExpression ──────────────────────
    // Note: DynamoDB applies this AFTER reading items, so it reduces network
    // payload but does NOT reduce consumed RCUs. DuckDB can do this too but
    // pushing it here saves bandwidth.
    if (!bind_data.filter_expr.empty()) {
        bind_data.filter_expr += " AND " + expr;
    } else {
        bind_data.filter_expr = expr;
    }
    return PushdownResult::PUSHED_FILTER;
}

// ─────────────────────────────────────────────
// Inspect all filters and decide the best DynamoDB operation.
//
// Returns the operation type and leaves unpushed filters for DuckDB.
// Called from DynamoInitFunction after the planner has handed us filters.
// ─────────────────────────────────────────────
DynamoOperation ResolveBestOperation(DynamoBindData &bind_data,
                                     const TableFilterSet *filters,
                                     ClientContext &ctx) {

    if (!filters || filters->filters.empty()) {
        // No predicates at all → must full scan
        if (!bind_data.config.allow_full_scan) {
            throw InvalidInputException(
                "Query on '%s' requires a full table scan. "
                "Pass allow_full_scan=true to proceed (expensive!).",
                bind_data.config.table_name);
        }
        return DynamoOperation::SCAN;
    }

    bool has_pk  = false;
    bool has_gsi = false;

    for (auto &[col_idx, filter] : filters->filters) {
        // Resolve column index → column name using schema
        std::string col_name = bind_data.schema.columns[col_idx].name;
        PushdownResult r = TryPushFilter(col_name, *filter, bind_data);

        if (r == PushdownResult::PUSHED_PK)  has_pk  = true;
        if (r == PushdownResult::PUSHED_GSI) has_gsi = true;
    }

    if (has_pk) {
        // Check if we also have an SK for a possible GetItem
        bool has_sk = !bind_data.config.sk_name.empty() &&
                      bind_data.key_condition_expr.find("#" + bind_data.config.sk_name) != std::string::npos;

        // GetItem is only possible for exact PK+SK equality (both present)
        // If we have PK=val AND SK=val → GetItem (cheapest)
        // Otherwise → Query on PK (still cheap, reads partition only)
        if (has_sk) return DynamoOperation::GET_ITEM;
        return DynamoOperation::QUERY;
    }

    if (has_gsi) return DynamoOperation::QUERY_GSI;

    // We might have only non-key filters pushed as FilterExpression.
    // Still requires a full scan but at least we reduce bandwidth.
    if (!bind_data.config.allow_full_scan) {
        throw InvalidInputException(
            "No key condition found for table '%s'. "
            "This would require a full scan. Set allow_full_scan=true.",
            bind_data.config.table_name);
    }

    return DynamoOperation::SCAN;
}

static void DynamoProjectionPushdown(ClientContext &ctx,
									  LogicalGet &get,
									  FunctionData *bind_data_p,
									  vector<idx_t> &column_ids) {
	auto &bind_data = bind_data_p->Cast<DynamoBindData>();
	bind_data.projected_col_indices = column_ids;  // DuckDB tells us which cols it needs
}

} // namespace duckdb
