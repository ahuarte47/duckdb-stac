#pragma once

#include "duckdb/common/types/value.hpp"

// JSON
#include "yyjson.hpp"
using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

//! Class to represent and manage JSON objects
class JsonObject {
public:
	//! Returns the DuckDB LogicalType corresponding to a given JSON value.
	static LogicalType GetPropertyTypeOfJsonValue(yyjson_val *val);
	//! Returns the DuckDB Value corresponding to a given JSON value.
	static Value GetPropertyValueOfJsonValue(yyjson_val *val);

	//! Parses a STAC object to a generic DuckDB JSON value.
	static Value ParseGenericObject(yyjson_val *object_val);

	//! Parses a STAC bbox array to a DuckDB BoundingBox value.
	static Value ParseBoundingBoxObject(yyjson_val *bbox_val);
	//! Parses a STAC time interval to a DuckDB list of TIMESTAMP values.
	static Value ParseIntervalObject(yyjson_val *interval_val);

	//! Parses a STAC links array to a DuckDB list of LINK values.
	static Value ParseLinksObject(yyjson_val *links_val);
	//! Parses a STAC assets object to a DuckDB map of ASSET values.
	static Value ParseAssetsObject(yyjson_val *assets_val);
	//! Parses a STAC extensions array to a DuckDB list of VARCHAR values.
	static Value ParseExtensionsObject(yyjson_val *extensions_val);
	//! Parses a STAC keywords array to a DuckDB list of VARCHAR values.
	static Value ParseKeywordsObject(yyjson_val *keywords_val);
};

} // namespace duckdb
