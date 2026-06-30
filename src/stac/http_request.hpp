#pragma once

#include "duckdb.hpp"

#ifndef __EMSCRIPTEN__
// Use httplib directly for full HTTP method support
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"
#endif

namespace duckdb {

// *** NOTE:
// 	Code in this file was extracted from 'duckdb_http_request' extension:
// 	https://github.com/midwork-finds-jobs/duckdb_http_request
// 	Thanks a lot to Onni Hakala (onnimonni) for open sourcing it!

//! Struct to hold HTTP settings extracted from context (thread-safe to pass to workers)
struct HttpSettings {
	uint64_t timeout;
	bool keep_alive;
	string proxy;
	idx_t proxy_port = 80;
	string proxy_username;
	string proxy_password;
	string user_agent;
	uint64_t max_concurrency;
	bool use_cache;
	bool follow_redirects;
};

//! Struct to hold HTTP headers map
using HttpHeaders = unordered_map<string, string>;

//! Struct to hold HTTP response
struct HttpResponseData {
	int32_t status_code;
	string content_type;
	int64_t content_length;
	vector<Value> header_keys;
	vector<Value> header_values;
	vector<Value> cookies;
	string body;
	string error; // Non-empty if request failed
};

//! Represents an HTTP request
struct HttpRequest {
	// Extract HTTP settings from context
	static HttpSettings ExtractHttpSettings(ClientContext &context, const string &url);

	// Execute HTTP request with given settings
	static HttpResponseData ExecuteHttpRequest(const HttpSettings &settings, const string &url, const string &method,
	                                           const HttpHeaders &headers, const string &request_body,
	                                           const string &content_type);
};

} // namespace duckdb
