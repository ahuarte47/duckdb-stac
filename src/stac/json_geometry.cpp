#include "json_geometry.hpp"

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

	throw InvalidInputException("Unsupported GeoJSON geometry type: %s", geom_type);
}

} // namespace duckdb
