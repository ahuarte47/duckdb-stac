#pragma once

#include <sstream>
#include <string>

// JSON
#include "yyjson.hpp"
using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

//! Class to represent and manage GeoJSON geometry objects
class JsonGeometry {
private:
	//! Transforms a GeoJson Coordinate pair to its string representation "(x y)"
	static void CoordinateToString(yyjson_val *coord_val, std::ostringstream &oss);
	//! Transforms a GeoJson Coordinate sequence to its string representation "(x1 y1, x2 y2, ...)"
	static void CoordinateSequenceToString(yyjson_val *sequence_val, std::ostringstream &oss);

public:
	//! Parses a GeoJSON geometry object to its WKT representation
	static std::string ParseGeometryAsWKT(yyjson_val *geometry_val);
};

} // namespace duckdb
