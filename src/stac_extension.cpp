#define DUCKDB_EXTENSION_MAIN

#include "stac_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

namespace duckdb {

inline void StacScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "...........🦆 " + name.GetString());
	});
}

inline void StacOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "Stac " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto stac_scalar_function =
	    ScalarFunction("stac", {LogicalType::VARCHAR}, LogicalType::VARCHAR, StacScalarFun);

	loader.RegisterFunction(stac_scalar_function);

	// Register another scalar function
	auto stac_openssl_version_scalar_function = ScalarFunction("stac_openssl_version", {LogicalType::VARCHAR},
	                                                             LogicalType::VARCHAR, StacOpenSSLVersionScalarFun);
	loader.RegisterFunction(stac_openssl_version_scalar_function);
}

void StacExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string StacExtension::Name() {
	return "stac";
}

std::string StacExtension::Version() const {
#ifdef EXT_VERSION_STAC
	return EXT_VERSION_STAC;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(stac, loader) {
	duckdb::LoadInternal(loader);
}
}
