#include "stac_reader.hpp"
#include "stac_schema.hpp"
#include "stac_types.hpp"
#include "json_object.hpp"

// DuckDB
#include "duckdb/common/path.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

namespace duckdb {

//======================================================================================================================
// STAC Item Schema
//======================================================================================================================

ItemSchema::ItemSchema(ClientContext &context, MemoryStream &buffer) : context(context), buffer(buffer) {
}

void ItemSchema::Clear() {
	itemtype_set.clear();
	property_set.clear();
	column_names.clear();
	column_types.clear();
	number_matched = -1;
}

void ItemSchema::ParseSchemaOfLinks(std::string catalog_id, std::string collection_id, yyjson_val *links_val,
                                    const std::string &links_path, int32_t ttl_seconds) {
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

			// Parse the child JSON items recursively.

			if (strcmp(rel_type, "child") == 0 || strcmp(rel_type, "item") == 0 || strcmp(rel_type, "items") == 0) {
				std::string href = std::string(href_val);

				// Is the href a relative path? If so, resolve it relative to the object path.
				auto href_path = Path::FromString(href);
				if (!href_path.IsAbsolute() && !href_path.HasScheme()) {
					auto parent_dir = Path::FromString(links_path).Parent();
					href = parent_dir.Join(href_path).ToString();
				}

				std::string href_str = ReadContentOfCatalog(context, buffer, href, SearchFilter(), ttl_seconds);
				ParseSchemaOfObject(catalog_id, collection_id, href_str, href, ttl_seconds);
			}
		}
	}
}

void ItemSchema::ParseSchemaOfObject(std::string catalog_id, std::string collection_id, yyjson_val *json_val,
                                     const std::string &json_path, int32_t ttl_seconds) {
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
			ParseSchemaOfLinks(catalog_id, collection_id, temp_val, json_path, ttl_seconds);
		}
		return;
	}
	if (strcmp(item_type, "Collection") == 0) {
		if (yyjson_is_str(temp_val = yyjson_obj_get(json_val, "id"))) {
			collection_id = yyjson_get_str(temp_val);
		}
		if (yyjson_is_arr(temp_val = yyjson_obj_get(json_val, "links"))) {
			ParseSchemaOfLinks(catalog_id, collection_id, temp_val, json_path, ttl_seconds);
		}
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

				if (yyjson_is_obj(feature_val)) {
					ParseSchemaOfObject(catalog_id, collection_id, feature_val, json_path, ttl_seconds);
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

void ItemSchema::ParseSchemaOfObject(std::string catalog_id, std::string collection_id, const std::string &json_str,
                                     const std::string &json_path, int32_t ttl_seconds) {
	yyjson_doc *json_data = yyjson_read(json_str.c_str(), json_str.size(), YYJSON_READ_NOFLAG);
	if (!json_data) {
		throw IOException("Failed to parse data of the object '%s'.", json_path.c_str());
	}

	try {
		yyjson_val *root_val = yyjson_doc_get_root(json_data);
		if (!root_val) {
			throw IOException("Failed to get the root value of the JSON object '%s'.", json_path.c_str());
		}

		ParseSchemaOfObject(catalog_id, collection_id, root_val, json_path, ttl_seconds);

		// Make sure to free the JSON document
		yyjson_doc_free(json_data);
	} catch (...) {
		// Make sure to free the JSON document in case of an exception
		yyjson_doc_free(json_data);
		throw;
	}
}

} // namespace duckdb
