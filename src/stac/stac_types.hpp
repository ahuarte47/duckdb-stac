#pragma once

// DuckDB
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

//! Column indices for the implicit columns of the STAC data table function.
#define STAC_CATALOG_COLUMN_INDEX        0
#define STAC_COLLECTION_COLUMN_INDEX     1
#define STAC_ID_COLUMN_INDEX             2
#define STAC_GEOMETRY_COLUMN_INDEX       3
#define STAC_BBOX_COLUMN_INDEX           4
#define STAC_EXTENSIONS_COLUMN_INDEX     5
#define STAC_LINKS_COLUMN_INDEX          6
#define STAC_ASSETS_COLUMN_INDEX         7
#define STAC_FIRST_PROPERTY_COLUMN_INDEX 8

//! Represents a single STAC item row in the result table.
struct ItemRow {
	std::string catalog;
	std::string collection;
	std::string id;
	std::string geometry;
	duckdb::Value bbox;
	duckdb::Value extensions;
	duckdb::Value links;
	duckdb::Value assets;
	// Dynamic properties stored as a map of column index to Value.
	std::map<idx_t, duckdb::Value> properties;
};

class ExtensionLoader;

//! Define new types to register into DuckDB.
struct STACTypes {
	static LogicalType BBOX();
	static LogicalType LINK();
	static LogicalType ASSET();

	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
