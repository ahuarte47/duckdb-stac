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
	           unique_ptr<FilterEval> filter_eval, idx_t row_offset = 0, std::size_t row_limit = 0)
	    : STACReader(context, buffer, row_offset, row_limit), filter_eval(std::move(filter_eval)), schema(schema) {
	}

private:
	//! The filter evaluator for pushdown filtering, if defined.
	unique_ptr<FilterEval> filter_eval;

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
	bool NeedConsumeLink(const char *rel_type) override {
		return rel_type &&
		       (strcmp(rel_type, "child") == 0 || strcmp(rel_type, "item") == 0 || strcmp(rel_type, "items") == 0);
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

		// Handle data of a STAC Catalog, Collection or Feature...

		temp_val = yyjson_obj_get(json_val, "type");
		if (yyjson_is_str(temp_val)) {
			item_type = yyjson_get_str(temp_val);
		}
		if (!item_type || strlen(item_type) == 0) {
			throw InvalidInputException("Missing 'type' field in the JSON object '%s'.", json_path.c_str());
		}

		if (strcmp(item_type, "Catalog") == 0) {
			std::string prev_catalog_id = catalog_id;

			temp_val = yyjson_obj_get(json_val, "id");
			if (yyjson_is_str(temp_val)) {
				catalog_id = yyjson_get_str(temp_val);
			}
			temp_val = yyjson_obj_get(json_val, "links");
			if (yyjson_is_arr(temp_val)) {
				ReadContentOfLinks(temp_val, json_path, ttl_seconds);
			}
			catalog_id = prev_catalog_id;
			return;
		}
		if (strcmp(item_type, "Collection") == 0) {
			std::string prev_collection_id = collection_id;

			temp_val = yyjson_obj_get(json_val, "id");
			if (yyjson_is_str(temp_val)) {
				collection_id = yyjson_get_str(temp_val);
			}
			temp_val = yyjson_obj_get(json_val, "links");
			if (yyjson_is_arr(temp_val)) {
				ReadContentOfLinks(temp_val, json_path, ttl_seconds);
			}
			collection_id = prev_collection_id;
			return;
		}
		if (strcmp(item_type, "FeatureCollection") == 0) {
			temp_val = yyjson_obj_get(json_val, "search:metadata");
			if (yyjson_is_obj(temp_val)) {
				temp_val = yyjson_obj_get(temp_val, "numberMatched");
				if (yyjson_is_int(temp_val)) {
					number_matched = yyjson_get_int(temp_val);
				}
			}
			temp_val = yyjson_obj_get(json_val, "numberMatched");
			if (yyjson_is_int(temp_val)) {
				number_matched = yyjson_get_int(temp_val);
			}
			temp_val = yyjson_obj_get(json_val, "features");
			if (yyjson_is_arr(temp_val)) {
				std::size_t features_size = yyjson_arr_size(temp_val);

				for (std::size_t i = 0; i < features_size; i++) {
					yyjson_val *feature_val = yyjson_arr_get(temp_val, i);

					// Ignore rows until the row_offset is reached.

					if (row_offset > 0 && !filter_eval->HasExpressions()) {
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
			temp_val = yyjson_obj_get(json_val, "links");
			if (yyjson_is_arr(temp_val)) {
				ReadContentOfLinks(temp_val, json_path, ttl_seconds);
			}
			return;
		}
		if (strcmp(item_type, "Feature") == 0) {
			// Ignore rows until the row_offset is reached.

			if (row_offset > 0 && !filter_eval->HasExpressions()) {
				row_offset--;
				return;
			}

			// Collect the data of the Feature.

			ItemRow row;
			row.catalog = Value(catalog_id);

			// Extract id
			temp_val = yyjson_obj_get(json_val, "id");
			if (yyjson_is_str(temp_val)) {
				row.id = Value(yyjson_get_str(temp_val));
			} else {
				throw InvalidInputException("Missing 'id' field in the JSON Feature '%s'.", json_path.c_str());
			}

			// Extract geometry as WKT
			temp_val = yyjson_obj_get(json_val, "geometry");
			if (yyjson_is_obj(temp_val)) {
				row.geometry = JsonGeometry::ParseGeometryAsWKT(temp_val);
			} else {
				throw InvalidInputException("Missing 'geometry' field in the JSON Feature '%s'.", json_path.c_str());
			}

			// Extract bbox
			temp_val = yyjson_obj_get(json_val, "bbox");
			if (yyjson_is_arr(temp_val)) {
				row.bbox = JsonObject::ParseBoundingBoxObject(temp_val);
			} else {
				throw InvalidInputException("Missing 'bbox' field in the JSON Feature '%s'.", json_path.c_str());
			}

			// Extract stac_version
			temp_val = yyjson_obj_get(json_val, "stac_version");
			if (yyjson_is_str(temp_val)) {
				row.version = Value(yyjson_get_str(temp_val));
			} else {
				throw InvalidInputException("Missing 'stac_version' field in the JSON Feature '%s'.",
				                            json_path.c_str());
			}

			// Extract stac_extensions
			temp_val = yyjson_obj_get(json_val, "stac_extensions");
			if (yyjson_is_arr(temp_val)) {
				row.extensions = JsonObject::ParseExtensionsObject(temp_val);
			} else {
				throw InvalidInputException("Missing 'stac_extensions' field in the JSON Feature '%s'.",
				                            json_path.c_str());
			}

			// Extract links
			temp_val = yyjson_obj_get(json_val, "links");
			if (yyjson_is_arr(temp_val)) {
				row.links = JsonObject::ParseLinksObject(temp_val);
			} else {
				throw InvalidInputException("Missing 'links' field in the JSON Feature '%s'.", json_path.c_str());
			}

			// Extract assets
			temp_val = yyjson_obj_get(json_val, "assets");
			if (yyjson_is_obj(temp_val)) {
				row.assets = JsonObject::ParseAssetsObject(temp_val);
			} else {
				throw InvalidInputException("Missing 'assets' field in the JSON Feature '%s'.", json_path.c_str());
			}

			// Extract collection id (if present)
			temp_val = yyjson_obj_get(json_val, "collection");
			if (yyjson_is_str(temp_val)) {
				row.collection = Value(yyjson_get_str(temp_val));
			} else {
				row.collection = Value(collection_id);
			}

			// Extract properties (all other dynamic fields)
			temp_val = yyjson_obj_get(json_val, "properties");
			if (yyjson_is_obj(temp_val)) {
				yyjson_obj_iter iter;
				yyjson_obj_iter_init(temp_val, &iter);
				yyjson_val *key, *val;

				while ((key = yyjson_obj_iter_next(&iter))) {
					if (yyjson_is_str(key)) {
						val = yyjson_obj_iter_get_val(key);
						if (val) {
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
				}
			} else {
				throw InvalidInputException("Missing 'properties' field in the JSON Feature '%s'.", json_path.c_str());
			}

			// The filter expressions were evaluated but item does not match the conditions?
			if (!filter_eval->Eval(row)) {
				STAC_SCAN_DEBUG_LOG(1, " > id=(%s): item did not match filter conditions, skipped",
				                    row.id.ToString().c_str());
				return;
			}
			if (row_offset > 0 && filter_eval->HasExpressions()) {
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

		temp_val = yyjson_obj_get(json_val, "type");
		if (yyjson_is_str(temp_val)) {
			item_type = yyjson_get_str(temp_val);
		}
		if (!item_type || strlen(item_type) == 0) {
			temp_val = yyjson_obj_get(json_val, "collections");
			if (yyjson_is_arr(temp_val)) {
				item_type = "Collections";
			} else {
				throw InvalidInputException("Missing 'type' field in the JSON object '%s'.", json_path.c_str());
			}
		}

		if (strcmp(item_type, "Catalog") == 0) {
			std::string prev_catalog_id = catalog_id;

			temp_val = yyjson_obj_get(json_val, "id");
			if (yyjson_is_str(temp_val)) {
				catalog_id = yyjson_get_str(temp_val);
			}
			temp_val = yyjson_obj_get(json_val, "links");
			if (yyjson_is_arr(temp_val)) {
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
				coll_val = yyjson_arr_get(colls_val, i);
				if (yyjson_is_obj(coll_val)) {
					ReadContentOfObject(coll_val, json_path, ttl_seconds);
				}
			}
			temp_val = yyjson_obj_get(json_val, "links");
			if (yyjson_is_arr(temp_val)) {
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
			temp_val = yyjson_obj_get(json_val, "id");
			if (yyjson_is_str(temp_val)) {
				row.id = Value(yyjson_get_str(temp_val));
			} else {
				throw InvalidInputException("Missing 'id' field in the JSON Collection '%s'.", json_path.c_str());
			}

			// Extract title
			temp_val = yyjson_obj_get(json_val, "title");
			if (yyjson_is_str(temp_val)) {
				row.title = Value(yyjson_get_str(temp_val));
			}

			// Extract description
			temp_val = yyjson_obj_get(json_val, "description");
			if (yyjson_is_str(temp_val)) {
				row.description = Value(yyjson_get_str(temp_val));
			}

			// Extract keywords
			temp_val = yyjson_obj_get(json_val, "keywords");
			if (yyjson_is_arr(temp_val)) {
				row.keywords = JsonObject::ParseKeywordsObject(temp_val);
			}

			// Extract license
			temp_val = yyjson_obj_get(json_val, "license");
			if (yyjson_is_str(temp_val)) {
				row.license = Value(yyjson_get_str(temp_val));
			}

			// Extract providers
			temp_val = yyjson_obj_get(json_val, "providers");
			if (yyjson_is_arr(temp_val)) {
				row.providers = JsonObject::ParseGenericObject(temp_val);
			}

			// Extract bbox/interval (extent)
			temp_val = yyjson_obj_get(json_val, "extent");
			if (yyjson_is_obj(temp_val)) {
				yyjson_val *spatial_val = nullptr;
				yyjson_val *rngtime_val = nullptr;

				spatial_val = yyjson_obj_get(temp_val, "spatial");
				if (yyjson_is_obj(spatial_val)) {
					spatial_val = yyjson_obj_get(spatial_val, "bbox");
					if (yyjson_is_arr(spatial_val) && yyjson_arr_size(spatial_val) > 0) {
						yyjson_val *bbox_val = yyjson_arr_get(spatial_val, 0);
						row.bbox = JsonObject::ParseBoundingBoxObject(bbox_val);
					}
				}
				rngtime_val = yyjson_obj_get(temp_val, "temporal");
				if (yyjson_is_obj(rngtime_val)) {
					rngtime_val = yyjson_obj_get(rngtime_val, "interval");
					if (yyjson_is_arr(rngtime_val) && yyjson_arr_size(rngtime_val) > 0) {
						yyjson_val *interval_val = yyjson_arr_get(rngtime_val, 0);
						row.interval = JsonObject::ParseIntervalObject(interval_val);
					}
				}
			}

			// Extract summaries
			temp_val = yyjson_obj_get(json_val, "summaries");
			if (yyjson_is_obj(temp_val)) {
				row.summaries = JsonObject::ParseGenericObject(temp_val);
			}

			// Extract stac_version
			temp_val = yyjson_obj_get(json_val, "stac_version");
			if (yyjson_is_str(temp_val)) {
				row.version = Value(yyjson_get_str(temp_val));
			}

			// Extract stac_extensions
			temp_val = yyjson_obj_get(json_val, "stac_extensions");
			if (yyjson_is_arr(temp_val)) {
				row.extensions = JsonObject::ParseExtensionsObject(temp_val);
			}

			// Extract links
			temp_val = yyjson_obj_get(json_val, "links");
			if (yyjson_is_arr(temp_val)) {
				row.links = JsonObject::ParseLinksObject(temp_val);
			}

			// Extract assets
			temp_val = yyjson_obj_get(json_val, "assets");
			if (yyjson_is_obj(temp_val)) {
				row.assets = JsonObject::ParseAssetsObject(temp_val);
			}

			// Extract item assets
			temp_val = yyjson_obj_get(json_val, "item_assets");
			if (yyjson_is_obj(temp_val)) {
				row.item_assets = JsonObject::ParseGenericObject(temp_val);
			}

			rows.push_back(row);
			row_count++;

			// Next, extract the links if available.

			temp_val = yyjson_obj_get(json_val, "links");
			if (yyjson_is_arr(temp_val)) {
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
		// A MemoryStream buffer used for reading JSON content.
		MemoryStream buffer;
		// The schema of the STAC Catalog.
		ItemSchema schema;

		// Offset for the rows to be read.
		idx_t row_offset = 0;
		// Limit for the rows to be read (A value of 0 means no limit is applied).
		std::size_t row_limit = 0;

		// Optional search criteria for the STAC API item-search.
		SearchFilter search_filter;

		// Optional pushdown filter expressions for the STAC items.
		vector<unique_ptr<Expression>> filter_expressions;
		// All column types for the output of the table function, including dynamic fields.
		vector<LogicalType> column_types;

		explicit BindData(ClientContext &context) : buffer(Allocator::Get(context)), schema(context, buffer) {
		}
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<Identifier> &names,
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

		auto result = make_uniq<BindData>(context);
		auto json_str = ReadContentOfCatalog(context, result->buffer, catalog_path, search_filter, 30);
		result->schema.ParseSchemaOfObject("", "", json_str, catalog_path, 30);

		for (const auto &prop_name : result->schema.column_names) {
			names.emplace_back(prop_name);
		}
		for (const auto &prop_type : result->schema.column_types) {
			return_types.emplace_back(prop_type);
		}

		// Return the bind data.

		result->catalog_path = std::move(catalog_path);
		result->column_types = return_types;
		result->search_filter = search_filter;
		result->row_limit = 0;
		result->row_offset = 0;

		return std::move(result);
	}

	static unique_ptr<FunctionData> BindRead(ClientContext &context, TableFunctionBindInput &input,
	                                         vector<LogicalType> &return_types, vector<Identifier> &names) {
		return Bind(context, input, return_types, names, SearchFilter());
	}

	//------------------------------------------------------------------------------------------------------------------
	// Init Global
	//------------------------------------------------------------------------------------------------------------------

	struct State final : GlobalTableFunctionState {
		vector<column_t> column_ids;
		vector<unique_ptr<Expression>> filter_expressions;

		// A MemoryStream buffer used for reading JSON content.
		MemoryStream buffer;
		// The reader of the STAC Catalog.
		unique_ptr<ItemReader> reader;

		// Total number of items matched by the filter (if any) in the Catalog.
		int number_matched = -1;

		State(ClientContext &context, const vector<column_t> &column_ids)
		    : column_ids(column_ids), buffer(Allocator::Get(context)) {
		}
	};

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
		const auto &bind_data = input.bind_data->Cast<BindData>();

		// Capture the final projected column IDs here, after all optimizer passes.
		// input.column_ids is guaranteed to match output.data.size() in Execute.
		auto gstate = make_uniq<State>(context, input.column_ids);

		for (const auto &expr : bind_data.filter_expressions) {
			gstate->filter_expressions.push_back(expr->Copy());
		}

		// The filter expressions built in PushdownComplexFilter reference columns by their absolute table
		// column index (since the final projection wasn't known yet at that point). Remap them now to the
		// position within the final projected column list, since that is what FilterEval's input DataChunk
		// is built with (see filter_eval.cpp).
		if (!gstate->filter_expressions.empty()) {
			unordered_map<idx_t, idx_t> table_col_to_position;

			for (idx_t i = 0; i < gstate->column_ids.size(); i++) {
				table_col_to_position[gstate->column_ids[i]] = i;
			}
			for (auto &expr : gstate->filter_expressions) {
				ExpressionIterator::VisitExpressionMutable<BoundReferenceExpression>(
				    expr, [&table_col_to_position](BoundReferenceExpression &bound_ref, unique_ptr<Expression> &) {
					    const auto entry = table_col_to_position.find(bound_ref.Index());
					    if (entry == table_col_to_position.end()) {
						    throw InternalException("STAC_Read: filter column was pruned from the projected columns");
					    }
					    bound_ref.IndexMutable() = entry->second;
				    });
			}
		}

		// Read the first page of the Catalog content.

		STAC_SCAN_DEBUG_LOG(1, "Reading first page of the Catalog '%s'...", bind_data.catalog_path.c_str());

		const std::string &catalog_path = bind_data.catalog_path;
		const ItemSchema &schema = bind_data.schema;
		const auto &column_types = bind_data.column_types;
		auto filter_eval = make_uniq<FilterEval>(context, gstate->filter_expressions, gstate->column_ids, column_types);

		const auto row_offset = bind_data.row_offset;
		const auto row_limit = bind_data.row_limit;
		auto &buffer = gstate->buffer;
		auto reader = make_uniq<ItemReader>(context, buffer, schema, std::move(filter_eval), row_offset, row_limit);

		auto json_str = ReadContentOfCatalog(context, buffer, catalog_path, bind_data.search_filter, 30);
		reader->ReadContentOfObject(json_str, catalog_path, 30);

		// Set the number of items matched by the filter (if any) in the Catalog.

		int32_t number_matched = bind_data.schema.GetNumberMatched();
		if (number_matched == -1) {
			number_matched = reader->GetNumberMatched();
		}
		gstate->number_matched = number_matched;

		STAC_SCAN_DEBUG_LOG(1, "Request matched %d items", gstate->number_matched);

		if (gstate->number_matched > 2048 && bind_data.row_limit == 0) {
			fprintf(stderr,
			        "STAC: Warning, the request matched %d items, consider refining the query to get fewer results!\n",
			        gstate->number_matched);
		}

		// Return the global state with the reader.

		gstate->reader = std::move(reader);
		return std::move(gstate);
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

					if (StringUtil::Lower(get.function.GetName().GetIdentifierName()) == "stac_read") {
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
					    const auto &return_type = col_ref.GetReturnType();
					    const idx_t table_col = column_ids[col_ref.Binding().column_index].GetPrimaryIndex();
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
		auto &gstate = input.global_state->Cast<State>();

		ItemReader *reader = gstate.reader.get();
		idx_t row_idx = 0;

		// Load pending rows from the reader into the output chunk.

		for (idx_t i = 0; i < reader->rows.size(); i++, row_idx++) {
			const auto &item_row = reader->rows[i];

			if (row_idx >= STANDARD_VECTOR_SIZE - 1) {
				reader->rows.erase(reader->rows.begin(),
				                   reader->rows.begin() + static_cast<vector<ItemRow>::difference_type>(i));
				output.SetChildCardinality(row_idx + 1);
				return;
			}
			for (idx_t col_idx = 0; col_idx < gstate.column_ids.size(); col_idx++) {
				const idx_t dim_index = gstate.column_ids[col_idx];
				const Value &value = item_row.ValueOf(dim_index);
				output.data[col_idx].SetValue(row_idx, value);
			}
		}
		reader->rows.clear();

		// Load additional rows from the next pages of the catalog if available.

		while (reader->ReadNextPageOfResults(0)) {
			for (idx_t i = 0; i < reader->rows.size(); i++, row_idx++) {
				const auto &item_row = reader->rows[i];

				if (row_idx >= STANDARD_VECTOR_SIZE - 1) {
					reader->rows.erase(reader->rows.begin(),
					                   reader->rows.begin() + static_cast<vector<ItemRow>::difference_type>(i));
					output.SetChildCardinality(row_idx + 1);
					return;
				}
				for (idx_t col_idx = 0; col_idx < gstate.column_ids.size(); col_idx++) {
					const idx_t dim_index = gstate.column_ids[col_idx];
					const Value &value = item_row.ValueOf(dim_index);
					output.data[col_idx].SetValue(row_idx, value);
				}
			}
			reader->rows.clear();
		}

		// Set the cardinality of the output.
		output.SetChildCardinality(row_idx);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Progress Scan
	//------------------------------------------------------------------------------------------------------------------

	static double Progress(ClientContext &context, const FunctionData *bind_data_p,
	                       const GlobalTableFunctionState *global_state) {
		auto &gstate = global_state->Cast<State>();

		const ItemReader *reader = gstate.reader.get();
		int32_t number_matched = gstate.number_matched;
		std::size_t current_row = reader->GetRowCount();

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
		FunctionSignature signature;
		signature.AddParameter("catalog", LogicalType::VARCHAR);

		TableFunction func("STAC_Read", std::move(signature), Execute, BindRead, Init);

		// Enable progress reporting - allows DuckDB to report the progress of the table scan
		func.table_scan_progress = Progress;

		// Enable projection pushdown - allows DuckDB to tell us which columns are needed
		// The column_ids will be passed to InitGlobal via TableFunctionInitInput
		func.projection_pushdown = true;

		// Enable complex filter pushdown - handles expressions like (A AND B) OR (C AND D)
		// that cannot be represented as simple TableFilter objects
		func.pushdown_complex_filter = PushdownComplexFilter;

		loader.RegisterFunction(func);

		// Add documentation & tags for the table function
		InsertionOrderPreservingMap<string> tags;
		tags.insert("ext", "stac");
		tags.insert("category", "table");
		FunctionBuilder::AddTableFunctionDocs(loader, "STAC_Read", DESCRIPTION, EXAMPLE, tags);

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
	                                           vector<LogicalType> &return_types, vector<Identifier> &names) {
		SearchFilter search_filter;

		// Parse the named parameters for the STAC Search API filter.

		const named_argument_map_t &named_params = input.named_parameters;

		auto input_param = named_params.find("collections");
		if (input_param != named_params.end()) {
			for (auto &param : ListValue::GetChildren(input_param->second)) {
				search_filter.collections.emplace_back(StringValue::Get(param).c_str());
			}
		}

		input_param = named_params.find("ids");
		if (input_param != named_params.end()) {
			for (auto &param : ListValue::GetChildren(input_param->second)) {
				search_filter.ids.emplace_back(StringValue::Get(param).c_str());
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
		FunctionSignature signature;
		signature.AddParameter("catalog", LogicalType::VARCHAR).WithTypedKwargs("options", [&](TypedKwargs &options) {
			options.Add("collections", LogicalType::LIST(LogicalType::VARCHAR))
			    .Add("ids", LogicalType::LIST(LogicalType::VARCHAR))
			    .Add("datetime", LogicalType::VARCHAR)
			    .Add("bbox", LogicalType::LIST(LogicalType::DOUBLE))
			    .Add("intersects", LogicalType::GEOMETRY("EPSG:4326"))
			    .Add("filter", LogicalType::VARCHAR)
			    .Add("filter_lang", LogicalType::VARCHAR)
			    .Add("fields", LogicalType::VARCHAR)
			    .Add("sortby", LogicalType::VARCHAR)
			    .Add("max_items", LogicalType::INTEGER);
		});

		TableFunction func("STAC_Search", std::move(signature), Execute, BindSearch, Init);

		// Enable progress reporting - allows DuckDB to report the progress of the table scan
		func.table_scan_progress = Progress;

		// Enable projection pushdown - allows DuckDB to tell us which columns are needed
		// The column_ids will be passed to InitGlobal via TableFunctionInitInput
		func.projection_pushdown = true;

		// Enable complex filter pushdown - handles expressions like (A AND B) OR (C AND D)
		// that cannot be represented as simple TableFilter objects
		func.pushdown_complex_filter = PushdownComplexFilter;

		loader.RegisterFunction(func);

		// Add documentation & tags for the table function
		InsertionOrderPreservingMap<string> tags;
		tags.insert("ext", "stac");
		tags.insert("category", "table");
		FunctionBuilder::AddTableFunctionDocs(loader, "STAC_Search", DESCRIPTION, EXAMPLE, tags);

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
	                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
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
		vector<column_t> column_ids;
		idx_t current_idx;

		explicit State(vector<column_t> column_ids) : column_ids(std::move(column_ids)), current_idx(0) {
		}
	};

	static unique_ptr<GlobalTableFunctionState> Init(ClientContext &context, TableFunctionInitInput &input) {
		// Capture the final projected column IDs here, after all optimizer passes.
		// input.column_ids is guaranteed to match output.data.size() in Execute.
		return make_uniq_base<GlobalTableFunctionState, State>(input.column_ids);
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

			for (idx_t col_idx = 0; col_idx < gstate.column_ids.size(); col_idx++) {
				const idx_t dim_index = gstate.column_ids[col_idx];
				const Value &value = collection_row.ValueOf(dim_index);
				output.data[col_idx].SetValue(count, value);
			}
			count++;
		}

		output.SetChildCardinality(count);
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
		FunctionSignature signature;
		signature.AddParameter("catalog", LogicalType::VARCHAR);

		TableFunction func("STAC_Collections", std::move(signature), Execute, Bind, Init);

		// Enable projection pushdown - allows DuckDB to tell us which columns are needed
		// The column_ids will be passed to InitGlobal via TableFunctionInitInput
		func.projection_pushdown = true;

		loader.RegisterFunction(func);

		// Add documentation & tags for the table function
		InsertionOrderPreservingMap<string> tags;
		tags.insert("ext", "stac");
		tags.insert("category", "table");
		FunctionBuilder::AddTableFunctionDocs(loader, "STAC_Collections", DESCRIPTION, EXAMPLE, tags);
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
