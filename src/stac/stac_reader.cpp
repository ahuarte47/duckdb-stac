#include "stac_reader.hpp"
#include "stac_types.hpp"
#include "json_object.hpp"

// DuckDB
#include "duckdb/common/path.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

namespace duckdb {

//======================================================================================================================
// Utility types and functions
//======================================================================================================================

//! Executes an HTTP request and returns the response body as a string.
static std::string ExecuteHttpRequest(ClientContext &context, const std::string &url, const std::string &method,
                                      const HttpHeaders &headers, const std::string &body,
                                      const std::string &content_type, int32_t ttl_seconds) {
	HttpSettings settings;
	settings = HttpRequest::ExtractHttpSettings(context, url);
	settings.timeout = 30;

	HttpResponseData response =
	    HttpRequest::ExecuteHttpRequest(settings, url, method, headers, body, content_type, ttl_seconds);

	// Handle the HTTP response and check for errors.
	if (response.status_code != 200 && response.content_type == "application/json") {
		std::string error_msg = response.body;

		yyjson_doc *json_data = yyjson_read(error_msg.c_str(), error_msg.size(), YYJSON_READ_NOFLAG);
		if (json_data) {
			yyjson_val *error_val = yyjson_doc_get_root(json_data);

			if (yyjson_is_obj(error_val)) {
				yyjson_val *detail_val = yyjson_obj_get(error_val, "detail");

				if (yyjson_is_obj(detail_val)) {
					yyjson_val *message_val = yyjson_obj_get(detail_val, "message");

					if (yyjson_is_str(message_val)) {
						error_msg = yyjson_get_str(message_val);
					}
				}
			}
			yyjson_doc_free(json_data);
		}

		throw IOException("Failed to fetch the Catalog '%s': (%d) %s", url.c_str(), response.status_code,
		                  error_msg.c_str());
	}
	if (response.status_code != 200) {
		throw IOException("Failed to fetch the Catalog '%s': (%d) %s", url.c_str(), response.status_code,
		                  response.error.c_str());
	}
	if (!response.error.empty()) {
		throw IOException(response.error);
	}
	return response.body;
}

//! Determines whether the given catalog path is a static STAC Catalog (JSON file) or a dynamic STAC Catalog (URL).
static bool IsStaticCatalog(const std::string &catalog_path) {
	std::string l_path = StringUtil::Lower(catalog_path);
	return StringUtil::EndsWith(l_path, ".json") || StringUtil::EndsWith(l_path, ".geojson");
}

//! Reads the content of a JSON file and returns it as a string.
static std::string ReadContentOfJsonFile(ClientContext &context, MemoryStream &buffer, const std::string &file_path) {
	OpenFileInfo file(file_path);

	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(file, FileFlags::FILE_FLAGS_READ);
	if (!handle) {
		throw IOException("Failed to open the file '%s'.", file_path.c_str());
	}

	uint64_t file_size = handle->GetFileSize();

	if (file_size == 0) {
		buffer.SetPosition(0);
		buffer.GrowCapacity(2048);

		const char *buffer_ptr = reinterpret_cast<const char *>(buffer.GetData());
		std::ostringstream oss;

		int64_t bytes_read = 0;
		while ((bytes_read = handle->Read(QueryContext(), buffer.GetData(), 2048)) > 0) {
			oss.write(buffer_ptr, bytes_read);
		}

		std::string json_str = oss.str();
		handle.reset();
		return json_str;
	} else {
		buffer.SetPosition(0);
		buffer.GrowCapacity(file_size);

		const char *buffer_ptr = reinterpret_cast<const char *>(buffer.GetData());
		int64_t bytes_read = handle->Read(QueryContext(), buffer.GetData(), file_size);

		std::string json_str = std::string(buffer_ptr, bytes_read);
		handle.reset();
		return json_str;
	}
}

//! Reads the content of a STAC catalog and returns it as a string.
std::string ReadContentOfCatalog(ClientContext &context, MemoryStream &buffer, const std::string &catalog_path,
                                 const SearchFilter &filter, int32_t ttl_seconds) {
	if (IsStaticCatalog(catalog_path)) {
		return ReadContentOfJsonFile(context, buffer, catalog_path);
	} else if (filter.IsEmpty()) {
		return ExecuteHttpRequest(context, catalog_path, "GET", HttpHeaders(), "", "application/json", ttl_seconds);
	} else {
		std::string q = filter.AsQueryJson();
		return ExecuteHttpRequest(context, catalog_path, "POST", HttpHeaders(), q, "application/json", ttl_seconds);
	}
}

//======================================================================================================================
// STAC Abstract Reader
//======================================================================================================================

STACReader::STACReader(ClientContext &context, MemoryStream &buffer, idx_t row_offset, std::size_t row_limit)
    : context(context), buffer(buffer), row_offset(row_offset), row_limit(row_limit) {
}

void STACReader::ReadContentOfLinks(yyjson_val *links_val, const std::string &links_path, int32_t ttl_seconds) {
	yyjson_val *temp_val = nullptr;
	std::size_t links_size = yyjson_arr_size(links_val);
	yyjson_val *link_val = nullptr;

	for (std::size_t i = 0; i < links_size; i++) {
		if (yyjson_is_obj(link_val = yyjson_arr_get(links_val, i))) {
			const char *href_val = nullptr;
			const char *rel_type = nullptr;

			// Stop processing links if the limit is reached.

			if (row_limit > 0 && row_count >= row_limit) {
				next_href.clear();
				return;
			}

			// Check required fields in the link object.

			if (yyjson_is_str(temp_val = yyjson_obj_get(link_val, "href"))) {
				href_val = yyjson_get_str(temp_val);
			}
			if (!href_val || strlen(href_val) == 0) {
				continue; // Skip links without a "href" field.
			}
			if (yyjson_is_str(temp_val = yyjson_obj_get(link_val, "rel"))) {
				rel_type = yyjson_get_str(temp_val);
			}
			if (!rel_type || strlen(rel_type) == 0) {
				continue; // Skip links without a "rel" field.
			}

			// If the link is a "next" link, store its href for pagination.

			if (strcmp(rel_type, "next") == 0) {
				next_href = std::string(href_val);

				if (yyjson_is_str(temp_val = yyjson_obj_get(link_val, "method"))) {
					next_method = yyjson_get_str(temp_val);
				} else {
					next_method = "GET";
				}
				if (yyjson_is_bool(temp_val = yyjson_obj_get(link_val, "merge"))) {
					next_merge = yyjson_get_bool(temp_val);
				} else {
					next_merge = false;
				}
				if (!next_merge) {
					next_headers.clear();
					next_body.clear();
				}
				if (yyjson_is_obj(temp_val = yyjson_obj_get(link_val, "headers"))) {
					yyjson_obj_iter iter;
					yyjson_obj_iter_init(temp_val, &iter);
					yyjson_val *key, *val;

					while ((key = yyjson_obj_iter_next(&iter))) {
						if (yyjson_is_str(key) && (val = yyjson_obj_iter_get_val(key)) && yyjson_is_str(val)) {
							std::string key_str = yyjson_get_str(key);
							std::string val_str = yyjson_get_str(val);
							next_headers[key_str] = val_str;
						}
					}
				}
				if (yyjson_is_obj(temp_val = yyjson_obj_get(link_val, "body"))) {
					if (next_merge && !next_body.empty()) {
						throw NotImplementedException(
						    "The 'body' field in the 'next' link is not supported when 'merge' is true.");
					}
					char *json_str = yyjson_val_write(temp_val, YYJSON_WRITE_NOFLAG, nullptr);
					if (json_str) {
						next_body = std::string(json_str);
						free(json_str);
					} else {
						next_body.clear();
					}
				}
				continue;
			}

			// Parse the child JSON data recursively.

			if (NeedConsumeLink(rel_type)) {
				std::string href = std::string(href_val);

				// Is the href a relative path? If so, resolve it relative to the object path.
				auto href_path = Path::FromString(href);
				if (!href_path.IsAbsolute() && !href_path.HasScheme()) {
					auto parent_dir = Path::FromString(links_path).Parent();
					href = parent_dir.Join(href_path).ToString();
				}

				std::string href_str = ReadContentOfCatalog(context, buffer, href, SearchFilter(), ttl_seconds);
				ReadContentOfObject(href_str, href, ttl_seconds);
			}
		}
	}
}

void STACReader::ReadContentOfObject(const std::string &json_str, const std::string &json_path, int32_t ttl_seconds) {
	STAC_SCAN_DEBUG_LOG(1, "Reading JSON object: '%s'...", json_str.c_str());

	yyjson_doc *json_data = yyjson_read(json_str.c_str(), json_str.size(), YYJSON_READ_NOFLAG);
	if (!json_data) {
		throw IOException("Failed to parse data of the JSON object '%s'.", json_path.c_str());
	}

	try {
		yyjson_val *root_val = yyjson_doc_get_root(json_data);
		if (!root_val) {
			throw IOException("Failed to get the root value of the JSON object '%s'.", json_path.c_str());
		}

		ReadContentOfObject(root_val, json_path, ttl_seconds);

		// Make sure to free the JSON document
		yyjson_doc_free(json_data);
	} catch (...) {
		// Make sure to free the JSON document in case of an exception
		yyjson_doc_free(json_data);
		throw;
	}
}

bool STACReader::ReadNextPageOfResults(int32_t ttl_seconds) {
	if (!next_href.empty()) {
		std::string href = next_href;
		next_href.clear();

		std::string &method = next_method;
		HttpHeaders &headers = next_headers;
		std::string &body = next_body;
		std::string content_type = "application/json";

		STAC_SCAN_DEBUG_LOG(1, "Reading next page: '%s' (body: '%s')...", href.c_str(), body.c_str());

		auto json_str = ExecuteHttpRequest(context, href, method, headers, body, content_type, ttl_seconds);
		ReadContentOfObject(json_str, href, ttl_seconds);
		return true;
	}
	return false;
}

} // namespace duckdb
