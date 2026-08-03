#include "stac_types.hpp"
#include "stac_types_casts.hpp"

// DuckDB
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/cast/default_casts.hpp"

namespace duckdb {

namespace {

//======================================================================================================================
// STAC types Casts
//======================================================================================================================

struct STACTypesCasts {
	//------------------------------------------------------------------------------------------------------------------
	// STAC_ASSET -> STRUCT
	//------------------------------------------------------------------------------------------------------------------

	static bool Asset2Struct(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return DefaultCasts::NopCast(source, result, count, parameters);
	}

	//------------------------------------------------------------------------------------------------------------------
	// STAC_BBOX -> STRUCT
	//------------------------------------------------------------------------------------------------------------------

	static bool BBox2Struct(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return DefaultCasts::NopCast(source, result, count, parameters);
	}

	//------------------------------------------------------------------------------------------------------------------
	// STAC_LINK -> STRUCT
	//------------------------------------------------------------------------------------------------------------------

	static bool Link2Struct(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
		return DefaultCasts::NopCast(source, result, count, parameters);
	}

	//------------------------------------------------------------------------------------------------------------------
	// Register
	//------------------------------------------------------------------------------------------------------------------

	static void Register(ExtensionLoader &loader) {
		// STAC_ASSET -> STRUCT
		loader.RegisterCastFunction(STACTypes::ASSET(), LogicalType(LogicalTypeId::STRUCT), Asset2Struct, 0);
		// STAC_BBOX -> STRUCT
		loader.RegisterCastFunction(STACTypes::BBOX(), LogicalType(LogicalTypeId::STRUCT), BBox2Struct, 0);
		// STAC_LINK -> STRUCT
		loader.RegisterCastFunction(STACTypes::LINK(), LogicalType(LogicalTypeId::STRUCT), Link2Struct, 0);
	}
};

} // namespace

// ######################################################################################################################
//  Register
// ######################################################################################################################

void STACTypesCastsFunctions::Register(ExtensionLoader &loader) {
	STACTypesCasts::Register(loader);
}

} // namespace duckdb
