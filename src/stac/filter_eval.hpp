#pragma once

#include "stac_types.hpp"

// DuckDB
#include "duckdb.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

/**
 * Evaluates filter expressions on STAC items.
 */
class FilterEval {
public:
	FilterEval(ClientContext &context, const vector<unique_ptr<Expression>> &expressions,
	           const vector<column_t> &column_ids, const vector<LogicalType> &column_types)
	    : context(context), expressions(expressions), column_ids(column_ids), column_types(column_types) {
	}

private:
	//! The client context for expression execution.
	ClientContext &context;
	//! The set of filter expressions to evaluate.
	const vector<unique_ptr<Expression>> &expressions;
	//! The map column indices to table columns.
	const vector<column_t> &column_ids;
	//! The types of the columns.
	const vector<LogicalType> &column_types;

	//! The expression executor used for evaluating the filter expressions.
	std::unique_ptr<ExpressionExecutor> executor;
	//! The input DataChunk.
	DataChunk input;
	//! The result DataChunk.
	DataChunk result;

	//! Get the expression executor, initializing it if necessary.
	ExpressionExecutor *GetExecutor();

public:
	/**
	 * Evaluate a set of DuckDB expressions on a STAC item.
	 *
	 * Returns True if the filter was successfully evaluated,
	 * False means we must skip the current tile in the table scan.
	 */
	bool Eval(const ItemRow &row);

	//! Returns true if there are any filter expressions to evaluate.
	inline bool HasExpressions() const {
		return !expressions.empty();
	}
};

} // namespace duckdb
