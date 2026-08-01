#pragma once

namespace duckdb {

class ExtensionLoader;

struct STACTypesCastsFunctions {
public:
	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
