#include "search_filter.hpp"
#include "json_geometry.hpp"

#include <sstream>
#include <iomanip>

namespace duckdb {

bool SearchFilter::IsEmpty() const {
	return collections.empty() && ids.empty() && datetime.empty() && !bbox.HasXY() && intersects.IsNull();
}

std::string SearchFilter::AsQueryJson() const {
	bool first_param = true;
	std::ostringstream oss;
	oss << std::fixed << std::setprecision(10);
	oss << "{";

	// Add the collections parameter if it is not empty.
	if (!collections.empty()) {
		oss << "\"collections\": [";
		for (size_t i = 0; i < collections.size(); ++i) {
			oss << "\"" << collections[i] << "\"";
			if (i < collections.size() - 1) {
				oss << ", ";
			}
		}
		oss << "]";
		first_param = false;
	}

	// Add the ids parameter if it is not empty.
	if (!ids.empty()) {
		if (!first_param) {
			oss << ", ";
		}
		oss << "\"ids\": [";
		for (size_t i = 0; i < ids.size(); ++i) {
			oss << "\"" << ids[i] << "\"";
			if (i < ids.size() - 1) {
				oss << ", ";
			}
		}
		oss << "]";
		first_param = false;
	}

	// Add the datetime parameter if it is not empty.
	if (!datetime.empty()) {
		if (!first_param) {
			oss << ", ";
		}
		oss << "\"datetime\": \"" << datetime << "\"";
		first_param = false;
	}

	// Add the bbox parameter if it is not empty.
	if (bbox.HasXY()) {
		if (!first_param) {
			oss << ", ";
		}
		oss << "\"bbox\": [" << bbox.x_min << ", " << bbox.y_min << ", " << bbox.x_max << ", " << bbox.y_max << "]";
		first_param = false;
	}

	// Add the intersects parameter if it is not empty.
	if (!intersects.IsNull()) {
		if (!first_param) {
			oss << ", ";
		}
		oss << "\"intersects\": " << JsonGeometry::ParseGeometryAsGeoJson(intersects);
		first_param = false;
	}

	// Add the limit parameter if it is greater than 0.
	if (limit > 0) {
		if (!first_param) {
			oss << ", ";
		}
		oss << "\"limit\": " << limit;
	}

	oss << "}";
	return oss.str();
}

} // namespace duckdb
