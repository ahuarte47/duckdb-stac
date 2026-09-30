#pragma once

// DuckDB
#include "duckdb.hpp"

#include "http_request.hpp"
#include "search_filter.hpp"
#include "yyjson.hpp"
using namespace duckdb_yyjson; // NOLINT

namespace duckdb {

//! Reads the content of a STAC catalog and returns it as a string.
std::string ReadContentOfCatalog(ClientContext &context, MemoryStream &buffer, const std::string &catalog_path,
                                 const SearchFilter &filter, int32_t ttl_seconds);

//! Base class for reading the content of a type of objects in a STAC Catalog.
class STACReader {
protected:
	//! The client context for the current query execution.
	ClientContext &context;
	//! The buffer used to read JSON content.
	MemoryStream &buffer;

	//! Offset to be applied for the rows when reading objects.
	idx_t row_offset = 0;
	//! Limit for the rows to be read (A value of 0 means no limit is applied).
	std::size_t row_limit = 0;
	//! Total number of rows read so far by this reader.
	std::size_t row_count = 0;
	//! Total number of objects matched by the filter (if any) in the Catalog.
	int32_t number_matched = -1;

protected:
	//! The next href for the next page of results, if any.
	std::string next_href;
	//! The next method for the next page of results, if any.
	std::string next_method = "GET";
	//! The next headers for the next page of results, if any.
	HttpHeaders next_headers;
	//! The next body for the next page of results, if any.
	std::string next_body;
	//! The headers/body in the next link must be merged into the original request
	//! and be sent combined in the next request.
	bool next_merge = false;

public:
	//! Constructor for the STACReader class.
	STACReader(ClientContext &context, MemoryStream &buffer, idx_t row_offset = 0, std::size_t row_limit = 0);

	//! Returns the total number of objects matched by the filter (if any) in the Catalog.
	inline int32_t GetNumberMatched() const {
		return number_matched;
	}

	//! Returns the total number of rows read so far by this reader.
	inline std::size_t GetRowCount() const {
		return row_count;
	}

	//! Returns true if the given STAC "rel_type" requires fetching a node link (e.g., a "child" link).
	virtual bool NeedConsumeLink(const char *rel_type) = 0;

	//! Reads the content of a JSON links array to extract the child STAC objects.
	virtual void ReadContentOfLinks(yyjson_val *links_val, const std::string &links_path, int32_t ttl_seconds);

	//! Reads the content of a JSON object to extract the child STAC objects.
	virtual void ReadContentOfObject(yyjson_val *json_val, const std::string &json_path, int32_t ttl_seconds) = 0;

	//! Reads the content of a JSON object to extract the child STAC objects.
	virtual void ReadContentOfObject(const std::string &json_str, const std::string &json_path, int32_t ttl_seconds);

	//! Reads the next page of results, true if more results are available, false otherwise.
	virtual bool ReadNextPageOfResults(int32_t ttl_seconds = 0);
};

} // namespace duckdb
