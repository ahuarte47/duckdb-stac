# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(stac
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
    EXTENSION_VERSION 0.5.0
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
duckdb_extension_load(httpfs
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    GIT_TAG 91df60d5f1e768222c55f3ae24d520117f01deca
)

# Since DuckDB's v2.0-cyanoptera duckdb_extension_load() only decides what is built;
# the shell and the test runner link an extension only when a config asks for it.
# Linked as before the change.
duckdb_extension_statically_link(stac)
duckdb_extension_statically_link(httpfs)
