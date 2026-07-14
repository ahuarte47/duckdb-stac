#pragma once

namespace duckdb {

class ExtensionLoader;

struct STACReadFunctions {
public:
	static void Register(ExtensionLoader &loader);
};

} // namespace duckdb
