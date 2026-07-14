#include "json_object.hpp"
#include "stac_types.hpp"

namespace duckdb {

LogicalType JsonObject::GetPropertyTypeOfJsonValue(yyjson_val *val) {
	if (yyjson_is_bool(val)) {
		return LogicalType::BOOLEAN;
	}
	if (yyjson_is_int(val)) {
		return LogicalType::INTEGER;
	}
	if (yyjson_is_uint(val)) {
		return LogicalType::UBIGINT;
	}
	if (yyjson_is_real(val)) {
		return LogicalType::DOUBLE;
	}
	if (yyjson_is_str(val)) {
		return LogicalType::VARCHAR;
	}
	return LogicalType::JSON();
}

Value JsonObject::GetPropertyValueOfJsonValue(yyjson_val *val) {
	if (!val || yyjson_is_null(val)) {
		return Value();
	}
	if (yyjson_is_bool(val)) {
		return Value::BOOLEAN(yyjson_get_bool(val));
	}
	if (yyjson_is_int(val)) {
		return Value::INTEGER(yyjson_get_int(val));
	}
	if (yyjson_is_uint(val)) {
		return Value::UBIGINT(yyjson_get_uint(val));
	}
	if (yyjson_is_real(val)) {
		return Value::DOUBLE(yyjson_get_real(val));
	}
	if (yyjson_is_str(val)) {
		return Value(yyjson_get_str(val));
	}

	char *json_str = yyjson_val_write(val, YYJSON_WRITE_NOFLAG, nullptr);
	if (json_str) {
		Value json_value = Value(json_str);
		free(json_str);
		return json_value;
	}
	return Value();
}

Value JsonObject::ParseBoundingBoxObject(yyjson_val *bbox_val) {
	if (yyjson_is_arr(bbox_val) && yyjson_arr_size(bbox_val) >= 4) {
		yyjson_val *xmin_val = yyjson_arr_get(bbox_val, 0);
		yyjson_val *ymin_val = yyjson_arr_get(bbox_val, 1);
		yyjson_val *xmax_val = yyjson_arr_get(bbox_val, 2);
		yyjson_val *ymax_val = yyjson_arr_get(bbox_val, 3);

		if (yyjson_is_real(xmin_val) && yyjson_is_real(ymin_val) && yyjson_is_real(xmax_val) &&
		    yyjson_is_real(ymax_val)) {
			double xmin = yyjson_get_real(xmin_val);
			double ymin = yyjson_get_real(ymin_val);
			double xmax = yyjson_get_real(xmax_val);
			double ymax = yyjson_get_real(ymax_val);

			Value bbox = Value::STRUCT({{"xmin", xmin}, {"ymin", ymin}, {"xmax", xmax}, {"ymax", ymax}});
			bbox.Reinterpret(STACTypes::BBOX());
			return bbox;
		}
	}
	return Value();
}

Value JsonObject::ParseLinksObject(yyjson_val *links_val) {
	vector<Value> links;

	if (yyjson_is_arr(links_val)) {
		yyjson_arr_iter iter;
		yyjson_arr_iter_init(links_val, &iter);
		yyjson_val *link;

		while ((link = yyjson_arr_iter_next(&iter))) {
			if (yyjson_is_obj(link)) {
				yyjson_val *href_val = yyjson_obj_get(link, "href");
				yyjson_val *type_val = yyjson_obj_get(link, "type");
				yyjson_val *title_val = yyjson_obj_get(link, "title");
				yyjson_val *rel_val = yyjson_obj_get(link, "rel");

				if (!yyjson_is_str(href_val) || !yyjson_is_str(rel_val)) {
					continue; // Skip invalid link objects
				}

				std::string href = yyjson_get_str(href_val);
				std::string type = yyjson_is_str(type_val) ? yyjson_get_str(type_val) : "";
				std::string title = yyjson_is_str(title_val) ? yyjson_get_str(title_val) : "";
				std::string rel = yyjson_get_str(rel_val);

				Value link = Value::STRUCT({{"href", href}, {"type", type}, {"title", title}, {"rel", rel}});
				link.Reinterpret(STACTypes::LINK());

				links.push_back(link);
			}
		}
	}
	return Value::LIST(STACTypes::LINK(), links);
}

Value JsonObject::ParseAssetsObject(yyjson_val *assets_val) {
	vector<Value> keys;
	vector<Value> assets;

	if (yyjson_is_obj(assets_val)) {
		yyjson_obj_iter iter;
		yyjson_obj_iter_init(assets_val, &iter);
		yyjson_val *key, *val;

		while ((key = yyjson_obj_iter_next(&iter))) {
			if (yyjson_is_str(key) && (val = yyjson_obj_iter_get_val(key)) && yyjson_is_obj(val)) {
				yyjson_val *href_val = yyjson_obj_get(val, "href");
				yyjson_val *type_val = yyjson_obj_get(val, "type");
				yyjson_val *title_val = yyjson_obj_get(val, "title");
				yyjson_val *descr_val = yyjson_obj_get(val, "description");
				yyjson_val *roles_val = yyjson_obj_get(val, "roles");

				if (!yyjson_is_str(href_val)) {
					continue; // Skip invalid asset objects
				}

				std::string href = yyjson_is_str(href_val) ? yyjson_get_str(href_val) : "";
				std::string type = yyjson_is_str(type_val) ? yyjson_get_str(type_val) : "";
				std::string title = yyjson_is_str(title_val) ? yyjson_get_str(title_val) : "";
				std::string descr = yyjson_is_str(descr_val) ? yyjson_get_str(descr_val) : "";
				vector<Value> roles;

				if (yyjson_is_arr(roles_val)) {
					yyjson_arr_iter role_iter;
					yyjson_arr_iter_init(roles_val, &role_iter);
					yyjson_val *role_val;

					while ((role_val = yyjson_arr_iter_next(&role_iter))) {
						if (yyjson_is_str(role_val)) {
							roles.push_back(Value(yyjson_get_str(role_val)));
						}
					}
				}

				Value asset = Value::STRUCT({{"href", href},
				                             {"type", type},
				                             {"title", title},
				                             {"description", descr},
				                             {"roles", Value::LIST(LogicalType::VARCHAR, roles)}});
				asset.Reinterpret(STACTypes::ASSET());

				keys.emplace_back(yyjson_get_str(key));
				assets.push_back(asset);
			}
		}
	}
	return Value::MAP(LogicalType::VARCHAR, STACTypes::ASSET(), std::move(keys), std::move(assets));
}

Value JsonObject::ParseExtensionsObject(yyjson_val *extensions_val) {
	vector<Value> extensions;

	if (yyjson_is_arr(extensions_val)) {
		yyjson_arr_iter iter;
		yyjson_arr_iter_init(extensions_val, &iter);
		yyjson_val *ext_val;

		while ((ext_val = yyjson_arr_iter_next(&iter))) {
			if (yyjson_is_str(ext_val)) {
				extensions.push_back(Value(yyjson_get_str(ext_val)));
			}
		}
	}
	return Value::LIST(LogicalType::VARCHAR, extensions);
}

} // namespace duckdb
