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
private:
	//! A constant NULL value to return for invalid column indices.
	static const Value NULL_VALUE;

public:
	duckdb::Value catalog;
	duckdb::Value collection;
	duckdb::Value id;
	duckdb::Value geometry;
	duckdb::Value bbox;
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
