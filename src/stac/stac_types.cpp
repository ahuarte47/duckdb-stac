#include "stac_types.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

const Value ItemRow::NULL_VALUE = Value();

const duckdb::Value &ItemRow::ValueOf(const idx_t &dim_index) const {
	switch (dim_index) {
	case STAC_CATALOG_COLUMN_INDEX:
		return catalog;
	case STAC_COLLECTION_COLUMN_INDEX:
		return collection;
	case STAC_ID_COLUMN_INDEX:
		return id;
	case STAC_GEOMETRY_COLUMN_INDEX:
		return geometry;
	case STAC_BBOX_COLUMN_INDEX:
		return bbox;
	case STAC_EXTENSIONS_COLUMN_INDEX:
		return extensions;
	case STAC_LINKS_COLUMN_INDEX:
		return links;
	case STAC_ASSETS_COLUMN_INDEX:
		return assets;
	default: {
		// Handle dynamic properties columns
		const idx_t property_idx = dim_index - STAC_FIRST_PROPERTY_COLUMN_INDEX;

		auto it = this->properties.find(property_idx);
		if (it != this->properties.end()) {
			return it->second;
		} else {
			return ItemRow::NULL_VALUE;
		}
	}
	}
}

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
