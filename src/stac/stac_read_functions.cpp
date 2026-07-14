#include "stac_types.hpp"
#include "stac_read_functions.hpp"
#include "function_builder.hpp"
#include <cinttypes>
#include <string>
#include <sstream>

// DuckDB
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/path.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "yyjson.hpp"
using namespace duckdb_yyjson; // NOLINT

// STAC
#include "http_request.hpp"
#include "json_geometry.hpp"
#include "json_object.hpp"

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
// Utility Functions
//======================================================================================================================

//! Reads the content of a JSON file and returns it as a string.
static std::string ReadContentOfJsonFile(ClientContext &context, const std::string &file_path, MemoryStream &buffer) {
	OpenFileInfo file(file_path);

	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(file, FileFlags::FILE_FLAGS_READ);
	if (!handle) {
		throw IOException("Failed to open the file '%s'.", file_path.c_str());
	}

	uint64_t file_size = handle->GetFileSize();

	if (file_size == 0) {
		buffer.SetPosition(0);
		buffer.GrowCapacity(2048);

		const char *buffer_ptr = reinterpret_cast<const char *>(buffer.GetData());
		std::ostringstream oss;

		int64_t bytes_read = 0;
		while ((bytes_read = handle->Read(QueryContext(), buffer.GetData(), 2048)) > 0) {
			oss.write(buffer_ptr, bytes_read);
		}

		std::string json_str = oss.str();
		handle.reset();
		return json_str;
	} else {
		buffer.SetPosition(0);
		buffer.GrowCapacity(file_size);

		const char *buffer_ptr = reinterpret_cast<const char *>(buffer.GetData());
		int64_t bytes_read = handle->Read(QueryContext(), buffer.GetData(), file_size);

		std::string json_str = std::string(buffer_ptr, bytes_read);
		handle.reset();
		return json_str;
	}
}

//! Manages the context of parsing a STAC Catalog.
class CatalogContext {
public:
	// The client context for the current query execution.
	ClientContext &client_context;
	// The set of property names encountered so far, to determine if a new property needs to be added to the schema.
	std::map<std::string, int16_t> property_set;
	// The schema of the Catalog currently being parsed.
	std::vector<std::string> column_names;
	std::vector<LogicalType> column_types;
};

static std::size_t ParseContentOfJsonObject(CatalogContext &context, std::string &json_str, std::string &object_path,
                                            std::string catalog_id, std::string collection_id,
                                            std::vector<ItemRow> &rows, MemoryStream &buffer);

//! Parses a STAC JSON links array to extract child items recursively.
static std::size_t ParseContentOfJsonLinks(CatalogContext &context, yyjson_val *links_val,
                                           const std::string &parent_path, std::string catalog_id,
                                           std::string collection_id, std::vector<ItemRow> &rows,
                                           MemoryStream &buffer) {
	std::size_t count = 0;

	yyjson_val *temp_val = nullptr;
	std::size_t links_size = yyjson_arr_size(links_val);
	yyjson_val *link_val = nullptr;
	const char *href_val = nullptr;
	const char *rel_type = nullptr;

	for (std::size_t i = 0; i < links_size; i++) {
		if (yyjson_is_obj(link_val = yyjson_arr_get(links_val, i))) {
			// Check required fields in the link object.

			if (yyjson_is_str(temp_val = yyjson_obj_get(link_val, "href"))) {
				href_val = yyjson_get_str(temp_val);

				if (!href_val || strlen(href_val) == 0) {
					continue; // Skip links without a "href" field.
				}
			}
			if (yyjson_is_str(temp_val = yyjson_obj_get(link_val, "rel"))) {
				rel_type = yyjson_get_str(temp_val);

				if (!rel_type || strlen(rel_type) == 0) {
					continue; // Skip links without a "rel" field.
				}
			}

			// Parse the child JSON item recursively.

			if (strcmp(rel_type, "child") == 0 || strcmp(rel_type, "item") == 0) {
				std::string href = std::string(href_val);

				// Is the href a relative path? If so, resolve it relative to the object path.
				auto href_path = Path::FromString(href);
				if (!href_path.IsAbsolute() && !href_path.HasScheme()) {
					auto parent_dir = Path::FromString(parent_path).Parent();
					href = parent_dir.Join(href_path).ToString();
				}

				std::string json_str = ReadContentOfJsonFile(context.client_context, href, buffer);
				count += ParseContentOfJsonObject(context, json_str, href, catalog_id, collection_id, rows, buffer);
			}
		}
	}
	return count;
}

//! Parses a STAC JSON object to extract the item schema and rows.
static std::size_t ParseContentOfJsonObject(CatalogContext &context, yyjson_val *json_data,
                                            const std::string &object_path, std::string catalog_id,
                                            std::string collection_id, std::vector<ItemRow> &rows,
                                            MemoryStream &buffer) {
	yyjson_val *temp_val = nullptr;
	const char *item_type = nullptr;

	if (yyjson_is_str(temp_val = yyjson_obj_get(json_data, "type"))) {
		item_type = yyjson_get_str(temp_val);
	}
	if (!item_type || strlen(item_type) == 0) {
		throw InvalidInputException("Missing 'type' field in the JSON object '%s'.", object_path.c_str());
	}

	// Handle data of a STAC Catalog, Collection or Feature...
	if (strcmp(item_type, "Catalog") == 0) {
		if (yyjson_is_str(temp_val = yyjson_obj_get(json_data, "id"))) {
			catalog_id = yyjson_get_str(temp_val);
		}
		if (yyjson_is_arr(temp_val = yyjson_obj_get(json_data, "links"))) {
			return ParseContentOfJsonLinks(context, temp_val, object_path, catalog_id, collection_id, rows, buffer);
		}
		return 0;
	}
	if (strcmp(item_type, "Collection") == 0) {
		if (yyjson_is_str(temp_val = yyjson_obj_get(json_data, "id"))) {
			collection_id = yyjson_get_str(temp_val);
		}
		if (yyjson_is_arr(temp_val = yyjson_obj_get(json_data, "links"))) {
			return ParseContentOfJsonLinks(context, temp_val, object_path, catalog_id, collection_id, rows, buffer);
		}
		return 0;
	}
	if (strcmp(item_type, "Feature") == 0) {
		ItemRow row;
		row.catalog = catalog_id;

		// Extract id
		if (yyjson_is_str(temp_val = yyjson_obj_get(json_data, "id"))) {
			row.id = yyjson_get_str(temp_val);
		} else {
			throw InvalidInputException("Missing 'id' field in the JSON Feature '%s'.", object_path.c_str());
		}

		// Extract geometry as WKT
		if (yyjson_is_obj(temp_val = yyjson_obj_get(json_data, "geometry"))) {
			row.geometry = JsonGeometry::ParseGeometryAsWKT(temp_val);
		} else {
			throw InvalidInputException("Missing 'geometry' field in the JSON Feature '%s'.", object_path.c_str());
		}

		// Extract bbox
		if (yyjson_is_arr(temp_val = yyjson_obj_get(json_data, "bbox"))) {
			row.bbox = JsonObject::ParseBoundingBoxObject(temp_val);
		} else {
			throw InvalidInputException("Missing 'bbox' field in the JSON Feature '%s'.", object_path.c_str());
		}

		// Extract stac_extensions
		if (yyjson_is_arr(temp_val = yyjson_obj_get(json_data, "stac_extensions"))) {
			row.extensions = JsonObject::ParseExtensionsObject(temp_val);
		} else {
			throw InvalidInputException("Missing 'stac_extensions' field in the JSON Feature '%s'.",
			                            object_path.c_str());
		}

		// Extract links
		if (yyjson_is_arr(temp_val = yyjson_obj_get(json_data, "links"))) {
			row.links = JsonObject::ParseLinksObject(temp_val);
		} else {
			throw InvalidInputException("Missing 'links' field in the JSON Feature '%s'.", object_path.c_str());
		}

		// Extract assets
		if (yyjson_is_obj(temp_val = yyjson_obj_get(json_data, "assets"))) {
			row.assets = JsonObject::ParseAssetsObject(temp_val);
		} else {
			throw InvalidInputException("Missing 'assets' field in the JSON Feature '%s'.", object_path.c_str());
		}

		// Extract collection id (if present)
		if (yyjson_is_str(temp_val = yyjson_obj_get(json_data, "collection"))) {
			row.collection = yyjson_get_str(temp_val);
		} else {
			row.collection = collection_id;
		}

		// Extract properties (all other fields)
		if (yyjson_is_obj(temp_val = yyjson_obj_get(json_data, "properties"))) {
			yyjson_obj_iter iter;
			yyjson_obj_iter_init(temp_val, &iter);
			yyjson_val *key, *val;

			while ((key = yyjson_obj_iter_next(&iter))) {
				if (yyjson_is_str(key) && (val = yyjson_obj_iter_get_val(key))) {
					std::string key_str = yyjson_get_str(key);
					idx_t key_idx = 0;

					// New property? If so, add it to the schema.
					auto it = context.property_set.find(key_str);
					if (it == context.property_set.end()) {
						key_idx = static_cast<idx_t>(context.column_names.size());
						context.property_set[key_str] = key_idx;
						context.column_names.push_back(key_str);
						context.column_types.push_back(JsonObject::GetPropertyTypeOfJsonValue(val));
					} else {
						key_idx = it->second;
					}

					// Add the property value to the row.
					row.properties.emplace(key_idx, JsonObject::GetPropertyValueOfJsonValue(val));
				}
			}
		} else {
			throw InvalidInputException("Missing 'properties' field in the JSON Feature '%s'.", object_path.c_str());
		}

		rows.push_back(row);
		return 1;
	}
	return 0;
}

//! Parses a STAC JSON object to extract the item rows.
static std::size_t ParseContentOfJsonObject(CatalogContext &context, std::string &json_str, std::string &object_path,
                                            std::string catalog_id, std::string collection_id,
                                            std::vector<ItemRow> &rows, MemoryStream &buffer) {
	yyjson_doc *json_data = yyjson_read(json_str.c_str(), json_str.size(), YYJSON_READ_NOFLAG);
	if (!json_data) {
		throw IOException("Failed to parse data of the object '%s'.", object_path.c_str());
	}

	try {
		yyjson_val *root_val = yyjson_doc_get_root(json_data);
		if (!root_val) {
			throw IOException("Failed to get the root value of the JSON object '%s'.", object_path.c_str());
		}

		std::size_t count =
		    ParseContentOfJsonObject(context, root_val, object_path, catalog_id, collection_id, rows, buffer);

		// Make sure to free the JSON document
		yyjson_doc_free(json_data);

		return count;
	} catch (...) {
		// Make sure to free the JSON document in case of an exception
		yyjson_doc_free(json_data);
		throw;
	}
}

//======================================================================================================================
// STAC_Read
//======================================================================================================================

struct STAC_Read {
	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------

	struct BindData final : TableFunctionData {
		CatalogContext catalog_context;
		std::string catalog_path;
		std::vector<ItemRow> rows;
		std::string where_clause;
		idx_t row_offset = 0;
		idx_t row_count = 0;

		explicit BindData(CatalogContext &&context) : catalog_context(std::move(context)) {
		}
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<string> &names) {
		D_ASSERT(input.inputs.size() == 1);

		auto catalog_path = input.inputs[0].GetValue<std::string>();
		if (catalog_path.empty()) {
			throw InvalidInputException("First parameter, the 'catalog_path', cannot be empty.");
		}

		// Get the catalog metadata and determine the return types and column names.

		std::string crs = "EPSG:4326"; // Default CRS for STAC items

		names.emplace_back("catalog");
		return_types.push_back(LogicalType::VARCHAR);
		names.emplace_back("collection");
		return_types.push_back(LogicalType::VARCHAR);
		names.emplace_back("id");
		return_types.push_back(LogicalType::VARCHAR);
		names.emplace_back("geometry");
		return_types.push_back(LogicalType::GEOMETRY(crs));
		names.emplace_back("bbox");
		return_types.push_back(STACTypes::BBOX());
		names.emplace_back("stac_extensions");
		return_types.push_back(LogicalType::LIST(LogicalType::VARCHAR));
		names.emplace_back("links");
		return_types.push_back(LogicalType::LIST(STACTypes::LINK()));
		names.emplace_back("assets");
		return_types.push_back(LogicalType::MAP(LogicalType::VARCHAR, STACTypes::ASSET()));

		// Set of properties are dynamic, so we must query the catalog to determine their schema.

		MemoryStream buffer(Allocator::Get(context));

		std::string lower_path = StringUtil::Lower(catalog_path);
		std::string json_str;

		if (StringUtil::EndsWith(lower_path, ".json") || StringUtil::EndsWith(lower_path, ".geojson")) {
			json_str = ReadContentOfJsonFile(context, catalog_path, buffer);
		} else {
			throw NotImplementedException("Dynamic Catalogs are not yet supported.");
		}

		// Parse JSON response

		CatalogContext catalog_context {context};
		std::vector<ItemRow> rows;
		std::size_t row_count = ParseContentOfJsonObject(catalog_context, json_str, catalog_path, "", "", rows, buffer);

		for (const auto &prop_name : catalog_context.column_names) {
			names.push_back(prop_name);
		}
		for (const auto &prop_type : catalog_context.column_types) {
			return_types.push_back(prop_type);
		}

		// Return the bind data.

		auto result = make_uniq<BindData>(std::move(catalog_context));
		result->catalog_path = std::move(catalog_path);
		result->rows = std::move(rows);
		result->row_offset = 0;
		result->row_count = row_count;

		return std::move(result);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Init Global
	//------------------------------------------------------------------------------------------------------------------

	struct State final : GlobalTableFunctionState {
		idx_t current_row;
		explicit State() : current_row(0) {
		}
	};

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
		// Capture the final projected column IDs here, after all optimizer passes.
		// input.column_ids is guaranteed to match output.data.size() in Execute.
		auto &bind_data = const_cast<BindData &>(input.bind_data->Cast<BindData>());
		bind_data.column_ids = input.column_ids;

		return make_uniq_base<GlobalTableFunctionState, State>();
	}

	//------------------------------------------------------------------------------------------------------------------
	// Init Local
	//------------------------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------------------------
	// Optimize (Only LIMIT pushdown is implemented)
	//------------------------------------------------------------------------------------------------------------------

	static void Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op) {
		// Apply optimizations on the LogicalPlan

		if (op->type == LogicalOperatorType::LOGICAL_LIMIT) {
			auto &limit = op->Cast<LogicalLimit>();

			// Only push down simple LIMIT & OFFSET without ORDER BY or GROUP BY, and with constant values,
			// as it would change the result of the query.
			if (limit.limit_val.Type() != LimitNodeType::CONSTANT_VALUE) {
				return;
			}
			if (limit.offset_val.Type() != LimitNodeType::UNSET &&
			    limit.offset_val.Type() != LimitNodeType::CONSTANT_VALUE) {
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

						if (limit.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
							const idx_t offset_value = limit.offset_val.GetConstantValue();
							STAC_SCAN_DEBUG_LOG(1, "OFFSET pushdown: %" PRIu64, offset_value);
							bind_data.row_offset = offset_value;
							limit.offset_val = BoundLimitNode();
						}
						const idx_t limit_value = limit.limit_val.GetConstantValue();
						STAC_SCAN_DEBUG_LOG(1, "LIMIT pushdown: %" PRIu64, limit_value);
						bind_data.row_count = MinValue<idx_t>(bind_data.row_count, bind_data.row_offset + limit_value);
						limit.limit_val = BoundLimitNode();
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
	// Complex Filter Pushdown
	//------------------------------------------------------------------------------------------------------------------

	static void PushdownComplexFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
	                                  vector<unique_ptr<Expression>> &expressions) {
		auto &bind_data = bind_data_p->Cast<BindData>();

		// Catch filter expressions for later evaluation during scanning if possible.
		if (!expressions.empty()) {
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Cardinality
	//------------------------------------------------------------------------------------------------------------------

	static unique_ptr<NodeStatistics> Cardinality(ClientContext &context, const FunctionData *data) {
		auto &bind_data = data->Cast<BindData>();
		auto result = make_uniq<NodeStatistics>();

		// This is the maximum number of rows/tiles in a single file
		result->has_max_cardinality = true;
		result->max_cardinality = bind_data.row_count - bind_data.row_offset;

		// This is an estimate of the number of rows/tiles
		result->has_estimated_cardinality = true;
		result->estimated_cardinality = result->max_cardinality;

		return result;
	}

	//------------------------------------------------------------------------------------------------------------------
	// Execute
	//------------------------------------------------------------------------------------------------------------------

	static void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
		auto &bind_data = input.bind_data->Cast<BindData>();
		auto &gstate = input.global_state->Cast<State>();

		// If we've already read all the rows, return an empty chunk to indicate we're done.
		const idx_t start_row = bind_data.row_offset + gstate.current_row;
		if (start_row >= bind_data.row_count) {
			output.SetCardinality(0);
			return;
		}

		const idx_t output_size = MinValue<idx_t>(STANDARD_VECTOR_SIZE, bind_data.row_count - start_row);
		idx_t current_row = bind_data.row_offset + gstate.current_row;

		// Load current subset of rows.
		for (idx_t row_idx = 0, record_idx = current_row; row_idx < output_size; row_idx++, record_idx++) {
			const auto &item_row = bind_data.rows[record_idx];

			for (idx_t col_idx = 0; col_idx < bind_data.column_ids.size(); col_idx++) {
				const auto &dim_index = bind_data.column_ids[col_idx];

				switch (dim_index) {
				case STAC_CATALOG_COLUMN_INDEX:
					output.data[col_idx].SetValue(row_idx, Value(item_row.catalog));
					break;
				case STAC_COLLECTION_COLUMN_INDEX:
					output.data[col_idx].SetValue(row_idx, Value(item_row.collection));
					break;
				case STAC_ID_COLUMN_INDEX:
					output.data[col_idx].SetValue(row_idx, Value(item_row.id));
					break;
				case STAC_GEOMETRY_COLUMN_INDEX:
					output.data[col_idx].SetValue(row_idx, item_row.geometry);
					break;
				case STAC_BBOX_COLUMN_INDEX:
					output.data[col_idx].SetValue(row_idx, item_row.bbox);
					break;
				case STAC_EXTENSIONS_COLUMN_INDEX:
					output.data[col_idx].SetValue(row_idx, item_row.extensions);
					break;
				case STAC_LINKS_COLUMN_INDEX:
					output.data[col_idx].SetValue(row_idx, item_row.links);
					break;
				case STAC_ASSETS_COLUMN_INDEX:
					output.data[col_idx].SetValue(row_idx, item_row.assets);
					break;
				// Handle dynamic properties columns
				default:
					if (dim_index >= STAC_FIRST_PROPERTY_COLUMN_INDEX) {
						const idx_t property_idx = dim_index - STAC_FIRST_PROPERTY_COLUMN_INDEX;

						auto it = item_row.properties.find(property_idx);
						if (it != item_row.properties.end()) {
							output.data[col_idx].SetValue(row_idx, it->second);
						} else {
							output.data[col_idx].SetValue(row_idx, Value());
						}
					}
					break;
				}
			}
		}

		// Update the row index.
		gstate.current_row += output_size;

		// Set the cardinality of the output.
		output.SetCardinality(output_size);
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
	// Register
	//------------------------------------------------------------------------------------------------------------------

	static void Register(ExtensionLoader &loader) {
		InsertionOrderPreservingMap<string> tags;
		tags.insert("ext", "stac");
		tags.insert("category", "table");

		TableFunction func("STAC_Read", {LogicalType::VARCHAR}, Execute, Bind, Init);
		func.cardinality = Cardinality;

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
// Register Read Functions
// #####################################################################################################################

void STACReadFunctions::Register(ExtensionLoader &loader) {
	STAC_Read::Register(loader);
}

} // namespace duckdb
