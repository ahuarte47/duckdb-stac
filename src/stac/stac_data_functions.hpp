#pragma once

namespace duckdb {

class ExtensionLoader;

struct STACDataFunctions {
public:
	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
