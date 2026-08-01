#pragma once

#include "stac_types.hpp"

// DuckDB
#include "duckdb.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

/**
 * Context for expression evaluation on a STAC item.
 */
class FilterContext {
public:
	FilterContext(ClientContext &context, const vector<std::unique_ptr<Expression>> &expressions,
	              const vector<column_t> &column_ids, const vector<LogicalType> &column_types)
	    : context(context), expressions(expressions), column_ids(column_ids), column_types(column_types) {
	}

public:
	//! The client context for expression execution.
	ClientContext &context;
	//! The set of filter expressions to evaluate.
	const vector<std::unique_ptr<Expression>> &expressions;
	//! The map column indices to table columns.
	const vector<column_t> &column_ids;
	//! The types of the columns.
	const vector<LogicalType> &column_types;
};

/**
 * Evaluates filter expressions on a STAC item.
 */
class FilterEval {
public:
	/**
	 * Evaluate a set of DuckDB expressions on a STAC item.
	 *
	 * Returns True if the filter was successfully evaluated,
	 * False means we must skip the current tile in the table scan.
	 */
	static bool Eval(const ItemRow &row, const FilterContext &ctx);
};

} // namespace duckdb
