#pragma once

// DuckDB
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"

// Debug logging controlled by STAC_DEBUG environment variable
#if defined(__has_cpp_attribute) && __has_cpp_attribute(maybe_unused)
[[maybe_unused]]
#endif
static int
GetDebugLevel() {
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

//! Column indices for the implicit columns of the STAC collection table function.
#define STAC_COLLECTION_CATALOG_COLUMN_INDEX     0
#define STAC_COLLECTION_ID_COLUMN_INDEX          1
#define STAC_COLLECTION_TITLE_COLUMN_INDEX       2
#define STAC_COLLECTION_DESCRIPTION_COLUMN_INDEX 3
#define STAC_COLLECTION_KEYWORDS_COLUMN_INDEX    4
#define STAC_COLLECTION_LICENSE_COLUMN_INDEX     5
#define STAC_COLLECTION_PROVIDERS_COLUMN_INDEX   6
#define STAC_COLLECTION_BBOX_COLUMN_INDEX        7
#define STAC_COLLECTION_INTERVAL_COLUMN_INDEX    8
#define STAC_COLLECTION_SUMMARIES_COLUMN_INDEX   9
#define STAC_COLLECTION_VERSION_COLUMN_INDEX     10
#define STAC_COLLECTION_EXTENSIONS_COLUMN_INDEX  11
#define STAC_COLLECTION_LINKS_COLUMN_INDEX       12
#define STAC_COLLECTION_ASSETS_COLUMN_INDEX      13
#define STAC_COLLECTION_ITEM_ASSETS_COLUMN_INDEX 14

//! Represents a single STAC item row in the result table.
struct CollectionRow {
private:
	//! A constant NULL value to return for invalid column indices.
	static const Value NULL_VALUE;

public:
	duckdb::Value catalog;
	duckdb::Value id;
	duckdb::Value title;
	duckdb::Value description;
	duckdb::Value keywords;
	duckdb::Value license;
	duckdb::Value providers;
	duckdb::Value bbox;
	duckdb::Value interval;
	duckdb::Value summaries;
	duckdb::Value version;
	duckdb::Value extensions;
	duckdb::Value links;
	duckdb::Value assets;
	duckdb::Value item_assets;

	//! Get the value of a column by index.
	const duckdb::Value &ValueOf(const idx_t &dim_index) const;
};

//! Column indices for the implicit columns of the STAC item table function.
#define STAC_ITEM_CATALOG_COLUMN_INDEX        0
#define STAC_ITEM_COLLECTION_COLUMN_INDEX     1
#define STAC_ITEM_ID_COLUMN_INDEX             2
#define STAC_ITEM_GEOMETRY_COLUMN_INDEX       3
#define STAC_ITEM_BBOX_COLUMN_INDEX           4
#define STAC_ITEM_VERSION_COLUMN_INDEX        5
#define STAC_ITEM_EXTENSIONS_COLUMN_INDEX     6
#define STAC_ITEM_LINKS_COLUMN_INDEX          7
#define STAC_ITEM_ASSETS_COLUMN_INDEX         8
#define STAC_ITEM_FIRST_PROPERTY_COLUMN_INDEX 9

//! Represents a single STAC item row in the result table.
struct ItemRow {
private:
	//! A constant NULL value to return for invalid column indices.
	static const Value NULL_VALUE;

public:
	duckdb::Value catalog;
	duckdb::Value collection;
	duckdb::Value id;
	duckdb::Value geometry;
	duckdb::Value bbox;
	duckdb::Value version;
	duckdb::Value extensions;
	duckdb::Value links;
	duckdb::Value assets;
	// Dynamic properties stored as a map of column index to Value.
	std::map<idx_t, duckdb::Value> properties;

	//! Get the value of a column by index.
	const duckdb::Value &ValueOf(const idx_t &dim_index) const;
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
