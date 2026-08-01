#pragma once

#include <sstream>
#include <string>

// JSON
#include "yyjson.hpp"
using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

class Value;

//! Class to manage GeoJSON geometry objects
class JsonGeometry {
private:
	//! Transforms a GeoJson Coordinate pair to its string representation "(x y)"
	static void CoordinateToString(yyjson_val *coord_val, std::ostringstream &oss);
	//! Transforms a GeoJson Coordinate sequence to its string representation "(x1 y1, x2 y2, ...)"
	static void CoordinateSequenceToString(yyjson_val *sequence_val, std::ostringstream &oss);

public:
	//! Parses a GeoJSON geometry object to its WKT representation
	static std::string ParseGeometryAsWKT(yyjson_val *geometry_val);
	//! Parses a DuckDB geometry value to its GeoJSON-string representation
	static std::string ParseGeometryAsGeoJson(const Value &geometry_val);
};

} // namespace duckdb
