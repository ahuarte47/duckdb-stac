#include "stac_data_functions.hpp"
#include "function_builder.hpp"

// DuckDB
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"

// STAC
#include "http_request.hpp"

// Debug logging controlled by STAC_DEBUG environment variable
static int GetDebugLevel() {
	static int level = -1;
	if (level == -1) {
		const char *env = std::getenv("STAC_DEBUG");
		level = env ? std::atoi(env) : 0;
	}
	return level;
}

#define STAC_SCAN_DEBUG_LOG(level, fmt, ...)                                                                           \
	do {                                                                                                               \
		if (GetDebugLevel() >= level) {                                                                                \
			fprintf(stderr, "STAC: " fmt "\n", ##__VA_ARGS__);                                                         \
		}                                                                                                              \
	} while (0)

namespace duckdb {

namespace {

//======================================================================================================================
// STAC_Read
//======================================================================================================================

struct STAC_Read {
	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------

	struct BindData final : TableFunctionData {
		std::size_t limit = 0;

		explicit BindData() {
		}
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<string> &names) {
		D_ASSERT(input.inputs.size() == 1);

		return unique_ptr<FunctionData>(new BindData());
	}

	//------------------------------------------------------------------------------------------------------------------
	// Init
	//------------------------------------------------------------------------------------------------------------------

	struct State final : GlobalTableFunctionState {
		idx_t current_row;

		explicit State() : current_row(0) {
		}
	};

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
		auto &bind_data = input.bind_data->Cast<BindData>();
		auto global_state = make_uniq_base<GlobalTableFunctionState, State>();
		auto &data_table = global_state->Cast<State>();

		return global_state;
	}

	//------------------------------------------------------------------------------------------------------------------
	// Optimize (Only LIMIT pushdown is implemented)
	//------------------------------------------------------------------------------------------------------------------

	static void Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op) {
		// Apply optimizations on the LogicalPlan

		if (op->type == LogicalOperatorType::LOGICAL_LIMIT) {
			auto &limit = op->Cast<LogicalLimit>();

			// Only push down simple LIMIT without OFFSET, ORDER BY or GROUP BY, and with a constant value,
			// as it would change the result of the query.
			if (limit.limit_val.Type() != LimitNodeType::CONSTANT_VALUE) {
				return;
			}
			if (limit.offset_val.Type() != LimitNodeType::UNSET) {
				return;
			}
			for (const auto &child : op->children) {
				if (child->type == LogicalOperatorType::LOGICAL_ORDER_BY) {
					return;
				}
				if (child->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
					return;
				}
				if (child->type == LogicalOperatorType::LOGICAL_GET) {
					auto &get = child->Cast<LogicalGet>();

					if (StringUtil::Lower(get.function.name) == "stac_read") {
						auto &bind_data = get.bind_data->Cast<BindData>();
						bind_data.limit = limit.limit_val.GetConstantValue();
						STAC_SCAN_DEBUG_LOG(1, "LIMIT pushdown: %zu", bind_data.limit);
						return;
					}
				}
			}
		}

		// Recurse into children
		for (auto &child : op->children) {
			Optimize(input, child);
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute
	//------------------------------------------------------------------------------------------------------------------

	static void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
		auto &gstate = input.global_state->Cast<State>();

		output.SetCardinality(0);
		return;
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------

	static constexpr auto DESCRIPTION = R"(
		Reads the content of a SpatioTemporal Asset Catalog (STAC) catalog from the given URL or JSON file
		and returns it as a table.

		This function exposes a STAC catalog as a relational table, following the
		[GeoParquet STAC specification](https://radiantearth.github.io/stac-geoparquet-spec/latest/).

		Each row represents a single STAC item. Almost all item fields are mapped to columns;
		nested JSON structures are preserved as Parquet structs where possible, but item properties
		are promoted to the top level for easier filtering and querying.
	)";

	static constexpr auto EXAMPLE = R"(
		SELECT * FROM STAC_Read('https://example.com/stac/collection.json');
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Complex Filter Pushdown
	//------------------------------------------------------------------------------------------------------------------

	static void PushdownComplexFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
	                                  vector<unique_ptr<Expression>> &expressions) {
		auto &bind_data = bind_data_p->Cast<BindData>();
	}

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------

	static void Register(ExtensionLoader &loader) {
		InsertionOrderPreservingMap<string> tags;
		tags.insert("ext", "stac");
		tags.insert("category", "table");

		TableFunction func("STAC_Read", {LogicalType::VARCHAR}, Execute, Bind, Init);

		// Enable projection pushdown - allows DuckDB to tell us which columns are needed
		// The column_ids will be passed to InitGlobal via TableFunctionInitInput
		func.projection_pushdown = true;

		// Enable complex filter pushdown - handles expressions like (A AND B) OR (C AND D)
		// that cannot be represented as simple TableFilter objects
		func.pushdown_complex_filter = PushdownComplexFilter;

		RegisterFunction<TableFunction>(loader, func, CatalogType::TABLE_FUNCTION_ENTRY, DESCRIPTION, EXAMPLE, tags);

		// Register optimizer extension for LIMIT pushdown
		auto &db = loader.GetDatabaseInstance();
		auto &config = DBConfig::GetConfig(db);
		OptimizerExtension stac_optimizer;
		stac_optimizer.optimize_function = STAC_Read::Optimize;
		OptimizerExtension::Register(config, std::move(stac_optimizer));
	}
};

} // namespace

// #####################################################################################################################
// Register Data Functions
// #####################################################################################################################

void STACDataFunctions::Register(ExtensionLoader &loader) {
	STAC_Read::Register(loader);
}

} // namespace duckdb
