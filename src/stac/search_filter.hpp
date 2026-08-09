#pragma once

#include "duckdb/common/types/geometry.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

//! Represents a filter conforming to the STAC API item-search (https://api.stacspec.org/v1.0.0/item-search/).
class SearchFilter {
public:
	std::vector<std::string> collections;
	std::vector<std::string> ids;
	std::string datetime;
	GeometryExtent bbox = GeometryExtent::Empty();
	Value intersects;
	int32_t max_items = 0;

	//! Extra STAC API - Filter Extension expression (cql2-json, cql-text).
	std::string filter;
	//! Filter language (cql2-json, cql-text), default is "cql2-json".
	std::string filter_lang = "cql2-json";
	//! Extra STAC API - Fields Extension expression (json).
	std::string fields;
	//! Extra STAC API - SortBy Extension expression (json).
	std::string sortby;

public:
	//! Checks if the filter criteria are empty.
	bool IsEmpty() const;

	//! Returns the JSON string representation of the filter suitable for a STAC API query.
	std::string AsQueryJson() const;
};

} // namespace duckdb
