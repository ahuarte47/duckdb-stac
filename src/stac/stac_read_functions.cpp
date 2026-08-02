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
#include "http_request.hpp"
#include "json_geometry.hpp"
#include "json_object.hpp"
#include "search_filter.hpp"
#include "xml_element.hpp"

namespace duckdb {

namespace {

//======================================================================================================================
// Utility types and functions
//======================================================================================================================

static constexpr const char *XML_ERROR_PATH = "/S:Fault/faultstring";

//! Extracts the error message of a given HTTP response body.
static std::string GetXmlErrorMessage(const std::string &response_body) {
	XmlDocument document = XmlDocument(response_body);
	xmlXPathContextPtr xpath_ctx = document.GetXPathContext();
	xmlXPathObjectPtr xpath_obj = nullptr;

	std::string error_msg;

	if ((xpath_obj = xmlXPathEvalExpression(BAD_CAST XML_ERROR_PATH, xpath_ctx)) && xpath_obj->nodesetval &&
	    xpath_obj->nodesetval->nodeNr > 0) {
		xmlNodePtr node = xpath_obj->nodesetval->nodeTab[0];
		error_msg = XmlUtils::GetNodeTextContent(node);
	}
	if (xpath_obj) {
		xmlXPathFreeObject(xpath_obj);
		xpath_obj = nullptr;
	}

	return error_msg;
}

//! Executes an HTTP request and returns the response body as a string.
static std::string ExecuteHttpRequest(ClientContext &context, const std::string &url, const std::string &method,
                                      const HttpHeaders &headers, const std::string &body,
                                      const std::string &content_type) {
	HttpSettings settings;
	settings = HttpRequest::ExtractHttpSettings(context, url);
	settings.timeout = 30;

	HttpResponseData response = HttpRequest::ExecuteHttpRequest(settings, url, method, headers, body, content_type);

	// Handle the HTTP response and check for errors.
	if (response.content_type == "application/xml") {
		std::string error_msg = GetXmlErrorMessage(response.body);

		STAC_SCAN_DEBUG_LOG(1, "Failed to fetch the Catalog '%s': (%d) %s", url.c_str(), response.status_code,
		                    error_msg.c_str());

		throw IOException("Failed to fetch the Catalog '%s': (%d) %s", url.c_str(), response.status_code,
		                  error_msg.c_str());
	}
	if (response.status_code != 200) {
		throw IOException("Failed to fetch the Catalog '%s': (%d) %s", url.c_str(), response.status_code,
		                  response.error.c_str());
	}
	if (!response.error.empty()) {
		throw IOException(response.error);
	}
	return response.body;
}

//! Determines whether the given catalog path is a static STAC Catalog (JSON file) or a dynamic STAC Catalog (URL).
static bool IsStaticCatalog(const std::string &catalog_path) {
	std::string l_path = StringUtil::Lower(catalog_path);
	return StringUtil::EndsWith(l_path, ".json") || StringUtil::EndsWith(l_path, ".geojson");
}

//! Reads the content of a JSON file and returns it as a string.
static std::string ReadContentOfJsonFile(ClientContext &context, MemoryStream &buffer, const std::string &file_path) {
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

//! Reads the content of a JSON catalog and returns it as a string.
static std::string ReadContentOfCatalog(ClientContext &context, MemoryStream &buffer, const std::string &catalog_path,
                                        const SearchFilter &filter) {
	if (IsStaticCatalog(catalog_path)) {
		return ReadContentOfJsonFile(context, buffer, catalog_path);
	} else if (filter.IsEmpty()) {
		return ExecuteHttpRequest(context, catalog_path, "GET", HttpHeaders(), "", "application/json");
	} else {
		std::string q = filter.AsQueryJson();
		return ExecuteHttpRequest(context, catalog_path, "POST", HttpHeaders(), q, "application/json");
	}
}

//======================================================================================================================
// STAC Schema definitions
//======================================================================================================================

//! Manages the schema definition of a set of STAC Items in a Catalog.
class ItemSchema {
private:
	//! The client context for the current query execution.
	ClientContext &context;
	//! The buffer used to read JSON content.
	MemoryStream &buffer;

public:
	//! The set of type names (A type is represented by the join of a catalog and collection identifiers).
	std::set<std::string> itemtype_set;
	//! The set of property names (Set of properties and their corresponding index in the column vector).
	std::map<std::string, int16_t> property_set;
	//! All the column names of the Catalog and its child Catalogs.
	std::vector<std::string> column_names;
	//! All the column types of the Catalog and its child Catalogs.
	std::vector<LogicalType> column_types;

public:
	//! Constructor for the ItemSchema class.
	ItemSchema(ClientContext &context, MemoryStream &buffer) : context(context), buffer(buffer) {
	}

	//! Clears the definition of the schema.
	void Clear() {
		itemtype_set.clear();
		property_set.clear();
		column_names.clear();
		column_types.clear();
	}

	//! Parses a STAC JSON links array to extract the schema of child STAC items recursively.
	void ParseSchemaOfJsonLinks(std::string catalog_id, std::string collection_id, yyjson_val *links_val,
	                            const std::string &links_path) {
		yyjson_val *temp_val = nullptr;
		std::size_t links_size = yyjson_arr_size(links_val);
		yyjson_val *link_val = nullptr;
		std::size_t item_count = 0;

		for (std::size_t i = 0; i < links_size; i++) {
			if (yyjson_is_obj(link_val = yyjson_arr_get(links_val, i))) {
				const char *href_val = nullptr;
				const char *rel_type = nullptr;

				// Check required fields in the link object.

				if (yyjson_is_str(temp_val = yyjson_obj_get(link_val, "href"))) {
					href_val = yyjson_get_str(temp_val);
				}
				if (!href_val || strlen(href_val) == 0) {
					continue; // Skip links without a "href" field.
				}
				if (yyjson_is_str(temp_val = yyjson_obj_get(link_val, "rel"))) {
					rel_type = yyjson_get_str(temp_val);
				}
				if (!rel_type || strlen(rel_type) == 0) {
					continue; // Skip links without a "rel" field.
				}

				// To extract the schema at this level, we only need to parse the first item.

				if (strcmp(rel_type, "item") == 0) {
					if (item_count > 0) {
						continue;
					}
					item_count++;
				}

				// Parse the child JSON item recursively.

				if (strcmp(rel_type, "item") == 0 || strcmp(rel_type, "child") == 0 || strcmp(rel_type, "items") == 0) {
					std::string href = std::string(href_val);

					// Is the href a relative path? If so, resolve it relative to the object path.
					auto href_path = Path::FromString(href);
					if (!href_path.IsAbsolute() && !href_path.HasScheme()) {
						auto parent_dir = Path::FromString(links_path).Parent();
						href = parent_dir.Join(href_path).ToString();
					}

					std::string href_str = ReadContentOfJsonFile(context, buffer, href);
					ParseSchemaOfJsonObject(catalog_id, collection_id, href_str, href);
				}
			}
		}
	}

	//! Parses a STAC JSON object to extract the schema of child STAC items recursively.
	void ParseSchemaOfJsonObject(std::string catalog_id, std::string collection_id, yyjson_val *json_val,
	                             const std::string &json_path) {
		yyjson_val *temp_val = nullptr;
		const char *item_type = nullptr;

		// Handle data of a STAC Catalog, Collection or Feature...

		if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "type"))) {
			item_type = yyjson_get_str(temp_val);
		}
		if (!item_type || strlen(item_type) == 0) {
			throw InvalidInputException("Missing 'type' field in the JSON object '%s'.", json_path.c_str());
		}

		if (strcmp(item_type, "Catalog") == 0) {
			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "id"))) {
				catalog_id = yyjson_get_str(temp_val);
			}
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "links"))) {
				ParseSchemaOfJsonLinks(catalog_id, collection_id, temp_val, json_path);
			}
			return;
		}
		if (strcmp(item_type, "Collection") == 0) {
			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "id"))) {
				collection_id = yyjson_get_str(temp_val);
			}
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "links"))) {
				ParseSchemaOfJsonLinks(catalog_id, collection_id, temp_val, json_path);
			}
			return;
		}
		if (strcmp(item_type, "FeatureCollection") == 0) {
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "features"))) {
				std::size_t features_size = yyjson_arr_size(temp_val);

				for (std::size_t i = 0; i < features_size; i++) {
					yyjson_val *feature_val = yyjson_arr_get(temp_val, i);

					if (yyjson_is_obj(feature_val)) {
						ParseSchemaOfJsonObject(catalog_id, collection_id, feature_val, json_path);
						break; // Only need to parse the first feature to extract the schema.
					}
				}
			}
			return;
		}
		if (strcmp(item_type, "Feature") == 0) {
			// Extract collection id (if present)
			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "collection"))) {
				collection_id = yyjson_get_str(temp_val);
			}

			std::string feature_type = catalog_id + "/" + collection_id;

			// Feature type already processed? If so, skip it to avoid reprocessing the feature type.
			auto it = itemtype_set.find(feature_type);
			if (it != itemtype_set.end()) {
				return;
			}
			itemtype_set.insert(feature_type);

			// Extract schema of properties (all other dynamic fields)
			if (yyjson_is_obj(temp_val = yyjson_obj_get(json_val, "properties"))) {
				yyjson_obj_iter iter;
				yyjson_obj_iter_init(temp_val, &iter);
				yyjson_val *key, *val;

				while ((key = yyjson_obj_iter_next(&iter))) {
					if (yyjson_is_str(key) && (val = yyjson_obj_iter_get_val(key))) {
						std::string key_str = yyjson_get_str(key);

						// New property? If so, add it to the schema.
						auto it = property_set.find(key_str);
						if (it == property_set.end()) {
							idx_t key_idx = static_cast<idx_t>(column_names.size());
							property_set[key_str] = key_idx;
							column_names.emplace_back(key_str);
							column_types.emplace_back(JsonObject::GetPropertyTypeOfJsonValue(val));
						}
					}
				}
			} else {
				throw InvalidInputException("Missing 'properties' field in the JSON Feature '%s'.", json_path.c_str());
			}
		}
	}

	//! Parses a STAC JSON object to extract the schema of child STAC items recursively.
	void ParseSchemaOfJsonObject(std::string catalog_id, std::string collection_id, const std::string &json_str,
	                             const std::string &json_path) {
		yyjson_doc *json_data = yyjson_read(json_str.c_str(), json_str.size(), YYJSON_READ_NOFLAG);
		if (!json_data) {
			throw IOException("Failed to parse data of the object '%s'.", json_path.c_str());
		}

		try {
			yyjson_val *root_val = yyjson_doc_get_root(json_data);
			if (!root_val) {
				throw IOException("Failed to get the root value of the JSON object '%s'.", json_path.c_str());
			}

			ParseSchemaOfJsonObject(catalog_id, collection_id, root_val, json_path);

			// Make sure to free the JSON document
			yyjson_doc_free(json_data);
		} catch (...) {
			// Make sure to free the JSON document in case of an exception
			yyjson_doc_free(json_data);
			throw;
		}
	}
};

//======================================================================================================================
// STAC Catalog reader
//======================================================================================================================

//! Reads the content of the set STAC items in a Catalog.
class ItemReader {
private:
	//! The client context for the current query execution.
	ClientContext &context;
	//! The buffer used to read JSON content.
	MemoryStream &buffer;

	//! The Catalog identifier read so far.
	std::string catalog_id;
	//! The Collection identifier read so far.
	std::string collection_id;

private:
	//! The filter context for pushdown filtering, if defined.
	const FilterContext filter_context;

	//! Offset to be applied for the rows when reading items.
	idx_t row_offset = 0;
	//! Limit for the rows to be read (A value of 0 means no limit is applied).
	std::size_t row_limit = 0;
	//! Total number of rows read so far by the ItemReader.
	std::size_t row_count = 0;
	// Total number of items matched by the filter (if any) in the Catalog.
	int number_matched = -1;

public:
	//! The schema of the Catalog.
	const ItemSchema &schema;

	//! The next href for the next page of results, if any.
	std::string next_href;
	//! The next method for the next page of results, if any.
	std::string next_method = "GET";
	//! The next headers for the next page of results, if any.
	HttpHeaders next_headers;
	//! The next body for the next page of results, if any.
	std::string next_body;
	//! The headers/body in the next link must be merged into the original request
	//! and be sent combined in the next request.
	bool next_merge = false;

	//! Set of item rows already extracted.
	std::vector<ItemRow> rows;

public:
	//! Constructor for the ItemReader class.
	ItemReader(ClientContext &context, MemoryStream &buffer, const ItemSchema &schema,
	           const FilterContext &filter_context, idx_t row_offset = 0, std::size_t row_limit = 0)
	    : context(context), buffer(buffer), filter_context(std::move(filter_context)), row_offset(row_offset),
	      row_limit(row_limit), schema(schema) {
	}

	//! Returns the total number of items matched by the filter (if any) in the Catalog.
	int GetNumberMatched() const {
		return number_matched;
	}

	//! Returns the total number of rows read so far by the ItemReader.
	std::size_t GetRowCount() const {
		return row_count;
	}

	//! Reads the content of a STAC JSON links array to extract the child STAC items.
	void ReadContentOfJsonLinks(yyjson_val *links_val, const std::string &links_path) {
		yyjson_val *temp_val = nullptr;
		std::size_t links_size = yyjson_arr_size(links_val);
		yyjson_val *link_val = nullptr;

		for (std::size_t i = 0; i < links_size; i++) {
			if (yyjson_is_obj(link_val = yyjson_arr_get(links_val, i))) {
				const char *href_val = nullptr;
				const char *rel_type = nullptr;

				// Stop processing links if the limit is reached.

				if (row_limit > 0 && row_count >= row_limit) {
					next_href.clear();
					return;
				}

				// Check required fields in the link object.

				if (yyjson_is_str(temp_val = yyjson_obj_get(link_val, "href"))) {
					href_val = yyjson_get_str(temp_val);
				}
				if (!href_val || strlen(href_val) == 0) {
					continue; // Skip links without a "href" field.
				}
				if (yyjson_is_str(temp_val = yyjson_obj_get(link_val, "rel"))) {
					rel_type = yyjson_get_str(temp_val);
				}
				if (!rel_type || strlen(rel_type) == 0) {
					continue; // Skip links without a "rel" field.
				}

				// If the link is a "next" link, store its href for pagination.

				if (strcmp(rel_type, "next") == 0) {
					next_href = std::string(href_val);

					if (yyjson_is_str(temp_val = yyjson_obj_get(link_val, "method"))) {
						next_method = yyjson_get_str(temp_val);
					} else {
						next_method = "GET";
					}
					if (yyjson_is_bool(temp_val = yyjson_obj_get(link_val, "merge"))) {
						next_merge = yyjson_get_bool(temp_val);
					} else {
						next_merge = false;
					}
					if (!next_merge) {
						next_headers.clear();
						next_body.clear();
					}
					if (yyjson_is_obj(temp_val = yyjson_obj_get(link_val, "headers"))) {
						yyjson_obj_iter iter;
						yyjson_obj_iter_init(temp_val, &iter);
						yyjson_val *key, *val;

						while ((key = yyjson_obj_iter_next(&iter))) {
							if (yyjson_is_str(key) && (val = yyjson_obj_iter_get_val(key)) && yyjson_is_str(val)) {
								std::string key_str = yyjson_get_str(key);
								std::string val_str = yyjson_get_str(val);
								next_headers[key_str] = val_str;
							}
						}
					}
					if (yyjson_is_obj(temp_val = yyjson_obj_get(link_val, "body"))) {
						if (next_merge && !next_body.empty()) {
							throw NotImplementedException(
							    "The 'body' field in the 'next' link is not supported when 'merge' is true.");
						}
						char *json_str = yyjson_val_write(temp_val, YYJSON_WRITE_NOFLAG, nullptr);
						if (json_str) {
							next_body = std::string(json_str);
							free(json_str);
						} else {
							next_body.clear();
						}
					}
					continue;
				}

				// Parse the child JSON item recursively.

				if (strcmp(rel_type, "item") == 0 || strcmp(rel_type, "child") == 0 || strcmp(rel_type, "items") == 0) {
					std::string href = std::string(href_val);

					// Is the href a relative path? If so, resolve it relative to the object path.
					auto href_path = Path::FromString(href);
					if (!href_path.IsAbsolute() && !href_path.HasScheme()) {
						auto parent_dir = Path::FromString(links_path).Parent();
						href = parent_dir.Join(href_path).ToString();
					}

					std::string href_str = ReadContentOfJsonFile(context, buffer, href);
					ReadContentOfJsonObject(href_str, href);
				}
			}
		}
	}

	//! Reads the content of a STAC JSON object to extract the child STAC items.
	void ReadContentOfJsonObject(yyjson_val *json_val, const std::string &json_path) {
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
			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "id"))) {
				catalog_id = yyjson_get_str(temp_val);
			}
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "links"))) {
				ReadContentOfJsonLinks(temp_val, json_path);
			}
			return;
		}
		if (strcmp(item_type, "Collection") == 0) {
			if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "id"))) {
				collection_id = yyjson_get_str(temp_val);
			}
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "links"))) {
				ReadContentOfJsonLinks(temp_val, json_path);
			}
			return;
		}
		if (strcmp(item_type, "FeatureCollection") == 0) {
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
						ReadContentOfJsonObject(feature_val, json_path);
					}
				}
			}
			if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "links"))) {
				ReadContentOfJsonLinks(temp_val, json_path);
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
							throw InvalidInputException(
							    "Property '%s' not found in the schema for the JSON Feature '%s'.", key_str.c_str(),
							    json_path.c_str());
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
				STAC_SCAN_DEBUG_LOG(3, " > id=(%s): item did not match filter conditions, skipped",
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

	//! Reads the content of a STAC JSON object to extract the child STAC items.
	void ReadContentOfJsonObject(const std::string &json_str, const std::string &json_path) {
		yyjson_doc *json_data = yyjson_read(json_str.c_str(), json_str.size(), YYJSON_READ_NOFLAG);
		if (!json_data) {
			throw IOException("Failed to parse data of the JSON object '%s'.", json_path.c_str());
		}

		try {
			yyjson_val *root_val = yyjson_doc_get_root(json_data);
			if (!root_val) {
				throw IOException("Failed to get the root value of the JSON object '%s'.", json_path.c_str());
			}

			ReadContentOfJsonObject(root_val, json_path);

			// Make sure to free the JSON document
			yyjson_doc_free(json_data);
		} catch (...) {
			// Make sure to free the JSON document in case of an exception
			yyjson_doc_free(json_data);
			throw;
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
		vector<std::unique_ptr<Expression>> filter_expressions;
		// All column types for the output of the table function, including dynamic fields.
		vector<LogicalType> column_types;

		explicit BindData(ItemSchema &&schema, MemoryStream &&buffer)
		    : schema(std::move(schema)), buffer(std::move(buffer)) {
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

		ItemSchema schema {context, buffer};
		auto json_str = ReadContentOfCatalog(context, buffer, catalog_path, SearchFilter());
		schema.ParseSchemaOfJsonObject("", "", json_str, catalog_path);

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
		result->row_limit = 0;
		result->row_offset = 0;

		return std::move(result);
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

		// Read the first page of the Catalog content.

		const std::string &catalog_path = bind_data.catalog_path;
		const ItemSchema &schema = bind_data.schema;
		MemoryStream &buffer = bind_data.buffer;

		const auto &filter_expressions = bind_data.filter_expressions;
		const auto &column_ids = bind_data.column_ids;
		const auto &column_types = bind_data.column_types;
		const FilterContext filter_context(context, filter_expressions, column_ids, column_types);

		ItemReader reader(context, buffer, schema, filter_context, bind_data.row_offset, bind_data.row_limit);
		auto json_str = ReadContentOfCatalog(context, buffer, catalog_path, bind_data.search_filter);
		reader.ReadContentOfJsonObject(json_str, catalog_path);

		// Set the number of items matched by the filter (if any) in the Catalog.

		bind_data.number_matched = reader.GetNumberMatched();

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
			vector<std::unique_ptr<Expression>> temp_expressions;

			for (const auto &expr : expressions) {
				auto expr_copy = expr->Copy();

				// We need to convert the column references in the filter expressions from BoundColumnRefExpression
				// to BoundReferenceExpression, so that one ExpressionExecutor can execute them during scanning.
				ExpressionIterator::VisitExpressionClassMutable(
				    expr_copy, ExpressionClass::BOUND_COLUMN_REF, [](unique_ptr<Expression> &child) {
					    const auto &col_ref = child->Cast<BoundColumnRefExpression>();
					    const auto &column_alias = col_ref.GetAlias();
					    const auto &column_index = col_ref.binding.column_index;
					    const auto &return_type = col_ref.return_type;
					    child = make_uniq<BoundReferenceExpression>(column_alias, return_type, column_index);
				    });

				temp_expressions.push_back(std::move(expr_copy));
			}
			bind_data.filter_expressions = std::move(temp_expressions);
			expressions.clear();
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

		while (!reader.next_href.empty()) {
			std::string href = reader.next_href;
			reader.next_href.clear();

			std::string &method = reader.next_method;
			HttpHeaders &headers = reader.next_headers;
			std::string &body = reader.next_body;
			std::string content_type = "application/json";

			auto json_str = ExecuteHttpRequest(context, href, method, headers, body, content_type);
			reader.ReadContentOfJsonObject(json_str, href);

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

		TableFunction func("STAC_Read", {LogicalType::VARCHAR}, Execute, Bind, Init);

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
		auto result = STAC_Read::Bind(context, input, return_types, names);
		auto &bind_data = result->Cast<BindData>();

		SearchFilter &search_filter = bind_data.search_filter;

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

		input_param = named_params.find("max_items");
		if (input_param != named_params.end()) {
			search_filter.limit = MaxValue<int32_t>(IntegerValue::Get(input_param->second), 0);
		}

		return result;
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

} // namespace

// #####################################################################################################################
// Register Read Functions
// #####################################################################################################################

void STACReadFunctions::Register(ExtensionLoader &loader) {
	STAC_Read::Register(loader);
	STAC_Search::Register(loader);
}

} // namespace duckdb
