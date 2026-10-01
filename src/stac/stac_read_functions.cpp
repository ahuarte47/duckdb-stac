#include "stac_types.hpp"
#include "stac_reader.hpp"
#include "stac_read_functions.hpp"
#include "stac_schema.hpp"
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
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_order.hpp"
#include "yyjson.hpp"
using namespace duckdb_yyjson; // NOLINT

// STAC
#include "filter_eval.hpp"
#include "json_geometry.hpp"
#include "json_object.hpp"
#include "search_filter.hpp"

namespace duckdb {

namespace {

//======================================================================================================================
// STAC Item & Collection Readers
//======================================================================================================================

//! Reads the set of items contained in a STAC Catalog.
class ItemReader : public STACReader {
public:
	using STACReader::ReadContentOfObject;

	ItemReader(ClientContext &context, MemoryStream &buffer, const ItemSchema &schema,
	           const FilterContext &filter_context, idx_t row_offset = 0, std::size_t row_limit = 0)
	    : STACReader(context, buffer, row_offset, row_limit), filter_context(filter_context), schema(schema) {
	}

private:
	//! The filter context for pushdown filtering, if defined.
	const FilterContext filter_context;

	//! The Catalog identifier read so far.
	std::string catalog_id;
	//! The Collection identifier read so far.
	std::string collection_id;

public:
	//! The schema of the Catalog.
	const ItemSchema &schema;

	//! Set of item rows already extracted.
	std::vector<ItemRow> rows;

public:
	//! Returns true if the given STAC "rel_type" requires fetching a node link (e.g., a "child" link).
	virtual bool NeedConsumeLink(const char *rel_type) override {
		return rel_type &&
		       (strcmp(rel_type, "child") == 0 || strcmp(rel_type, "item") == 0 || strcmp(rel_type, "items") == 0);
	}

	//! Reads the content of a JSON object to extract the child STAC objects.
	virtual void ReadContentOfObject(yyjson_val *json_val, const std::string &json_path, int32_t ttl_seconds) override {
		yyjson_val *temp_val = nullptr;
		const char *item_type = nullptr;

		// Stop processing if the limit is reached.

		if (row_limit > 0 && row_count >= row_limit) {
			next_href.clear();
			return;
		}

		// Handle data of a STAC Catalog, Collection or Feature...

		if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "type"))) {
			item_type = yyjson_get_str(temp_val);
		}
		if (!item_type || strlen(item_type) == 0) {
			throw InvalidInputException("Missing 'type' field in the JSON object '%s'.", json_path.c_str());
		}

		if (strcmp(item_type, "Catalog") == 0) {
			std::string prev_catalog_id = catalog_id;

			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "id"))) {
				catalog_id = yyjson_get_str(temp_val);
			}
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "links"))) {
				ReadContentOfLinks(temp_val, json_path, ttl_seconds);
			}
			catalog_id = prev_catalog_id;
			return;
		}
		if (strcmp(item_type, "Collection") == 0) {
			std::string prev_collection_id = collection_id;

			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "id"))) {
				collection_id = yyjson_get_str(temp_val);
			}
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "links"))) {
				ReadContentOfLinks(temp_val, json_path, ttl_seconds);
			}
			collection_id = prev_collection_id;
			return;
		}
		if (strcmp(item_type, "FeatureCollection") == 0) {
			if (yyjson_is_obj(temp_val = yyjson_obj_get(json_val, "search:metadata")) &&
			    yyjson_is_int(temp_val = yyjson_obj_get(temp_val, "numberMatched"))) {
				number_matched = yyjson_get_int(temp_val);
			}
			if (yyjson_is_int(temp_val = yyjson_obj_get(json_val, "numberMatched"))) {
				number_matched = yyjson_get_int(temp_val);
			}
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "features"))) {
				std::size_t features_size = yyjson_arr_size(temp_val);

				for (std::size_t i = 0; i < features_size; i++) {
					yyjson_val *feature_val = yyjson_arr_get(temp_val, i);

					// Ignore rows until the row_offset is reached.

					if (row_offset > 0 && filter_context.expressions.empty()) {
						row_offset--;
						continue;
					}

					// Stop processing if the limit is reached.

					if (row_limit > 0 && row_count >= row_limit) {
						next_href.clear();
						return;
					}

					// Parse the child JSON item recursively.

					if (yyjson_is_obj(feature_val)) {
						ReadContentOfObject(feature_val, json_path, ttl_seconds);
					}
				}
			}
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "links"))) {
				ReadContentOfLinks(temp_val, json_path, ttl_seconds);
			}
			return;
		}
		if (strcmp(item_type, "Feature") == 0) {
			// Ignore rows until the row_offset is reached.

			if (row_offset > 0 && filter_context.expressions.empty()) {
				row_offset--;
				return;
			}

			// Collect the data of the Feature.

			ItemRow row;
			row.catalog = Value(catalog_id);

			// Extract id
			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "id"))) {
				row.id = Value(yyjson_get_str(temp_val));
			} else {
				throw InvalidInputException("Missing 'id' field in the JSON Feature '%s'.", json_path.c_str());
			}

			// Extract geometry as WKT
			if (yyjson_is_obj(temp_val = yyjson_obj_get(json_val, "geometry"))) {
				row.geometry = JsonGeometry::ParseGeometryAsWKT(temp_val);
			} else {
				throw InvalidInputException("Missing 'geometry' field in the JSON Feature '%s'.", json_path.c_str());
			}

			// Extract bbox
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "bbox"))) {
				row.bbox = JsonObject::ParseBoundingBoxObject(temp_val);
			} else {
				throw InvalidInputException("Missing 'bbox' field in the JSON Feature '%s'.", json_path.c_str());
			}

			// Extract stac_version
			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "stac_version"))) {
				row.version = Value(yyjson_get_str(temp_val));
			} else {
				throw InvalidInputException("Missing 'stac_version' field in the JSON Feature '%s'.",
				                            json_path.c_str());
			}

			// Extract stac_extensions
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "stac_extensions"))) {
				row.extensions = JsonObject::ParseExtensionsObject(temp_val);
			} else {
				throw InvalidInputException("Missing 'stac_extensions' field in the JSON Feature '%s'.",
				                            json_path.c_str());
			}

			// Extract links
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "links"))) {
				row.links = JsonObject::ParseLinksObject(temp_val);
			} else {
				throw InvalidInputException("Missing 'links' field in the JSON Feature '%s'.", json_path.c_str());
			}

			// Extract assets
			if (yyjson_is_obj(temp_val = yyjson_obj_get(json_val, "assets"))) {
				row.assets = JsonObject::ParseAssetsObject(temp_val);
			} else {
				throw InvalidInputException("Missing 'assets' field in the JSON Feature '%s'.", json_path.c_str());
			}

			// Extract collection id (if present)
			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "collection"))) {
				row.collection = Value(yyjson_get_str(temp_val));
			} else {
				row.collection = Value(collection_id);
			}

			// Extract properties (all other dynamic fields)
			if (yyjson_is_obj(temp_val = yyjson_obj_get(json_val, "properties"))) {
				yyjson_obj_iter iter;
				yyjson_obj_iter_init(temp_val, &iter);
				yyjson_val *key, *val;

				while ((key = yyjson_obj_iter_next(&iter))) {
					if (yyjson_is_str(key) && (val = yyjson_obj_iter_get_val(key))) {
						std::string key_str = yyjson_get_str(key);
						idx_t key_idx = 0;

						// New property? the schema should have been extracted already.
						auto it = schema.property_set.find(key_str);
						if (it == schema.property_set.end()) {
							STAC_SCAN_DEBUG_LOG(
							    3, " > id=(%s): property '%s' not found in the schema for the JSON Feature '%s'.",
							    row.id.ToString().c_str(), key_str.c_str(), json_path.c_str());

							// Skip properties not found in the schema.
							continue;
						} else {
							key_idx = it->second;
						}

						// Add the property value to the row.
						row.properties.emplace(key_idx, JsonObject::GetPropertyValueOfJsonValue(val));
					}
				}
			} else {
				throw InvalidInputException("Missing 'properties' field in the JSON Feature '%s'.", json_path.c_str());
			}

			// The filter expressions were evaluated but item does not match the conditions?
			if (!FilterEval::Eval(row, filter_context)) {
				STAC_SCAN_DEBUG_LOG(1, " > id=(%s): item did not match filter conditions, skipped",
				                    row.id.ToString().c_str());
				return;
			}
			if (row_offset > 0 && !filter_context.expressions.empty()) {
				row_offset--;
				return;
			}

			rows.push_back(row);
			row_count++;
		}
	}
};

//! Reads the set of collections contained in a STAC Catalog.
class CollectionReader : public STACReader {
public:
	using STACReader::ReadContentOfObject;

	CollectionReader(ClientContext &context, MemoryStream &buffer, idx_t row_offset = 0, std::size_t row_limit = 0)
	    : STACReader(context, buffer, row_offset, row_limit) {
	}

private:
	//! The Catalog identifier read so far.
	std::string catalog_id;

public:
	//! Set of collection rows already extracted.
	std::vector<CollectionRow> rows;

public:
	//! Returns true if the given STAC "rel_type" requires fetching a node link (e.g., a "child" link).
	bool NeedConsumeLink(const char *rel_type) override {
		return rel_type && strcmp(rel_type, "child") == 0;
	}

	//! Reads the content of a JSON object to extract the child STAC objects.
	void ReadContentOfObject(yyjson_val *json_val, const std::string &json_path, int32_t ttl_seconds) override {
		yyjson_val *temp_val = nullptr;
		const char *item_type = nullptr;

		// Stop processing if the limit is reached.

		if (row_limit > 0 && row_count >= row_limit) {
			next_href.clear();
			return;
		}

		// Handle data of a STAC Catalog or Collection...

		if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "type"))) {
			item_type = yyjson_get_str(temp_val);
		}
		if (!item_type || strlen(item_type) == 0) {
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "collections"))) {
				item_type = "Collections";
			} else {
				throw InvalidInputException("Missing 'type' field in the JSON object '%s'.", json_path.c_str());
			}
		}

		if (strcmp(item_type, "Catalog") == 0) {
			std::string prev_catalog_id = catalog_id;

			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "id"))) {
				catalog_id = yyjson_get_str(temp_val);
			}
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "links"))) {
				ReadContentOfLinks(temp_val, json_path, ttl_seconds);
			}
			catalog_id = prev_catalog_id;
			return;
		}
		if (strcmp(item_type, "Collections") == 0) {
			yyjson_val *colls_val = yyjson_obj_get(json_val, "collections");
			std::size_t colls_size = yyjson_arr_size(colls_val);
			yyjson_val *coll_val = nullptr;

			for (std::size_t i = 0; i < colls_size; i++) {
				if (yyjson_is_obj(coll_val = yyjson_arr_get(colls_val, i))) {
					ReadContentOfObject(coll_val, json_path, ttl_seconds);
				}
			}
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "links"))) {
				ReadContentOfLinks(temp_val, json_path, ttl_seconds);
			}
			return;
		}
		if (strcmp(item_type, "Collection") == 0) {
			// Ignore rows until the row_offset is reached.

			if (row_offset > 0) {
				row_offset--;
				return;
			}

			// Collect the data of the Collection.

			CollectionRow row;
			row.catalog = Value(catalog_id);

			// Extract id
			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "id"))) {
				row.id = Value(yyjson_get_str(temp_val));
			} else {
				throw InvalidInputException("Missing 'id' field in the JSON Collection '%s'.", json_path.c_str());
			}

			// Extract title
			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "title"))) {
				row.title = Value(yyjson_get_str(temp_val));
			}

			// Extract description
			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "description"))) {
				row.description = Value(yyjson_get_str(temp_val));
			}

			// Extract keywords
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "keywords"))) {
				row.keywords = JsonObject::ParseKeywordsObject(temp_val);
			}

			// Extract license
			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "license"))) {
				row.license = Value(yyjson_get_str(temp_val));
			}

			// Extract providers
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "providers"))) {
				row.providers = JsonObject::ParseGenericObject(temp_val);
			}

			// Extract bbox/interval (extent)
			if (yyjson_is_obj(temp_val = yyjson_obj_get(json_val, "extent"))) {
				yyjson_val *spatial_val = nullptr;
				yyjson_val *rngtime_val = nullptr;

				if (yyjson_is_obj(spatial_val = yyjson_obj_get(temp_val, "spatial")) &&
				    yyjson_is_arr(spatial_val = yyjson_obj_get(spatial_val, "bbox")) &&
				    yyjson_arr_size(spatial_val) > 0) {
					yyjson_val *bbox_val = yyjson_arr_get(spatial_val, 0);
					row.bbox = JsonObject::ParseBoundingBoxObject(bbox_val);
				}
				if (yyjson_is_obj(rngtime_val = yyjson_obj_get(temp_val, "temporal")) &&
				    yyjson_is_arr(rngtime_val = yyjson_obj_get(rngtime_val, "interval")) &&
				    yyjson_arr_size(rngtime_val) > 0) {
					yyjson_val *interval_val = yyjson_arr_get(rngtime_val, 0);
					row.interval = JsonObject::ParseIntervalObject(interval_val);
				}
			}

			// Extract summaries
			if (yyjson_is_obj(temp_val = yyjson_obj_get(json_val, "summaries"))) {
				row.summaries = JsonObject::ParseGenericObject(temp_val);
			}

			// Extract stac_version
			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "stac_version"))) {
				row.version = Value(yyjson_get_str(temp_val));
			}

			// Extract stac_extensions
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "stac_extensions"))) {
				row.extensions = JsonObject::ParseExtensionsObject(temp_val);
			}

			// Extract links
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "links"))) {
				row.links = JsonObject::ParseLinksObject(temp_val);
			}

			// Extract assets
			if (yyjson_is_obj(temp_val = yyjson_obj_get(json_val, "assets"))) {
				row.assets = JsonObject::ParseAssetsObject(temp_val);
			}

			// Extract item assets
			if (yyjson_is_obj(temp_val = yyjson_obj_get(json_val, "item_assets"))) {
				row.item_assets = JsonObject::ParseGenericObject(temp_val);
			}

			rows.push_back(row);
			row_count++;

			// Next, extract the links if available.

			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "links"))) {
				ReadContentOfLinks(temp_val, json_path, ttl_seconds);
			}
			return;
		}
	}
};

//======================================================================================================================
// STAC_Read
//======================================================================================================================

struct STAC_Read {
	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------

	struct BindData final : TableFunctionData {
		// The path (URL or file path) to the STAC Catalog.
		std::string catalog_path;
		// The schema of the STAC Catalog.
		ItemSchema schema;

		// A MemoryStream buffer used for reading JSON content.
		MemoryStream buffer;

		// Offset for the rows to be read.
		idx_t row_offset = 0;
		// Limit for the rows to be read (A value of 0 means no limit is applied).
		std::size_t row_limit = 0;
		// Total number of items matched by the filter (if any) in the Catalog.
		int number_matched = -1;

		// Optional search criteria for the STAC API item-search.
		SearchFilter search_filter;

		// Optional pushdown filter expressions for the STAC items.
		vector<unique_ptr<Expression>> filter_expressions;
		// All column types for the output of the table function, including dynamic fields.
		vector<LogicalType> column_types;

		explicit BindData(ItemSchema &&schema, MemoryStream &&buffer)
		    : schema(std::move(schema)), buffer(std::move(buffer)) {
		}
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<string> &names,
	                                     const SearchFilter &search_filter) {
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
		names.emplace_back("stac_version");
		return_types.push_back(LogicalType::VARCHAR);
		names.emplace_back("stac_extensions");
		return_types.push_back(LogicalType::LIST(LogicalType::VARCHAR));
		names.emplace_back("links");
		return_types.push_back(LogicalType::LIST(STACTypes::LINK()));
		names.emplace_back("assets");
		return_types.push_back(LogicalType::MAP(LogicalType::VARCHAR, STACTypes::ASSET()));

		// Set of properties are dynamic, so we must query the catalog to determine their schema.

		STAC_SCAN_DEBUG_LOG(1, "Reading the schema of the Catalog '%s'...", catalog_path.c_str());

		MemoryStream buffer(Allocator::Get(context));

		ItemSchema schema {context, buffer};
		auto json_str = ReadContentOfCatalog(context, buffer, catalog_path, search_filter, 30);
		schema.ParseSchemaOfObject("", "", json_str, catalog_path, 30);

		for (const auto &prop_name : schema.column_names) {
			names.emplace_back(prop_name);
		}
		for (const auto &prop_type : schema.column_types) {
			return_types.emplace_back(prop_type);
		}

		// Return the bind data.

		auto result = make_uniq<BindData>(std::move(schema), std::move(buffer));
		result->catalog_path = std::move(catalog_path);
		result->column_types = return_types;
		result->search_filter = search_filter;
		result->number_matched = schema.GetNumberMatched();
		result->row_limit = 0;
		result->row_offset = 0;

		return std::move(result);
	}

	static unique_ptr<FunctionData> BindRead(ClientContext &context, TableFunctionBindInput &input,
	                                         vector<LogicalType> &return_types, vector<string> &names) {
		return Bind(context, input, return_types, names, SearchFilter());
	}

	//------------------------------------------------------------------------------------------------------------------
	// Init Global
	//------------------------------------------------------------------------------------------------------------------

	struct State final : GlobalTableFunctionState {
		ItemReader reader;
		explicit State(ItemReader &&reader) : reader(std::move(reader)) {
		}
	};

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
		// Capture the final projected column IDs here, after all optimizer passes.
		// input.column_ids is guaranteed to match output.data.size() in Execute.
		auto &bind_data = const_cast<BindData &>(input.bind_data->Cast<BindData>());
		bind_data.column_ids = input.column_ids;

		// The filter expressions built in PushdownComplexFilter reference columns by their absolute table
		// column index (since the final projection wasn't known yet at that point). Remap them now to the
		// position within the final projected column list, since that is what FilterEval's input DataChunk
		// is built with (see filter_eval.cpp).
		if (!bind_data.filter_expressions.empty()) {
			unordered_map<idx_t, idx_t> table_col_to_position;

			for (idx_t i = 0; i < bind_data.column_ids.size(); i++) {
				table_col_to_position[bind_data.column_ids[i]] = i;
			}
			for (auto &expr : bind_data.filter_expressions) {
				ExpressionIterator::VisitExpressionMutable<BoundReferenceExpression>(
				    expr, [&table_col_to_position](BoundReferenceExpression &bound_ref, unique_ptr<Expression> &) {
					    const auto entry = table_col_to_position.find(bound_ref.index);
					    if (entry == table_col_to_position.end()) {
						    throw InternalException("STAC_Read: filter column was pruned from the projected columns");
					    }
					    bound_ref.index = entry->second;
				    });
			}
		}

		// Read the first page of the Catalog content.

		STAC_SCAN_DEBUG_LOG(1, "Reading first page of the Catalog '%s'...", bind_data.catalog_path.c_str());

		const std::string &catalog_path = bind_data.catalog_path;
		const ItemSchema &schema = bind_data.schema;
		MemoryStream &buffer = bind_data.buffer;

		const auto &filter_expressions = bind_data.filter_expressions;
		const auto &column_ids = bind_data.column_ids;
		const auto &column_types = bind_data.column_types;
		const FilterContext filter_context(context, filter_expressions, column_ids, column_types);

		ItemReader reader(context, buffer, schema, filter_context, bind_data.row_offset, bind_data.row_limit);
		auto json_str = ReadContentOfCatalog(context, buffer, catalog_path, bind_data.search_filter, 30);
		reader.ReadContentOfObject(json_str, catalog_path, 30);

		// Set the number of items matched by the filter (if any) in the Catalog.

		if (bind_data.number_matched == -1) {
			bind_data.number_matched = reader.GetNumberMatched();
		}

		STAC_SCAN_DEBUG_LOG(1, "Request matched %d items", bind_data.number_matched);

		if (bind_data.number_matched > 2048 && bind_data.row_limit == 0) {
			fprintf(stderr,
			        "STAC: Warning, the request matched %d items, consider refining the query to get fewer results!\n",
			        bind_data.number_matched);
		}

		// Return the global state with the reader.

		return make_uniq_base<GlobalTableFunctionState, State>(std::move(reader));
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
						bind_data.row_limit = limit_value;
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

		// Catch filter expressions for early evaluation during scanning if possible.
		if (!expressions.empty()) {
			const auto &column_ids = get.GetColumnIds();
			vector<unique_ptr<Expression>> temp_expressions;

			for (const auto &expr : expressions) {
				auto expr_copy = expr->Copy();

				// We need to convert the column references in the filter expressions from BoundColumnRefExpression
				// to BoundReferenceExpression, so that one ExpressionExecutor can execute them during scanning.
				// The index is temporarily set to the *absolute* table column index (rather than the position
				// within the current projection), because projection pushdown (which can add/remove/reorder
				// columns, e.g. for `count(*)` queries) runs after this callback. Init() remaps these indices to
				// the final projected column positions once bind_data.column_ids is known.
				ExpressionIterator::VisitExpressionClassMutable(
				    expr_copy, ExpressionClass::BOUND_COLUMN_REF, [&column_ids](unique_ptr<Expression> &child) {
					    const auto &col_ref = child->Cast<BoundColumnRefExpression>();
					    const auto &column_alias = col_ref.GetAlias();
					    const auto &return_type = col_ref.return_type;
					    const idx_t table_col = column_ids[col_ref.binding.column_index].GetPrimaryIndex();
					    child = make_uniq<BoundReferenceExpression>(column_alias, return_type, table_col);
				    });

				temp_expressions.push_back(std::move(expr_copy));
			}
			bind_data.filter_expressions = std::move(temp_expressions);

			// Do NOT clear 'expressions' here: keeping the filter in the logical plan ensures the referenced
			// columns stay 'used' for the projection-pushdown optimizer (otherwise columns only needed by the
			// filter, but not by the query's output, e.g. count(*), get pruned from the scan's column set).
			// DuckDB will still apply the filter on top of the scan; the copy above is only an optimization to
			// skip non-matching rows early during scanning.
		}
	}

	//------------------------------------------------------------------------------------------------------------------
	// Cardinality
	//------------------------------------------------------------------------------------------------------------------

	//------------------------------------------------------------------------------------------------------------------
	// Execute
	//------------------------------------------------------------------------------------------------------------------

	static void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
		auto &bind_data = const_cast<BindData &>(input.bind_data->Cast<BindData>());
		auto &gstate = input.global_state->Cast<State>();

		ItemReader &reader = gstate.reader;
		idx_t row_idx = 0;

		// Load pending rows from the reader into the output chunk.

		for (idx_t i = 0; i < reader.rows.size(); i++, row_idx++) {
			const auto &item_row = reader.rows[i];

			if (row_idx >= STANDARD_VECTOR_SIZE - 1) {
				reader.rows.erase(reader.rows.begin(), reader.rows.begin() + i);
				output.SetCardinality(row_idx + 1);
				return;
			}
			for (idx_t col_idx = 0; col_idx < bind_data.column_ids.size(); col_idx++) {
				const idx_t &dim_index = bind_data.column_ids[col_idx];
				const Value &value = item_row.ValueOf(dim_index);
				output.data[col_idx].SetValue(row_idx, value);
			}
		}
		reader.rows.clear();

		// Load additional rows from the next pages of the catalog if available.

		while (reader.ReadNextPageOfResults(0)) {
			for (idx_t i = 0; i < reader.rows.size(); i++, row_idx++) {
				const auto &item_row = reader.rows[i];

				if (row_idx >= STANDARD_VECTOR_SIZE - 1) {
					reader.rows.erase(reader.rows.begin(), reader.rows.begin() + i);
					output.SetCardinality(row_idx + 1);
					return;
				}
				for (idx_t col_idx = 0; col_idx < bind_data.column_ids.size(); col_idx++) {
					const idx_t &dim_index = bind_data.column_ids[col_idx];
					const Value &value = item_row.ValueOf(dim_index);
					output.data[col_idx].SetValue(row_idx, value);
				}
			}
			reader.rows.clear();
		}

		// Set the cardinality of the output.
		output.SetCardinality(row_idx);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Progress Scan
	//------------------------------------------------------------------------------------------------------------------

	static double Progress(ClientContext &context, const FunctionData *bind_data_p,
	                       const GlobalTableFunctionState *global_state) {
		auto &gstate = global_state->Cast<State>();

		const ItemReader &reader = gstate.reader;
		int number_matched = reader.GetNumberMatched();
		std::size_t current_row = reader.GetRowCount();

		// The result size is unknown, no progress to report.
		if (number_matched <= 0 || current_row <= 0) {
			return 0.0;
		}

		auto p = 100 * (static_cast<double>(current_row) / static_cast<double>(number_matched));
		return p > 100 ? 100 : p;
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

		TableFunction func("STAC_Read", {LogicalType::VARCHAR}, Execute, BindRead, Init);

		// Enable progress reporting - allows DuckDB to report the progress of the table scan
		func.table_scan_progress = Progress;

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

//======================================================================================================================
// STAC_Search
//======================================================================================================================

struct STAC_Search : public STAC_Read {
	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------

	static unique_ptr<FunctionData> BindSearch(ClientContext &context, TableFunctionBindInput &input,
	                                           vector<LogicalType> &return_types, vector<string> &names) {
		SearchFilter search_filter;

		// Parse the named parameters for the STAC Search API filter.

		const named_parameter_map_t &named_params = input.named_parameters;

		auto input_param = named_params.find("collections");
		if (input_param != named_params.end()) {
			for (auto &param : ListValue::GetChildren(input_param->second)) {
				search_filter.collections.push_back(StringValue::Get(param).c_str());
			}
		}

		input_param = named_params.find("ids");
		if (input_param != named_params.end()) {
			for (auto &param : ListValue::GetChildren(input_param->second)) {
				search_filter.ids.push_back(StringValue::Get(param).c_str());
			}
		}

		input_param = named_params.find("datetime");
		if (input_param != named_params.end()) {
			search_filter.datetime = StringValue::Get(input_param->second).c_str();
		}

		input_param = named_params.find("bbox");
		if (input_param != named_params.end()) {
			auto &list = ListValue::GetChildren(input_param->second);

			std::size_t bbox_size = list.size();
			if (bbox_size != 4) {
				throw InvalidInputException(
				    "The 'bbox' parameter must be a list of 4 numbers: [minx, miny, maxx, maxy].");
			}
			double minx = DoubleValue::Get(list[0]);
			double miny = DoubleValue::Get(list[1]);
			double maxx = DoubleValue::Get(list[2]);
			double maxy = DoubleValue::Get(list[3]);
			search_filter.bbox.Extend(VertexXY {minx, miny});
			search_filter.bbox.Extend(VertexXY {maxx, maxy});
		}

		input_param = named_params.find("intersects");
		if (input_param != named_params.end()) {
			search_filter.intersects = input_param->second;
		}

		input_param = named_params.find("filter");
		if (input_param != named_params.end()) {
			search_filter.filter = StringValue::Get(input_param->second);
		}

		input_param = named_params.find("filter_lang");
		if (input_param != named_params.end()) {
			search_filter.filter_lang = StringValue::Get(input_param->second);
		}

		input_param = named_params.find("fields");
		if (input_param != named_params.end()) {
			search_filter.fields = StringValue::Get(input_param->second);
		}

		input_param = named_params.find("sortby");
		if (input_param != named_params.end()) {
			search_filter.sortby = StringValue::Get(input_param->second);
		}

		input_param = named_params.find("max_items");
		if (input_param != named_params.end()) {
			search_filter.max_items = MaxValue<int32_t>(IntegerValue::Get(input_param->second), 0);
		}

		return STAC_Read::Bind(context, input, return_types, names, search_filter);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------

	static constexpr auto DESCRIPTION = R"(
		Searches the content of a SpatioTemporal Asset Catalog (STAC) catalog using the given STAC API - Item Search
		filtering criteria (https://api.stacspec.org/v1.0.0/item-search/) and returns the matching items as a table.

		This function exposes a STAC catalog as a relational table, following the
		[GeoParquet STAC specification](https://radiantearth.github.io/stac-geoparquet-spec/latest/).

		Each row represents a single STAC item. Almost all item fields are mapped to columns;
		nested JSON structures are preserved as Parquet structs where possible, but item properties
		are promoted to the top level for easier filtering and querying.
	)";

	static constexpr auto EXAMPLE = R"(
		SELECT * FROM STAC_Search('https://example.com/stac/collection.json', collections:='my_collection', bbox:=[-180, -90, 180, 90], max_items:=10);
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------

	static void Register(ExtensionLoader &loader) {
		InsertionOrderPreservingMap<string> tags;
		tags.insert("ext", "stac");
		tags.insert("category", "table");

		TableFunction func("STAC_Search", {LogicalType::VARCHAR}, Execute, BindSearch, Init);
		func.named_parameters["collections"] = LogicalType::LIST(LogicalType::VARCHAR);
		func.named_parameters["ids"] = LogicalType::LIST(LogicalType::VARCHAR);
		func.named_parameters["datetime"] = LogicalType::VARCHAR;
		func.named_parameters["bbox"] = LogicalType::LIST(LogicalType::DOUBLE);
		func.named_parameters["intersects"] = LogicalType::GEOMETRY("EPSG:4326");
		func.named_parameters["filter"] = LogicalType::VARCHAR;
		func.named_parameters["filter_lang"] = LogicalType::VARCHAR;
		func.named_parameters["fields"] = LogicalType::VARCHAR;
		func.named_parameters["sortby"] = LogicalType::VARCHAR;
		func.named_parameters["max_items"] = LogicalType::INTEGER;

		// Enable progress reporting - allows DuckDB to report the progress of the table scan
		func.table_scan_progress = Progress;

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

//======================================================================================================================
// STAC_Collections
//======================================================================================================================

struct STAC_Collections {
	//------------------------------------------------------------------------------------------------------------------
	// Bind
	//------------------------------------------------------------------------------------------------------------------

	struct BindData final : TableFunctionData {
		std::vector<CollectionRow> collections;
		explicit BindData(std::vector<CollectionRow> &&collections) : collections(std::move(collections)) {
		}
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<string> &names) {
		D_ASSERT(input.inputs.size() == 1);

		auto catalog_path = input.inputs[0].GetValue<std::string>();
		if (catalog_path.empty()) {
			throw InvalidInputException("First parameter, the 'catalog_path', cannot be empty.");
		}

		// Get the collections metadata and determine the return types and column names.

		names.emplace_back("catalog");
		return_types.emplace_back(LogicalType::VARCHAR);
		names.emplace_back("id");
		return_types.emplace_back(LogicalType::VARCHAR);
		names.emplace_back("title");
		return_types.emplace_back(LogicalType::VARCHAR);
		names.emplace_back("description");
		return_types.emplace_back(LogicalType::VARCHAR);
		names.emplace_back("keywords");
		return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
		names.emplace_back("license");
		return_types.emplace_back(LogicalType::VARCHAR);
		names.emplace_back("providers");
		return_types.emplace_back(LogicalType::JSON());
		names.emplace_back("bbox");
		return_types.emplace_back(STACTypes::BBOX());
		names.emplace_back("interval");
		return_types.emplace_back(LogicalType::LIST(LogicalType::TIMESTAMP));
		names.emplace_back("summaries");
		return_types.emplace_back(LogicalType::JSON());
		names.emplace_back("stac_version");
		return_types.emplace_back(LogicalType::VARCHAR);
		names.emplace_back("stac_extensions");
		return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
		names.emplace_back("links");
		return_types.emplace_back(LogicalType::LIST(STACTypes::LINK()));
		names.emplace_back("assets");
		return_types.push_back(LogicalType::MAP(LogicalType::VARCHAR, STACTypes::ASSET()));
		names.emplace_back("item_assets");
		return_types.emplace_back(LogicalType::JSON());

		// Load all collections in the STAC catalog.

		MemoryStream buffer(Allocator::Get(context));
		CollectionReader reader(context, buffer, 0, 0);
		int32_t ttl_seconds = 30;

		auto json_str = ReadContentOfCatalog(context, buffer, catalog_path, SearchFilter(), ttl_seconds);
		reader.ReadContentOfObject(json_str, catalog_path, ttl_seconds);

		while (reader.ReadNextPageOfResults(ttl_seconds)) {
			//...
		};

		return make_uniq_base<FunctionData, BindData>(std::move(reader.rows));
	}

	//------------------------------------------------------------------------------------------------------------------
	// Init
	//------------------------------------------------------------------------------------------------------------------

	struct State final : GlobalTableFunctionState {
		idx_t current_idx;
		explicit State() : current_idx(0) {
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
	// Execute
	//------------------------------------------------------------------------------------------------------------------

	static void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
		auto &bind_data = input.bind_data->Cast<BindData>();
		auto &gstate = input.global_state->Cast<State>();

		idx_t count = 0;

		const auto total_end = bind_data.collections.size();
		const auto batch_end = gstate.current_idx + STANDARD_VECTOR_SIZE;
		const auto chunk_end = MinValue<idx_t>(batch_end, total_end);

		for (const auto next_idx = chunk_end; gstate.current_idx < next_idx; gstate.current_idx++) {
			const auto &collection_row = bind_data.collections[gstate.current_idx];

			for (idx_t col_idx = 0; col_idx < bind_data.column_ids.size(); col_idx++) {
				const idx_t &dim_index = bind_data.column_ids[col_idx];
				const Value &value = collection_row.ValueOf(dim_index);
				output.data[col_idx].SetValue(count, value);
			}
			count++;
		}

		output.SetCardinality(count);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Documentation
	//------------------------------------------------------------------------------------------------------------------

	static constexpr auto DESCRIPTION = R"(
		Returns the collections available in a SpatioTemporal Asset Catalog (STAC) catalog.
	)";

	static constexpr auto EXAMPLE = R"(
		SELECT * FROM STAC_Collections('https://example.com/stac/catalog.json');
	)";

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------

	static void Register(ExtensionLoader &loader) {
		InsertionOrderPreservingMap<string> tags;
		tags.insert("ext", "stac");
		tags.insert("category", "table");

		TableFunction func("STAC_Collections", {LogicalType::VARCHAR}, Execute, Bind, Init);

		// Enable projection pushdown - allows DuckDB to tell us which columns are needed
		// The column_ids will be passed to InitGlobal via TableFunctionInitInput
		func.projection_pushdown = true;

		RegisterFunction<TableFunction>(loader, func, CatalogType::TABLE_FUNCTION_ENTRY, DESCRIPTION, EXAMPLE, tags);
	}
};

} // namespace

// #####################################################################################################################
// Register Read Functions
// #####################################################################################################################

void STACReadFunctions::Register(ExtensionLoader &loader) {
	STAC_Read::Register(loader);
	STAC_Search::Register(loader);
	STAC_Collections::Register(loader);
}

} // namespace duckdb
