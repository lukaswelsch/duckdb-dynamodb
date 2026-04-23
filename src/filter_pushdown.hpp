#pragma once

#include "dynamodb_extension.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/null_filter.hpp"

namespace duckdb {

enum class PushdownResult {
	PUSHED_PK,
	PUSHED_SK,
	PUSHED_GSI,
	PUSHED_FILTER,
	NOT_PUSHED,
};

static bool IsPKColumn(const std::string &col, const TableConfig &config) {
	return col == config.pk_name;
}

static bool IsSKColumn(const std::string &col, const TableConfig &config) {
	return !config.sk_name.empty() && col == config.sk_name;
}

// Returns the matching GSI config if the column is a GSI partition key
static const GSIConfig *MatchGSI(const std::string &col, const TableConfig &config) {
	for (auto &gsi : config.gsis) {
		if (gsi.pk_name == col)
			return &gsi;
	}
	return nullptr;
}

// ─────────────────────────────────────────────
// Convert DuckDB expression comparison type to DynamoDB operator string
// ─────────────────────────────────────────────
static std::string ComparisonToExpr(ExpressionType type, const std::string &attr_ref, const std::string &val_ref) {
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
		return "";
	}
}

// ─────────────────────────────────────────────
// Core pushdown logic — processes one filter at a time.
//
// DuckDB calls this via the filter_pushdown callback.
// We accumulate pushed expressions into bind_data and return the filters DuckDB should keep.
// ─────────────────────────────────────────────
PushdownResult TryPushFilter(const std::string &col_name, const TableFilter &filter, const DynamoBindData &bind_data,
                             DynamoScanState &state) {
	auto &config = bind_data.config;

	// Only ConstantFilter is safe to push (column OP constant)
	// ConjunctionFilter (AND/OR) is handled recursively by DuckDB
	if (filter.filter_type != TableFilterType::CONSTANT_COMPARISON) {
		return PushdownResult::NOT_PUSHED;
	}

	auto &cf = (const ConstantFilter &)filter;

	// Build safe placeholder names to avoid DynamoDB reserved word conflicts
	std::string attr_ref = "#" + col_name;
	std::string val_ref = ":" + col_name + "val";

	std::string expr = ComparisonToExpr(cf.comparison_type, attr_ref, val_ref);
	if (expr.empty())
		return PushdownResult::NOT_PUSHED;

	state.expr_attr_names[attr_ref] = col_name;
	state.expr_attr_values[val_ref] = cf.constant.ToString();

	if (IsPKColumn(col_name, config) && cf.comparison_type == ExpressionType::COMPARE_EQUAL) {
		state.key_condition_expr = expr;
		return PushdownResult::PUSHED_PK;
	}

	if (IsSKColumn(col_name, config)) {
		if (!state.key_condition_expr.empty()) {
			state.key_condition_expr += " AND " + expr;
		}
		return PushdownResult::PUSHED_SK;
	}

	if (cf.comparison_type == ExpressionType::COMPARE_EQUAL) {
		fprintf(stderr, "MatchGSI (%s) \n", col_name.c_str());
		const GSIConfig *gsi = MatchGSI(col_name, config);
		if (gsi) {
			state.index_name = gsi->index_name;
			state.key_condition_expr = expr;
			return PushdownResult::PUSHED_GSI;
		}
	}

	return PushdownResult::NOT_PUSHED;
}

// ─────────────────────────────────────────────
// Inspect all filters and decide the best DynamoDB operation.
//
// Returns the operation type and leaves unpushed filters for DuckDB.
// Called from DynamoInitFunction after the planner has handed us filters.
// ─────────────────────────────────────────────
DynamoOperation ResolveBestOperation(const DynamoBindData &bind_data, DynamoScanState &state,
                                     const TableFilterSet *filters, ClientContext &ctx) {
	if (!filters || filters->filters.empty()) {
		if (!bind_data.config.allow_full_scan) {
			throw InvalidInputException("Query on '%s' requires a full table scan. "
			                            "Pass allow_full_scan=true to proceed (expensive!).",
			                            bind_data.config.table_name);
		}
		return DynamoOperation::SCAN;
	}

	bool has_pk = false;
	bool has_gsi = false;

	for (auto &[col_idx, filter] : filters->filters) {
		idx_t schema_idx = col_idx;
		if (col_idx < state.projected_col_indices.size()) {
			schema_idx = state.projected_col_indices[col_idx];
		}

		std::string col_name = bind_data.schema.columns[schema_idx].name;

		PushdownResult r = TryPushFilter(col_name, *filter, bind_data, state);

		if (r == PushdownResult::PUSHED_PK)
			has_pk = true;
		if (r == PushdownResult::PUSHED_GSI)
			has_gsi = true;
	}

	if (has_pk) {
		// Check if we also have an SK for a possible GetItem
		bool has_sk = !bind_data.config.sk_name.empty() &&
		              state.key_condition_expr.find("#" + bind_data.config.sk_name) != std::string::npos;

		// GetItem is only possible for exact PK+SK equality (both present)
		// If we have PK=val AND SK=val use GetItem (cheapest)
		// Otherwise: Query on PK (still cheap, reads partition only)
		if (has_sk)
			return DynamoOperation::GET_ITEM;
		return DynamoOperation::QUERY;
	}

	if (has_gsi)
		return DynamoOperation::QUERY_GSI;

	// We might have only non-key filters pushed as FilterExpression.
	// Still requires a full scan but at least we reduce bandwidth.
	if (!bind_data.config.allow_full_scan) {
		throw InvalidInputException("No key condition found for table '%s'. "
		                            "This would require a full scan. Set allow_full_scan=true.",
		                            bind_data.config.table_name);
	}

	return DynamoOperation::SCAN;
}


static bool isEqualityFilter(const unique_ptr<Expression> &expr) {
	if (expr->GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
		return false;
	}
	auto &cmp = expr->Cast<BoundComparisonExpression>();
	// Left side must be a column reference
	if (cmp.left->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}

	// Right side must be a constant
	if (cmp.right->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
		return false;
	}

	return true;
}

static std::string GetPKColname(const unique_ptr<Expression> &expr) {
	if (isEqualityFilter(expr)) {
		auto &cmp = expr->Cast<BoundComparisonExpression>();
		auto &col_ref = cmp.left->Cast<BoundColumnRefExpression>();

		auto col_name = col_ref.GetName();

		return col_name;
	}

	return "";
}

static bool IsGSIFilter(const unique_ptr<Expression> &expr, const TableConfig &config) {
	if (isEqualityFilter(expr)) {
		auto &cmp = expr->Cast<BoundComparisonExpression>();
		auto &col_ref = cmp.left->Cast<BoundColumnRefExpression>();

		auto col_name = col_ref.GetName();

		// Find GSI value
		bool is_gsi_pk = std::any_of(config.gsis.begin(), config.gsis.end(),
		[&col_name](const GSIConfig& gsi) {
			return gsi.pk_name == col_name;
		});

		// Column must be PK
		return is_gsi_pk;
	}
	return false;

}

// Extract the constant value from a pk = 'value' expression
static std::string ExtractPKValue(const unique_ptr<Expression> &expr) {
	auto &cmp = expr->Cast<BoundComparisonExpression>();
	auto &constant = cmp.right->Cast<BoundConstantExpression>();
	return constant.value.ToString();
}

} // namespace duckdb
