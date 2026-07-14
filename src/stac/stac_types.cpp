#include "stac_types.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

LogicalType STACTypes::BBOX() {
	auto bbox_type = LogicalType::STRUCT({{"minx", LogicalType::DOUBLE},
	                                      {"miny", LogicalType::DOUBLE},
	                                      {"maxx", LogicalType::DOUBLE},
	                                      {"maxy", LogicalType::DOUBLE}});
	bbox_type.SetAlias("STAC_BBOX");
	return bbox_type;
}

LogicalType STACTypes::LINK() {
	auto link_type = LogicalType::STRUCT({{"href", LogicalType::VARCHAR},
	                                      {"type", LogicalType::VARCHAR},
	                                      {"title", LogicalType::VARCHAR},
	                                      {"rel", LogicalType::VARCHAR}});
	link_type.SetAlias("STAC_LINK");
	return link_type;
}

LogicalType STACTypes::ASSET() {
	auto asset_type = LogicalType::STRUCT({{"href", LogicalType::VARCHAR},
	                                       {"type", LogicalType::VARCHAR},
	                                       {"title", LogicalType::VARCHAR},
	                                       {"description", LogicalType::VARCHAR},
	                                       {"roles", LogicalType::LIST(LogicalType::VARCHAR)}});
	asset_type.SetAlias("STAC_ASSET");
	return asset_type;
}

void STACTypes::Register(ExtensionLoader &loader) {
	// Register types
	loader.RegisterType("STAC_BBOX", STACTypes::BBOX());
	loader.RegisterType("STAC_LINK", STACTypes::LINK());
	loader.RegisterType("STAC_ASSET", STACTypes::ASSET());
}

} // namespace duckdb
