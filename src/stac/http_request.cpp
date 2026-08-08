#include "http_request.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>

#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/main/settings.hpp"
#include "zstd.h"

namespace duckdb {

//======================================================================================================================
// Helper Functions
//======================================================================================================================

// Default max concurrent HTTP requests per scalar function call
static constexpr idx_t DEFAULT_HTTP_MAX_CONCURRENT = 32;

// Zstd magic number: 0xFD2FB528 (little-endian: 28 B5 2F FD)
static constexpr uint8_t ZSTD_MAGIC_1 = 0x28;
static constexpr uint8_t ZSTD_MAGIC_2 = 0xB5;
static constexpr uint8_t ZSTD_MAGIC_3 = 0x2F;
static constexpr uint8_t ZSTD_MAGIC_4 = 0xFD;

// Check if data is Zstd compressed by looking at magic number
static bool CheckIsZstd(const char *data, idx_t size) {
	if (size < 4) {
		return false;
	}
	return static_cast<uint8_t>(data[0]) == ZSTD_MAGIC_1 && static_cast<uint8_t>(data[1]) == ZSTD_MAGIC_2 &&
	       static_cast<uint8_t>(data[2]) == ZSTD_MAGIC_3 && static_cast<uint8_t>(data[3]) == ZSTD_MAGIC_4;
}

// Decompress Zstd compressed data
static string DecompressZstd(const string &compressed) {
	uint64_t decompressed_size = duckdb_zstd::ZSTD_getFrameContentSize(compressed.data(), compressed.size());

	if (decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
		throw IOException("Invalid zstd compressed data");
	}

	if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
		decompressed_size = compressed.size() * 10;
	}

	vector<char> buffer(decompressed_size);

	size_t actual_size =
	    duckdb_zstd::ZSTD_decompress(buffer.data(), buffer.size(), compressed.data(), compressed.size());

	if (duckdb_zstd::ZSTD_isError(actual_size)) {
		throw IOException("Zstd decompression failed: %s", duckdb_zstd::ZSTD_getErrorName(actual_size));
	}

	return string(buffer.data(), actual_size);
}

// Parse URL into host and path components
static void ParseUrl(const string &url, string &proto_host_port, string &path) {
	// Find scheme
	auto scheme_end = url.find("://");
	if (scheme_end == string::npos) {
		throw IOException("Invalid URL: missing scheme");
	}

	// Find path start (first / after scheme://)
	auto path_start = url.find('/', scheme_end + 3);
	if (path_start == string::npos) {
		proto_host_port = url;
		path = "/";
	} else {
		proto_host_port = url.substr(0, path_start);
		path = url.substr(path_start);
	}
}

// Parse a single Set-Cookie header value into a struct
static Value ParseSetCookieHeader(const string &cookie_str) {
	child_list_t<Value> cookie_values;
	string name, value, expires, path, domain, samesite;
	Value max_age = Value(LogicalType::INTEGER); // NULL by default
	bool secure = false, httponly = false;

	// Split by ';'
	vector<string> parts;
	idx_t start = 0;
	for (idx_t i = 0; i <= cookie_str.size(); i++) {
		if (i == cookie_str.size() || cookie_str[i] == ';') {
			if (i > start) {
				string part = cookie_str.substr(start, i - start);
				StringUtil::Trim(part);
				if (!part.empty()) {
					parts.push_back(part);
				}
			}
			start = i + 1;
		}
	}

	// First part is name=value
	if (!parts.empty()) {
		auto eq_pos = parts[0].find('=');
		if (eq_pos != string::npos) {
			name = parts[0].substr(0, eq_pos);
			value = parts[0].substr(eq_pos + 1);
			StringUtil::Trim(name);
			StringUtil::Trim(value);
		} else {
			name = parts[0];
		}
	}

	// Parse remaining attributes
	for (idx_t i = 1; i < parts.size(); i++) {
		string &part = parts[i];
		auto eq_pos = part.find('=');
		string attr_name, attr_value;
		if (eq_pos != string::npos) {
			attr_name = part.substr(0, eq_pos);
			attr_value = part.substr(eq_pos + 1);
			StringUtil::Trim(attr_name);
			StringUtil::Trim(attr_value);
		} else {
			attr_name = part;
			StringUtil::Trim(attr_name);
		}

		// Case-insensitive attribute matching
		if (StringUtil::CIEquals(attr_name, "expires")) {
			expires = attr_value;
		} else if (StringUtil::CIEquals(attr_name, "max-age")) {
			try {
				max_age = Value::INTEGER(std::stoi(attr_value));
			} catch (...) {
				// Invalid max-age, keep NULL
			}
		} else if (StringUtil::CIEquals(attr_name, "path")) {
			path = attr_value;
		} else if (StringUtil::CIEquals(attr_name, "domain")) {
			domain = attr_value;
		} else if (StringUtil::CIEquals(attr_name, "secure")) {
			secure = true;
		} else if (StringUtil::CIEquals(attr_name, "httponly")) {
			httponly = true;
		} else if (StringUtil::CIEquals(attr_name, "samesite")) {
			samesite = attr_value;
		}
	}

	cookie_values.push_back(make_pair("name", Value(name)));
	cookie_values.push_back(make_pair("value", Value(value)));
	cookie_values.push_back(make_pair("expires", expires.empty() ? Value(LogicalType::VARCHAR) : Value(expires)));
	cookie_values.push_back(make_pair("max_age", max_age));
	cookie_values.push_back(make_pair("path", path.empty() ? Value(LogicalType::VARCHAR) : Value(path)));
	cookie_values.push_back(make_pair("domain", domain.empty() ? Value(LogicalType::VARCHAR) : Value(domain)));
	cookie_values.push_back(make_pair("secure", Value::BOOLEAN(secure)));
	cookie_values.push_back(make_pair("httponly", Value::BOOLEAN(httponly)));
	cookie_values.push_back(make_pair("samesite", samesite.empty() ? Value(LogicalType::VARCHAR) : Value(samesite)));

	return Value::STRUCT(std::move(cookie_values));
}

// Normalize HTTP header name to Title-Case (e.g., "content-type" -> "Content-Type")
// Per HTTP convention, header names are case-insensitive but commonly written in Title-Case
static string NormalizeHeaderName(const string &name) {
	string result;
	result.reserve(name.size());
	bool capitalize_next = true;
	for (char c : name) {
		if (c == '-') {
			result += c;
			capitalize_next = true;
		} else if (capitalize_next) {
			result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			capitalize_next = false;
		} else {
			result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}
	return result;
}

//======================================================================================================================
// HttpRequest Implementation
//======================================================================================================================

// Extract HTTP settings from context (call from main thread)
HttpSettings HttpRequest::ExtractHttpSettings(ClientContext &context, const string &url) {
	HttpSettings settings;
	auto &db = DatabaseInstance::GetDatabase(context);
	auto &config = db.config;

	settings.timeout = 30;
	settings.keep_alive = true;
	settings.max_concurrency = DEFAULT_HTTP_MAX_CONCURRENT;
	settings.use_cache = true;
	settings.follow_redirects = true;

	ClientContextFileOpener opener(context);
	FileOpenerInfo info;
	info.file_path = url;

	FileOpener::TryGetCurrentSetting(&opener, "http_timeout", settings.timeout, &info);
	FileOpener::TryGetCurrentSetting(&opener, "http_keep_alive", settings.keep_alive, &info);
	FileOpener::TryGetCurrentSetting(&opener, "http_max_concurrency", settings.max_concurrency, &info);
	FileOpener::TryGetCurrentSetting(&opener, "http_request_cache", settings.use_cache, &info);
	FileOpener::TryGetCurrentSetting(&opener, "http_follow_redirects", settings.follow_redirects, &info);

	auto &http_proxy_setting = db.config.options.http_proxy;
	if (!http_proxy_setting.empty()) {
		idx_t port;
		string host;
		HTTPUtil::ParseHTTPProxyHost(http_proxy_setting, host, port);
		settings.proxy = host;
		settings.proxy_port = port;
	}
	settings.proxy_username = Settings::Get<HTTPProxyUsernameSetting>(context);
	settings.proxy_password = Settings::Get<HTTPProxyPasswordSetting>(context);

	KeyValueSecretReader secret_reader(opener, &info, "http");
	string proxy_from_secret;
	if (secret_reader.TryGetSecretKey<string>("http_proxy", proxy_from_secret) && !proxy_from_secret.empty()) {
		settings.proxy = proxy_from_secret;
	}
	string proxy_username;
	if (secret_reader.TryGetSecretKey<string>("http_proxy_username", proxy_username) && !proxy_username.empty()) {
		settings.proxy_username = proxy_username;
	}
	string proxy_password;
	if (secret_reader.TryGetSecretKey<string>("http_proxy_password", proxy_password) && !proxy_password.empty()) {
		settings.proxy_password = proxy_password;
	}

	// Check for custom user agent setting, otherwise use default
	string custom_user_agent;
	if (FileOpener::TryGetCurrentSetting(&opener, "http_user_agent", custom_user_agent, &info) &&
	    !custom_user_agent.empty()) {
		settings.user_agent = custom_user_agent;
	} else {
		settings.user_agent = StringUtil::Format("%s %s", config.UserAgent(), DuckDB::SourceID());
	}

	return settings;
}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Execute HTTP request using synchronous XHR (works in Web Worker context where duckdb-wasm always runs).
// Uses arraybuffer to safely receive binary/compressed responses, then applies C++ decompression.
HttpResponseData HttpRequest::ExecuteHttpRequest(const HttpSettings &settings, const string &url, const string &method,
                                                 const HttpHeaders &headers, const string &request_body,
                                                 const string &content_type) {
	HttpResponseData result;
	result.status_code = 0;
	result.content_length = -1;

	try {
		int32_t status_code = 0;
		int32_t body_len = 0;

		// Use arraybuffer to get raw bytes (safe for compressed/binary responses)
		char *body_ptr = (char *)EM_ASM_PTR(
		    {
			    try {
				    var url = UTF8ToString($0);
				    var method = UTF8ToString($1);
				    var xhr = new XMLHttpRequest();
				    xhr.open(method, url, false); // false = synchronous
				    xhr.responseType = 'arraybuffer';
				    xhr.send(null);
				    HEAP32[$2 >> 2] = xhr.status;
				    if (xhr.response && xhr.response.byteLength > 0) {
					    var bytes = new Uint8Array(xhr.response);
					    var len = bytes.length;
					    HEAP32[$3 >> 2] = len;
					    var ptr = _malloc(len + 1);
					    HEAPU8.set(bytes, ptr);
					    HEAPU8[ptr + len] = 0;
					    return ptr;
				    }
				    HEAP32[$3 >> 2] = 0;
				    return 0;
			    } catch (e) {
				    HEAP32[$2 >> 2] = 0;
				    HEAP32[$3 >> 2] = 0;
				    return 0;
			    }
		    },
		    url.c_str(), method.c_str(), &status_code, &body_len);

		result.status_code = status_code;
		if (result.status_code == 0) {
			result.error = "HTTP request failed (XHR error)";
		}
		if (body_ptr && body_len > 0) {
			string raw_body(body_ptr, static_cast<size_t>(body_len));
			free(body_ptr);
			// Auto-decompress (same logic as native path)
			try {
				if (GZipFileSystem::CheckIsZip(raw_body.data(), raw_body.size())) {
					result.body = GZipFileSystem::UncompressGZIPString(raw_body);
				} else if (CheckIsZstd(raw_body.data(), raw_body.size())) {
					result.body = DecompressZstd(raw_body);
				} else {
					result.body = std::move(raw_body);
				}
			} catch (...) {
				result.body = std::move(raw_body);
			}
		} else if (body_ptr) {
			free(body_ptr);
		}
	} catch (std::exception &e) {
		result.error = e.what();
	}

	return result;
}

#else

// Execute HTTP request with given settings
HttpResponseData HttpRequest::ExecuteHttpRequest(const HttpSettings &settings, const string &url, const string &method,
                                                 const HttpHeaders &headers, const string &request_body,
                                                 const string &content_type) {
	HttpResponseData result;
	result.status_code = 0;
	result.content_length = -1;

	try {
		string proto_host_port, path;
		ParseUrl(url, proto_host_port, path);

		duckdb_httplib_openssl::Client client(proto_host_port);
		client.set_follow_location(settings.follow_redirects);
		client.set_decompress(false);
		client.enable_server_certificate_verification(false);

		auto timeout_sec = static_cast<time_t>(settings.timeout);
		client.set_read_timeout(timeout_sec, 0);
		client.set_write_timeout(timeout_sec, 0);
		client.set_connection_timeout(timeout_sec, 0);
		client.set_keep_alive(settings.keep_alive);

		if (!settings.proxy.empty()) {
			string proxy_host;
			idx_t proxy_port = settings.proxy_port > 0 ? settings.proxy_port : 80;
			string proxy_copy = settings.proxy;
			HTTPUtil::ParseHTTPProxyHost(proxy_copy, proxy_host, proxy_port);
			client.set_proxy(proxy_host, static_cast<int>(proxy_port));
			if (!settings.proxy_username.empty()) {
				client.set_proxy_basic_auth(settings.proxy_username, settings.proxy_password);
			}
		}

		duckdb_httplib_openssl::Headers req_headers;
		for (auto &h : headers) {
			req_headers.insert({h.first, h.second});
		}
		if (req_headers.find("User-Agent") == req_headers.end()) {
			req_headers.insert({"User-Agent", settings.user_agent});
		}

		duckdb_httplib_openssl::Result res(nullptr, duckdb_httplib_openssl::Error::Unknown);

		if (StringUtil::CIEquals(method, "HEAD")) {
			res = client.Head(path, req_headers);
		} else if (StringUtil::CIEquals(method, "DELETE")) {
			res = client.Delete(path, req_headers);
		} else if (StringUtil::CIEquals(method, "POST")) {
			string ct = content_type.empty() ? "application/octet-stream" : content_type;
			res = client.Post(path, req_headers, request_body, ct);
		} else if (StringUtil::CIEquals(method, "PUT")) {
			string ct = content_type.empty() ? "application/octet-stream" : content_type;
			res = client.Put(path, req_headers, request_body, ct);
		} else if (StringUtil::CIEquals(method, "PATCH")) {
			string ct = content_type.empty() ? "application/octet-stream" : content_type;
			res = client.Patch(path, req_headers, request_body, ct);
		} else {
			res = client.Get(path, req_headers);
		}

		if (res.error() != duckdb_httplib_openssl::Error::Success) {
			result.error = "HTTP request failed: " + to_string(res.error());
			return result;
		}

		result.status_code = res->status;
		string response_body = res->body;

		for (auto &header : res->headers) {
			if (StringUtil::CIEquals(header.first, "Set-Cookie")) {
				result.cookies.push_back(ParseSetCookieHeader(header.second));
			} else {
				string normalized_key = NormalizeHeaderName(header.first);
				if (StringUtil::CIEquals(header.first, "Content-Type")) {
					result.content_type = header.second;
				} else if (StringUtil::CIEquals(header.first, "Content-Length")) {
					try {
						result.content_length = std::stoll(header.second);
					} catch (...) {
					}
				}
				bool found = false;
				for (idx_t i = 0; i < result.header_keys.size(); i++) {
					if (StringUtil::CIEquals(result.header_keys[i].GetValue<string>(), normalized_key)) {
						result.header_values[i] = Value(header.second);
						found = true;
						break;
					}
				}
				if (!found) {
					result.header_keys.push_back(Value(normalized_key));
					result.header_values.push_back(Value(header.second));
				}
			}
		}

		// Auto-decompress
		result.body = response_body;
		try {
			if (GZipFileSystem::CheckIsZip(response_body.data(), response_body.size())) {
				result.body = GZipFileSystem::UncompressGZIPString(response_body);
			} else if (CheckIsZstd(response_body.data(), response_body.size())) {
				result.body = DecompressZstd(response_body);
			}
		} catch (...) {
		}

	} catch (std::exception &e) {
		result.error = e.what();
	}

	return result;
}

#endif // __EMSCRIPTEN__

//======================================================================================================================
// HttpRequest with cache Implementation
//======================================================================================================================

//! Cache entry for HTTP responses, used to store responses in memory for a limited time
struct HttpResponseCacheEntry {
	HttpResponseData response;
	std::chrono::steady_clock::time_point created_at;
};

//! Mutex to protect access to the HTTP request cache
static std::mutex HTTP_REQUEST_CACHE_LOCK;
//! In-memory cache for HTTP responses, keyed by a unique cache key derived from the request
static std::unordered_map<std::string, HttpResponseCacheEntry> HTTP_REQUEST_CACHE;
//! Maximum number of entries to keep in the HTTP request cache (to prevent unbounded memory growth)
static constexpr idx_t HTTP_REQUEST_CACHE_MAX_ENTRIES = 64;

//! Returns true if the HTTP method is cacheable (GET or POST)
static bool IsCacheableHttpMethod(const std::string &method) {
	return StringUtil::CIEquals(method, "GET") || StringUtil::CIEquals(method, "POST");
}

//! Build a unique cache key for an HTTP request
static std::string BuildHttpCacheKey(const std::string &url, const std::string &method, const HttpHeaders &headers,
                                     const std::string &body, const std::string &content_type) {
	std::ostringstream key;
	key << url << '\n' << StringUtil::Upper(method) << '\n' << content_type << '\n' << body << '\n';

	// Sort headers by name to ensure consistent cache key regardless of header order

	if (headers.size() > 0) {
		std::vector<std::pair<std::string, std::string>> sorted_headers;

		sorted_headers.reserve(headers.size());
		for (const auto &entry : headers) {
			sorted_headers.emplace_back(entry.first, entry.second);
		}
		std::sort(sorted_headers.begin(), sorted_headers.end(),
		          [](const std::pair<std::string, std::string> &a, const std::pair<std::string, std::string> &b) {
			          return a.first < b.first || (a.first == b.first && a.second < b.second);
		          });

		for (const auto &entry : sorted_headers) {
			key << entry.first << ':' << entry.second << '\n';
		}
	}

	return key.str();
}

//! Prune expired entries from the HTTP request cache based on the given TTL (time-to-live)
static void PruneHttpResponseCache(std::chrono::seconds ttl_seconds) {
	const auto now = std::chrono::steady_clock::now();

	for (auto it = HTTP_REQUEST_CACHE.begin(); it != HTTP_REQUEST_CACHE.end();) {
		if (now - it->second.created_at > ttl_seconds) {
			it = HTTP_REQUEST_CACHE.erase(it);
		} else {
			++it;
		}
	}
	if (HTTP_REQUEST_CACHE.size() >= HTTP_REQUEST_CACHE_MAX_ENTRIES) {
		HTTP_REQUEST_CACHE.erase(HTTP_REQUEST_CACHE.begin());
	}
}

HttpResponseData HttpRequest::ExecuteHttpRequest(const HttpSettings &settings, const string &url, const string &method,
                                                 const HttpHeaders &headers, const string &request_body,
                                                 const string &content_type, int32_t ttl_seconds) {
	std::string cache_key;

	// Determine if caching should be used
	const bool use_cache = ttl_seconds > 0 && IsCacheableHttpMethod(method);
	if (use_cache) {
		cache_key = BuildHttpCacheKey(url, method, headers, request_body, content_type);

		std::lock_guard<std::mutex> lock(HTTP_REQUEST_CACHE_LOCK);

		auto cached_it = HTTP_REQUEST_CACHE.find(cache_key);
		if (cached_it != HTTP_REQUEST_CACHE.end()) {
			const auto now = std::chrono::steady_clock::now();

			if (now - cached_it->second.created_at <= std::chrono::seconds(ttl_seconds)) {
				return cached_it->second.response;
			}
			HTTP_REQUEST_CACHE.erase(cached_it);
		}
	}

	HttpResponseData result =
	    HttpRequest::ExecuteHttpRequest(settings, url, method, headers, request_body, content_type);

	// Store the response in the cache if caching is enabled
	if (use_cache) {
		std::lock_guard<std::mutex> lock(HTTP_REQUEST_CACHE_LOCK);

		if (HTTP_REQUEST_CACHE.size() >= HTTP_REQUEST_CACHE_MAX_ENTRIES) {
			PruneHttpResponseCache(std::chrono::seconds(ttl_seconds));
		}
		HTTP_REQUEST_CACHE[cache_key] = {result, std::chrono::steady_clock::now()};
	}

	return result;
}

} // namespace duckdb
