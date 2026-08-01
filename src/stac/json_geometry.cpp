#include "json_geometry.hpp"

// DuckDB
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/value.hpp"
#include "fast_float/fast_float.h"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <iomanip>

//======================================================================================================================
// Utilities to parse WKT geometries into GeoJSON
//======================================================================================================================

namespace {

using namespace duckdb;

//! Reader for WKT text (Copy of geometry.cpp's TextReader).
class WktReader {
public:
	WktReader(const char *text, const uint32_t size) : beg(text), pos(text), end(text + size) {
	}

	bool TryMatch(const char *str) {
		auto ptr = pos;
		while (*str && pos < end && tolower(*pos) == tolower(*str)) {
			pos++;
			str++;
		}
		if (*str == '\0') {
			SkipWhitespace(); // remove trailing whitespace
			return true;      // matched
		}
		pos = ptr;    // reset position
		return false; // not matched
	}

	bool TryMatch(char c) {
		if (pos < end && tolower(*pos) == tolower(c)) {
			pos++;
			SkipWhitespace(); // remove trailing whitespace
			return true;      // matched
		}
		return false; // not matched
	}

	void Match(const char *str) {
		if (!TryMatch(str)) {
			// Check if this would go EOF
			if (pos + strlen(str) >= end) {
				throw MakeError("Expected '%s' but got end of input", str);
			}
			throw MakeError("Expected '%s' but got '%c'", str, *pos);
		}
	}

	void Match(char c) {
		if (!TryMatch(c)) {
			if (pos >= end) {
				throw MakeError("Expected '%c' but got end of input", c);
			}
			throw MakeError("Expected '%c' but got '%c'", c, *pos);
		}
	}

	double MatchNumber() {
		// Now use fast_float to parse the number
		double num;
		const auto res = duckdb_fast_float::from_chars(pos, end, num);
		if (res.ec != std::errc()) {
			throw MakeError("Expected number");
		}

		pos = res.ptr; // update position to the end of the parsed number

		SkipWhitespace(); // remove trailing whitespace
		return num;       // return the parsed number
	}

	idx_t GetPosition() const {
		return static_cast<idx_t>(pos - beg);
	}

	void Reset() {
		pos = beg;
	}

	template <class... ARGS>
	InvalidInputException MakeError(const char *raw_msg, ARGS... args) const {
		const auto byte_offset = UnsafeNumericCast<idx_t>(pos - beg);
		auto msg = StringUtil::Format("Failed to parse geometry: %s at offset %lu",
		                              StringUtil::Format(raw_msg, args...), byte_offset);
		if (query_location.IsValid()) {
			const auto expr_offset = optional_idx(query_location.GetIndex() + byte_offset);
			return InvalidInputException(Exception::InitializeExtraInfo(expr_offset), msg);
		} else {
			return InvalidInputException(msg);
		}
	}

	void SetQueryLocation(optional_idx location) {
		query_location = location;
	}

	void SkipWhitespace() {
		while (pos < end && isspace(*pos)) {
			pos++;
		}
	}

private:
	const char *beg;
	const char *pos;
	const char *end;
	optional_idx query_location;
};

//! Writer for GeoJSON text from WKT text.
class GeoJsonWriter {
public:
	//! Writes a single vertex ("x y[ z][ m]") as a JSON array.
	static void WriteVertex(WktReader &reader, std::ostringstream &oss, uint32_t dims) {
		oss << "[";
		for (uint32_t d_idx = 0; d_idx < dims; d_idx++) {
			if (d_idx > 0) {
				oss << ", ";
			}
			oss << reader.MatchNumber();
		}
		oss << "]";
	}

	//! Writes a sequence of vertices "(v, v, ...)" as a JSON array of vertices.
	static void WriteVertexSequence(WktReader &reader, std::ostringstream &oss, uint32_t dims) {
		oss << "[";
		reader.Match('(');
		bool first = true;
		do {
			if (!first) {
				oss << ", ";
			}
			first = false;
			WriteVertex(reader, oss, dims);
		} while (reader.TryMatch(','));
		reader.Match(')');
		oss << "]";
	}

	//! Writes a sequence of rings "(ring, ring, ...)" (a polygon's rings) as a JSON array of vertex arrays.
	static void WriteRingSequence(WktReader &reader, std::ostringstream &oss, uint32_t dims) {
		oss << "[";
		reader.Match('(');
		bool first = true;
		do {
			if (!first) {
				oss << ", ";
			}
			first = false;
			WriteVertexSequence(reader, oss, dims);
		} while (reader.TryMatch(','));
		reader.Match(')');
		oss << "]";
	}
};

//! Writes a WKT geometry as GeoJSON.
void SerializeGeoJson(WktReader &reader, std::ostringstream &oss, uint32_t depth, bool parent_has_z,
                      bool parent_has_m) {
	if (depth == Geometry::MAX_RECURSION_DEPTH) {
		throw reader.MakeError("Geometry string exceeds maximum recursion depth of %d", Geometry::MAX_RECURSION_DEPTH);
	}

	// Skip leading whitespace
	reader.SkipWhitespace();

	// EWKT dialect (ignore SRID if present)
	if (reader.TryMatch("SRID")) {
		reader.Match('=');
		reader.MatchNumber();
		reader.Match(';');
	}

	GeometryType type = GeometryType::INVALID;

	if (reader.TryMatch("point")) {
		type = GeometryType::POINT;
	} else if (reader.TryMatch("linestring")) {
		type = GeometryType::LINESTRING;
	} else if (reader.TryMatch("polygon")) {
		type = GeometryType::POLYGON;
	} else if (reader.TryMatch("multipoint")) {
		type = GeometryType::MULTIPOINT;
	} else if (reader.TryMatch("multilinestring")) {
		type = GeometryType::MULTILINESTRING;
	} else if (reader.TryMatch("multipolygon")) {
		type = GeometryType::MULTIPOLYGON;
	} else if (reader.TryMatch("geometrycollection")) {
		type = GeometryType::GEOMETRYCOLLECTION;
	} else {
		throw reader.MakeError("unknown geometry type");
	}

	const auto has_z = reader.TryMatch("z");
	const auto has_m = reader.TryMatch("m");
	const auto is_empty = reader.TryMatch("empty");

	if ((depth != 0) && ((parent_has_z != has_z) || (parent_has_m != has_m))) {
		throw reader.MakeError("Geometry has inconsistent Z/M dimensions");
	}

	// How many dimensions does this geometry have?
	const uint32_t dims = 2 + (has_z ? 1 : 0) + (has_m ? 1 : 0);

	// Write the GeoJSON representation
	switch (type) {
	case GeometryType::POINT: {
		oss << "{\"type\": \"Point\", \"coordinates\": ";
		if (is_empty) {
			oss << "[]";
		} else {
			reader.Match('(');
			GeoJsonWriter::WriteVertex(reader, oss, dims);
			reader.Match(')');
		}
		break;
	}
	case GeometryType::LINESTRING: {
		oss << "{\"type\": \"LineString\", \"coordinates\": ";
		if (is_empty) {
			oss << "[]";
		} else {
			GeoJsonWriter::WriteVertexSequence(reader, oss, dims);
		}
		break;
	}
	case GeometryType::POLYGON: {
		oss << "{\"type\": \"Polygon\", \"coordinates\": ";
		if (is_empty) {
			oss << "[]";
		} else {
			GeoJsonWriter::WriteRingSequence(reader, oss, dims);
		}
		break;
	}
	case GeometryType::MULTIPOINT: {
		oss << "{\"type\": \"MultiPoint\", \"coordinates\": ";
		if (is_empty) {
			oss << "[]";
		} else {
			oss << "[";
			reader.Match('(');
			bool first = true;
			do {
				if (!first) {
					oss << ", ";
				}
				first = false;

				// Points inside a MULTIPOINT may optionally be individually parenthesized
				const auto has_paren = reader.TryMatch('(');
				if (reader.TryMatch("empty")) {
					oss << "null";
				} else {
					GeoJsonWriter::WriteVertex(reader, oss, dims);
				}
				if (has_paren) {
					reader.Match(')');
				}
			} while (reader.TryMatch(','));
			reader.Match(')');
			oss << "]";
		}
		break;
	}
	case GeometryType::MULTILINESTRING: {
		oss << "{\"type\": \"MultiLineString\", \"coordinates\": ";
		if (is_empty) {
			oss << "[]";
		} else {
			oss << "[";
			reader.Match('(');
			bool first = true;
			do {
				if (!first) {
					oss << ", ";
				}
				first = false;
				if (reader.TryMatch("empty")) {
					oss << "[]";
				} else {
					GeoJsonWriter::WriteVertexSequence(reader, oss, dims);
				}
			} while (reader.TryMatch(','));
			reader.Match(')');
			oss << "]";
		}
		break;
	}
	case GeometryType::MULTIPOLYGON: {
		oss << "{\"type\": \"MultiPolygon\", \"coordinates\": ";
		if (is_empty) {
			oss << "[]";
		} else {
			oss << "[";
			reader.Match('(');
			bool first = true;
			do {
				if (!first) {
					oss << ", ";
				}
				first = false;
				if (reader.TryMatch("empty")) {
					oss << "[]";
				} else {
					GeoJsonWriter::WriteRingSequence(reader, oss, dims);
				}
			} while (reader.TryMatch(','));
			reader.Match(')');
			oss << "]";
		}
		break;
	}
	case GeometryType::GEOMETRYCOLLECTION: {
		oss << "{\"type\": \"GeometryCollection\", \"geometries\": [";
		if (!is_empty) {
			reader.Match('(');
			bool first = true;
			do {
				if (!first) {
					oss << ", ";
				}
				first = false;
				SerializeGeoJson(reader, oss, depth + 1, has_z, has_m);
			} while (reader.TryMatch(','));
			reader.Match(')');
		}
		oss << "]";
		break;
	}
	default:
		throw reader.MakeError("Unknown geometry type %d", static_cast<int>(type));
	}
	oss << "}";
}

} // namespace

//======================================================================================================================
// JsonGeometry
//======================================================================================================================

namespace duckdb {

void JsonGeometry::CoordinateToString(yyjson_val *coord_val, std::ostringstream &oss) {
	if (!yyjson_is_arr(coord_val) || yyjson_arr_size(coord_val) < 2) {
		throw InvalidInputException("Invalid coordinate array: expected an array of at least two elements.");
	}

	yyjson_val *x = yyjson_arr_get(coord_val, 0);
	yyjson_val *y = yyjson_arr_get(coord_val, 1);

	if (!x || !y || !unsafe_yyjson_is_num(x) || !unsafe_yyjson_is_num(y)) {
		throw InvalidInputException("Invalid coordinate array: expected an array of at least two real elements.");
	}
	oss << yyjson_get_real(x) << " " << yyjson_get_real(y);
}

void JsonGeometry::CoordinateSequenceToString(yyjson_val *sequence_val, std::ostringstream &oss) {
	if (!yyjson_is_arr(sequence_val)) {
		throw InvalidInputException("Invalid coordinate sequence: expected an array of coordinates.");
	}

	yyjson_arr_iter iter;
	yyjson_arr_iter_init(sequence_val, &iter);
	yyjson_val *coord_val;
	bool first = true;

	while ((coord_val = yyjson_arr_iter_next(&iter))) {
		if (!first) {
			oss << ", ";
		}
		JsonGeometry::CoordinateToString(coord_val, oss);
		first = false;
	}
	if (first) {
		throw InvalidInputException("Invalid coordinate sequence: expected at least one coordinate.");
	}
}

std::string JsonGeometry::ParseGeometryAsWKT(yyjson_val *geometry_val) {
	if (!yyjson_is_obj(geometry_val)) {
		return "";
	}

	yyjson_val *type_val = yyjson_obj_get(geometry_val, "type");
	yyjson_val *coords_val = yyjson_obj_get(geometry_val, "coordinates");

	if (!yyjson_is_str(type_val)) {
		throw InvalidInputException("Invalid GeoJSON geometry: missing 'type' field.");
	}
	if (!yyjson_is_arr(coords_val)) {
		throw InvalidInputException("Invalid GeoJSON geometry: 'coordinates' field must exist as an array.");
	}

	const char *geom_type = yyjson_get_str(type_val);
	std::ostringstream wkt;

	if (strcmp(geom_type, "Point") == 0) {
		wkt << "POINT (";
		JsonGeometry::CoordinateToString(coords_val, wkt);
		wkt << ")";
		return wkt.str();
	}
	if (strcmp(geom_type, "LineString") == 0) {
		wkt << "LINESTRING (";
		JsonGeometry::CoordinateSequenceToString(coords_val, wkt);
		wkt << ")";
		return wkt.str();
	}
	if (strcmp(geom_type, "Polygon") == 0) {
		wkt << "POLYGON (";

		yyjson_arr_iter iter;
		yyjson_arr_iter_init(coords_val, &iter);
		yyjson_val *ring;
		bool first = true;

		while ((ring = yyjson_arr_iter_next(&iter))) {
			if (!first) {
				wkt << ", ";
			}
			wkt << "(";
			JsonGeometry::CoordinateSequenceToString(ring, wkt);
			wkt << ")";
			first = false;
		}

		wkt << ")";
		return wkt.str();
	}
	if (strcmp(geom_type, "MultiPoint") == 0) {
		wkt << "MULTIPOINT (";

		yyjson_arr_iter iter;
		yyjson_arr_iter_init(coords_val, &iter);
		yyjson_val *point;
		bool first = true;

		while ((point = yyjson_arr_iter_next(&iter))) {
			if (!first) {
				wkt << ", ";
			}
			wkt << "(";
			JsonGeometry::CoordinateToString(point, wkt);
			wkt << ")";
			first = false;
		}

		wkt << ")";
		return wkt.str();
	}
	if (strcmp(geom_type, "MultiLineString") == 0) {
		wkt << "MULTILINESTRING (";

		yyjson_arr_iter iter;
		yyjson_arr_iter_init(coords_val, &iter);
		yyjson_val *line;
		bool first = true;

		while ((line = yyjson_arr_iter_next(&iter))) {
			if (!first) {
				wkt << ", ";
			}
			wkt << "(";
			JsonGeometry::CoordinateSequenceToString(line, wkt);
			wkt << ")";
			first = false;
		}

		wkt << ")";
		return wkt.str();
	}
	if (strcmp(geom_type, "MultiPolygon") == 0) {
		wkt << "MULTIPOLYGON (";

		yyjson_arr_iter iter;
		yyjson_arr_iter_init(coords_val, &iter);
		yyjson_val *polygon;
		bool first = true;

		while ((polygon = yyjson_arr_iter_next(&iter))) {
			if (!first) {
				wkt << ", ";
			}
			wkt << "(";
			yyjson_arr_iter ring_iter;
			yyjson_arr_iter_init(polygon, &ring_iter);
			yyjson_val *ring;
			bool first_ring = true;

			while ((ring = yyjson_arr_iter_next(&ring_iter))) {
				if (!first_ring) {
					wkt << ", ";
				}
				wkt << "(";
				JsonGeometry::CoordinateSequenceToString(ring, wkt);
				wkt << ")";
				first_ring = false;
			}
			wkt << ")";
			first = false;
		}

		wkt << ")";
		return wkt.str();
	}
	if (strcmp(geom_type, "GeometryCollection") == 0) {
		wkt << "GEOMETRYCOLLECTION (";

		yyjson_val *geometries_val = yyjson_obj_get(geometry_val, "geometries");
		if (!yyjson_is_arr(geometries_val)) {
			throw InvalidInputException(
			    "Invalid GeoJSON geometry: 'geometries' field must exist as an array for GeometryCollection.");
		}

		yyjson_arr_iter iter;
		yyjson_arr_iter_init(geometries_val, &iter);
		yyjson_val *geom;
		bool first = true;

		while ((geom = yyjson_arr_iter_next(&iter))) {
			if (!first) {
				wkt << ", ";
			}
			wkt << JsonGeometry::ParseGeometryAsWKT(geom);
			first = false;
		}

		wkt << ")";
		return wkt.str();
	}
	throw InvalidInputException("Unsupported GeoJSON geometry type: %s", geom_type);
}

std::string JsonGeometry::ParseGeometryAsGeoJson(const Value &geometry) {
	if (geometry.IsNull()) {
		return "null";
	}

	// Convert the geometry to WKT, then read the WKT tokens and write the GeoJSON representation.
	if (geometry.type().id() == LogicalTypeId::GEOMETRY || geometry.type().id() == LogicalTypeId::VARCHAR) {
		std::ostringstream geojson;
		geojson << std::fixed << std::setprecision(10);

		const std::string wkt = geometry.ToString();
		WktReader wkt_reader(wkt.c_str(), static_cast<uint32_t>(wkt.size()));
		SerializeGeoJson(wkt_reader, geojson, 0, false, false);

		return geojson.str();
	}
	throw InvalidInputException("Invalid geometry type: expected GEOMETRY.");
}

} // namespace duckdb
