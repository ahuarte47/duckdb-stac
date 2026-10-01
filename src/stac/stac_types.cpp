#include "stac_types.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

const Value CollectionRow::NULL_VALUE = Value();

const duckdb::Value &CollectionRow::ValueOf(const idx_t &dim_index) const {
	switch (dim_index) {
	case STAC_COLLECTION_CATALOG_COLUMN_INDEX:
		return catalog;
	case STAC_COLLECTION_ID_COLUMN_INDEX:
		return id;
	case STAC_COLLECTION_TITLE_COLUMN_INDEX:
		return title;
	case STAC_COLLECTION_DESCRIPTION_COLUMN_INDEX:
		return description;
	case STAC_COLLECTION_KEYWORDS_COLUMN_INDEX:
		return keywords;
	case STAC_COLLECTION_LICENSE_COLUMN_INDEX:
		return license;
	case STAC_COLLECTION_PROVIDERS_COLUMN_INDEX:
		return providers;
	case STAC_COLLECTION_BBOX_COLUMN_INDEX:
		return bbox;
	case STAC_COLLECTION_INTERVAL_COLUMN_INDEX:
		return interval;
	case STAC_COLLECTION_SUMMARIES_COLUMN_INDEX:
		return summaries;
	case STAC_COLLECTION_VERSION_COLUMN_INDEX:
		return version;
	case STAC_COLLECTION_EXTENSIONS_COLUMN_INDEX:
		return extensions;
	case STAC_COLLECTION_LINKS_COLUMN_INDEX:
		return links;
	case STAC_COLLECTION_ASSETS_COLUMN_INDEX:
		return assets;
	case STAC_COLLECTION_ITEM_ASSETS_COLUMN_INDEX:
		return item_assets;
	default:
		throw InvalidInputException("Invalid column index for CollectionRow");
	}
}

const Value ItemRow::NULL_VALUE = Value();

const duckdb::Value &ItemRow::ValueOf(const idx_t &dim_index) const {
	switch (dim_index) {
	case STAC_ITEM_CATALOG_COLUMN_INDEX:
		return catalog;
	case STAC_ITEM_COLLECTION_COLUMN_INDEX:
		return collection;
	case STAC_ITEM_ID_COLUMN_INDEX:
		return id;
	case STAC_ITEM_GEOMETRY_COLUMN_INDEX:
		return geometry;
	case STAC_ITEM_BBOX_COLUMN_INDEX:
		return bbox;
	case STAC_ITEM_VERSION_COLUMN_INDEX:
		return version;
	case STAC_ITEM_EXTENSIONS_COLUMN_INDEX:
		return extensions;
	case STAC_ITEM_LINKS_COLUMN_INDEX:
		return links;
	case STAC_ITEM_ASSETS_COLUMN_INDEX:
		return assets;
	default: {
		// Handle dynamic properties columns
		const idx_t property_idx = dim_index - STAC_ITEM_FIRST_PROPERTY_COLUMN_INDEX;

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
