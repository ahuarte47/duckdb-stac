#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! Manages the schema definition of a set of items in a STAC Catalog.
class ItemSchema {
private:
	//! The client context for the current query execution.
	ClientContext &context;
	//! The buffer used to read JSON content.
	MemoryStream &buffer;

	//! Total number of items matched by the filter (if any) in the Catalog.
	int number_matched = -1;

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
	ItemSchema(ClientContext &context, MemoryStream &buffer);

	//! Clear the definition of the Schema.
	void Clear();

	//! Returns the total number of objects matched by the filter (if any) in the Catalog.
	inline int32_t GetNumberMatched() const {
		return number_matched;
	}

	//! Parses a JSON links array to extract the schema of its STAC items recursively.
	void ParseSchemaOfLinks(std::string catalog_id, std::string collection_id, yyjson_val *links_val,
	                        const std::string &links_path, int32_t ttl_seconds);

	//! Parses a JSON object to extract the schema of its STAC items recursively.
	void ParseSchemaOfObject(std::string catalog_id, std::string collection_id, yyjson_val *json_val,
	                         const std::string &json_path, int32_t ttl_seconds);

	//! Parses a JSON object to extract the schema of its STAC items recursively.
	void ParseSchemaOfObject(std::string catalog_id, std::string collection_id, const std::string &json_str,
	                         const std::string &json_path, int32_t ttl_seconds);
};

} // namespace duckdb
