#include "http_request.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>

#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_file_opener.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/http/http_util.hpp"
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
	FileOpenerInfo info(url);

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

	// Check for proxy settings in the secrets
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

	// Check for authentication settings in the secrets
	string basic_auth_username;
	if (secret_reader.TryGetSecretKey<string>("username", basic_auth_username) && !basic_auth_username.empty()) {
		settings.basic_auth.username = basic_auth_username;
	}
	string basic_auth_password;
	if (secret_reader.TryGetSecretKey<string>("password", basic_auth_password) && !basic_auth_password.empty()) {
		settings.basic_auth.password = basic_auth_password;
	}
	string bearer_token;
	if (secret_reader.TryGetSecretKey<string>("bearer_token", bearer_token) && !bearer_token.empty()) {
		settings.bearer_token_auth.token = bearer_token;
	}
	Value extra_headers;
	if (secret_reader.TryGetSecretKey<Value>("extra_http_headers", extra_headers) && !extra_headers.IsNull()) {
		if (extra_headers.type().id() != LogicalTypeId::MAP) {
			throw InvalidInputException("Extra HTTP headers must be a map of string to string");
		}
		for (const auto &entry : MapValue::GetChildren(extra_headers)) {
			const auto &kv_pair = StructValue::GetChildren(entry);

			if (kv_pair.size() == 2 && !kv_pair[0].IsNull() && !kv_pair[1].IsNull()) {
				auto key = kv_pair[0].ToString();

				if (settings.extra_headers.find(key) == settings.extra_headers.end()) {
					settings.extra_headers.emplace(key, kv_pair[1].ToString());
				}
			}
		}
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

static HTTPUtil &GetHTTPUtil(ClientContext &context, const string &url) {
	auto &db = DatabaseInstance::GetDatabase(context);
	auto &http_util = HTTPUtil::Get(db);

	auto scheme_end = url.find("://");
	if (scheme_end != string::npos && StringUtil::CIEquals(url.substr(0, scheme_end), "https") &&
	    http_util.GetName() == "Built-In") {
		if (!ExtensionHelper::TryAutoLoadExtension(context, "httpfs")) {
			throw MissingExtensionException("This extension needs the httpfs extension for HTTPS URLs. Run INSTALL "
			                                "httpfs; LOAD httpfs; and try again");
		}
		auto &loaded_util = HTTPUtil::Get(db);
		if (loaded_util.GetName() == "Built-In") {
			throw MissingExtensionException("This extension needs the httpfs extension for HTTPS URLs. Run INSTALL "
			                                "httpfs; LOAD httpfs; and try again");
		}
		return loaded_util;
	}
	return http_util;
}

// Execute HTTP request with DuckDB's configured HTTP transport.
HttpResponseData HttpRequest::ExecuteHttpRequest(ClientContext &context, const HttpSettings &settings,
                                                 const string &url, const string &method, const HttpHeaders &headers,
                                                 const string &request_body, const string &content_type) {
	HttpResponseData result;
	result.status_code = 0;
	result.content_length = -1;

	try {
		auto &http_util = GetHTTPUtil(context, url);
		auto params = http_util.InitializeParameters(context, url);
		params->timeout = settings.timeout;
		params->timeout_usec = 0;
		params->keep_alive = settings.keep_alive;
		params->follow_location = settings.follow_redirects;

		if (!settings.proxy.empty()) {
			string proxy_host;
			idx_t proxy_port = settings.proxy_port > 0 ? settings.proxy_port : 80;
			string proxy_copy = settings.proxy;
			HTTPUtil::ParseHTTPProxyHost(proxy_copy, proxy_host, proxy_port);
			params->http_proxy = proxy_host;
			params->http_proxy_port = proxy_port;
			params->http_proxy_username = settings.proxy_username;
			params->http_proxy_password = settings.proxy_password;
		} else {
			params->http_proxy.clear();
			params->http_proxy_port = 0;
			params->http_proxy_username.clear();
			params->http_proxy_password.clear();
		}

		HTTPHeaders req_headers;
		for (auto &h : headers) {
			req_headers.Insert(h.first, h.second);
		}
		for (auto &h : settings.extra_headers) {
			if (!req_headers.HasHeader(h.first)) {
				req_headers.Insert(h.first, h.second);
			}
		}
		if (!req_headers.HasHeader("Authorization")) {
			if (!settings.bearer_token_auth.token.empty()) {
				req_headers.Insert("Authorization", "Bearer " + settings.bearer_token_auth.token);
			} else if (!settings.basic_auth.username.empty() && !settings.basic_auth.password.empty()) {
				auto credentials = settings.basic_auth.username + ":" + settings.basic_auth.password;
				req_headers.Insert(
				    "Authorization",
				    "Basic " + Blob::ToBase64(string_t(credentials.data(), NumericCast<uint32_t>(credentials.size()))));
			}
		}
		if (!req_headers.HasHeader("User-Agent")) {
			req_headers.Insert("User-Agent", settings.user_agent);
		}

		unique_ptr<HTTPResponse> response;
		if (StringUtil::CIEquals(method, "HEAD")) {
			HeadRequestInfo request(url, req_headers, *params);
			request.try_request = true;
			response = http_util.Request(request);
		} else if (StringUtil::CIEquals(method, "DELETE")) {
			DeleteRequestInfo request(url, req_headers, *params);
			request.try_request = true;
			response = http_util.Request(request);
		} else if (StringUtil::CIEquals(method, "POST")) {
			if (!req_headers.HasHeader("Content-Type")) {
				req_headers.Insert("Content-Type", content_type.empty() ? "application/octet-stream" : content_type);
			}
			PostRequestInfo request(url, req_headers, *params, const_data_ptr_cast(request_body.data()),
			                        request_body.size());
			request.try_request = true;
			response = http_util.Request(request);
		} else if (StringUtil::CIEquals(method, "PUT")) {
			PutRequestInfo request(url, req_headers, *params, const_data_ptr_cast(request_body.data()),
			                       request_body.size(),
			                       content_type.empty() ? "application/octet-stream" : content_type);
			request.try_request = true;
			response = http_util.Request(request);
		} else if (StringUtil::CIEquals(method, "PATCH")) {
			throw NotImplementedException("HTTP PATCH requests are not supported by DuckDB's HTTPUtil");
		} else {
			GetRequestInfo request(url, req_headers, *params, {}, {});
			request.try_request = true;
			response = http_util.Request(request);
		}

		if (!response || response->HasRequestError()) {
			result.error = "HTTP request failed: " +
			               (response ? response->GetRequestError() : string("no response from HTTP provider"));
			return result;
		}

		result.status_code = static_cast<int32_t>(response->status);

		for (const auto &header : response->headers) {
			auto values = response->headers.GetHeaderValues(header.first);

			for (const auto &value : values) {
				if (StringUtil::CIEquals(header.first, "Set-Cookie")) {
					result.cookies.push_back(ParseSetCookieHeader(value));
				} else {
					string normalized_key = NormalizeHeaderName(header.first);
					if (StringUtil::CIEquals(header.first, "Content-Type")) {
						result.content_type = value;
					} else if (StringUtil::CIEquals(header.first, "Content-Length")) {
						try {
							result.content_length = std::stoll(value);
						} catch (...) {
						}
					}
					bool found = false;
					for (idx_t i = 0; i < result.header_keys.size(); i++) {
						if (StringUtil::CIEquals(result.header_keys[i].GetValue<string>(), normalized_key)) {
							result.header_values[i] = Value(value);
							found = true;
							break;
						}
					}
					if (!found) {
						result.header_keys.push_back(Value(normalized_key));
						result.header_values.push_back(Value(value));
					}
				}
			}
		}

		// Auto-decompress
		result.body = response->body;
		try {
			if (GZipFileSystem::CheckIsZip(response->body.data(), response->body.size())) {
				result.body = GZipFileSystem::UncompressGZIPString(response->body);
			} else if (CheckIsZstd(response->body.data(), response->body.size())) {
				result.body = DecompressZstd(response->body);
			}
		} catch (...) {
		}

	} catch (std::exception &e) {
		result.error = e.what();
	}

	return result;
}

//======================================================================================================================
// HttpRequest with cache Implementation
//======================================================================================================================

//! Cache entry for HTTP responses, used to store responses in memory for a limited time
struct HttpResponseCacheEntry {
	HttpResponseData response;
	std::chrono::steady_clock::time_point created_at;
};

//! Simple in-memory cache for HTTP responses
struct HttpResponseCache {
	std::mutex lock;
	std::unordered_map<std::string, HttpResponseCacheEntry> entries;
};

//! Get the global HTTP response cache instance
static HttpResponseCache &GetHttpResponseCache() {
	static HttpResponseCache cache;
	return cache;
}

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

	if (!headers.empty()) {
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
static void PruneHttpResponseCache(HttpResponseCache &cache, std::chrono::seconds ttl_seconds) {
	const auto now = std::chrono::steady_clock::now();

	for (auto it = cache.entries.begin(); it != cache.entries.end();) {
		if (now - it->second.created_at > ttl_seconds) {
			it = cache.entries.erase(it);
		} else {
			++it;
		}
	}
	if (cache.entries.size() >= HTTP_REQUEST_CACHE_MAX_ENTRIES) {
		cache.entries.erase(cache.entries.begin());
	}
}

HttpResponseData HttpRequest::ExecuteHttpRequest(ClientContext &context, const HttpSettings &settings,
                                                 const string &url, const string &method, const HttpHeaders &headers,
                                                 const string &request_body, const string &content_type,
                                                 int32_t ttl_seconds) {
	std::string cache_key;

	// Determine if caching should be used
	const bool use_cache = ttl_seconds > 0 && IsCacheableHttpMethod(method);
	if (use_cache) {
		cache_key = BuildHttpCacheKey(url, method, headers, request_body, content_type);

		auto &cache = GetHttpResponseCache();
		std::lock_guard<std::mutex> lock(cache.lock);

		auto cached_it = cache.entries.find(cache_key);
		if (cached_it != cache.entries.end()) {
			const auto now = std::chrono::steady_clock::now();

			if (now - cached_it->second.created_at <= std::chrono::seconds(ttl_seconds)) {
				return cached_it->second.response;
			}
			cache.entries.erase(cached_it);
		}
	}

	HttpResponseData result =
	    HttpRequest::ExecuteHttpRequest(context, settings, url, method, headers, request_body, content_type);

	// Store the response in the cache if caching is enabled
	if (use_cache) {
		auto &cache = GetHttpResponseCache();
		std::lock_guard<std::mutex> lock(cache.lock);

		if (cache.entries.size() >= HTTP_REQUEST_CACHE_MAX_ENTRIES) {
			PruneHttpResponseCache(cache, std::chrono::seconds(ttl_seconds));
		}
		cache.entries[cache_key] = {result, std::chrono::steady_clock::now()};
	}

	return result;
}

} // namespace duckdb
