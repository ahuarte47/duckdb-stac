#define DUCKDB_EXTENSION_MAIN

#include "stac_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// STAC
#include "stac/stac_read_functions.hpp"
#include "stac/stac_types.hpp"
#include "stac/stac_types_casts.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	// Register functions
	STACReadFunctions::Register(loader);
	// Register types & casts
	STACTypes::Register(loader);
	STACTypesCastsFunctions::Register(loader);
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
