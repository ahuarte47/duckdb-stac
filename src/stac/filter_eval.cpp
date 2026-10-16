#include "filter_eval.hpp"

// DuckDB
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/expression_executor_state.hpp"

namespace duckdb {

//======================================================================================================================
// FilterEval
//======================================================================================================================

ExpressionExecutor *FilterEval::GetExecutor() {
	if (!executor) {
		vector<LogicalType> data_types;

		// Build a 1-row DataChunk with the projected columns populated from the ItemRow.
		for (idx_t i = 0; i < column_ids.size(); i++) {
			const column_t table_col = column_ids[i];
			data_types.emplace_back(column_types[table_col]);
		}

		// Initialize the input chunk with the column values from the ItemRow.
		input.Initialize(context, data_types, 1);
		input.SetCardinality(1);

		// Prepare a chunk to hold the results of expression evaluation.
		result.Initialize(context, vector<LogicalType>(expressions.size(), LogicalType::BOOLEAN), 1);

		// Create the expression executor and add the filter expressions to it.

		executor = make_uniq<ExpressionExecutor>(context);

		for (const auto &expr : expressions) {
			executor->AddExpression(*expr);
		}
	}
	return executor.get();
}

bool FilterEval::Eval(const ItemRow &row) {
	// If there are no filter expressions, the row passes the filter by default.
	if (expressions.empty()) {
		return true;
	}

	// Evaluate the filter!
	try {
		ExpressionExecutor *executor = GetExecutor();

		for (idx_t i = 0; i < column_ids.size(); i++) {
			const column_t table_col = column_ids[i];
			input.data[i].SetValue(0, row.ValueOf(table_col));
		}

		executor->Execute(input, result);

		for (idx_t i = 0; i < expressions.size(); i++) {
			const Value val = result.GetValue(i, 0);

			if (val.IsNull() || !BooleanValue::Get(val)) {
				return false;
			}
		}
	} catch (const std::exception &ex) {
		STAC_SCAN_DEBUG_LOG(1, "Eval: exception during expression execution, Exception: %s", ex.what());

		// Defer to runtime evaluation on exception
		return true;
	}
	return true;
}

} // namespace duckdb
