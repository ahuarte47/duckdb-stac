# DuckDB STAC Extension

A DuckDB extension for reading data from SpatioTemporal Asset Catalogs (STAC) using SQL

🚧 WORK IN PROGRESS 🚧

The extension exposes STAC catalogs as relational tables, following the
[GeoParquet STAC specification](https://radiantearth.github.io/stac-geoparquet-spec/latest/).

Each row represents a single STAC item. Almost all item fields are mapped to columns;
nested JSON structures are preserved as Parquet structs where possible, but item properties
are promoted to the top level for easier filtering and querying.

```sql
SELECT * FROM STAC_Read('https://example.com/stac/collection.json');
```
```sql
--- TODO ---
```

### Supported Functions and Documentation

The full list of functions and their documentation is available in the [function reference](docs/functions.md)

## How do I get it?

### Loading from community (TODO)

The DuckDB **STAC Extension** is available as a signed [community extension](https://duckdb.org/community_extensions/list_of_extensions).
See more details on its [DuckDB CE web page](https://duckdb.org/community_extensions/extensions/stac.html).

To install and load it, you can run the following SQL commands in DuckDB:

```sql
INSTALL stac FROM community;
LOAD stac;
```

## How do I build it?

This extension is based on the [DuckDB extension template](https://github.com/duckdb/extension-template).

### Dependencies

You need CMake ≥ 3.5 and a C++17-compatible compiler. [Ninja](https://ninja-build.org) is recommended and can be selected by setting `GEN=ninja`.

```sh
git clone --recurse-submodules https://github.com/ahuarte47/duckdb-stac
cd duckdb-stac
make release
```

Invoke the built DuckDB (with the extension statically linked):

```sh
./build/release/duckdb
```

See the Makefile or the [extension template documentation](https://github.com/duckdb/extension-template) for additional options.

### Running the tests

SQL tests live in `./test/sql` and are the primary test suite for the extension:

```sh
make test
```

### Installing a locally built binary

To load an unsigned local build, launch DuckDB with `allow_unsigned_extensions` enabled:

CLI:
```sh
duckdb -unsigned
```

Python:
```python
con = duckdb.connect(':memory:', config={'allow_unsigned_extensions': 'true'})
```

NodeJS:
```js
db = new duckdb.Database(':memory:', { allow_unsigned_extensions: 'true' });
```

Then load the extension from its local path:
```sql
LOAD 'build/release/extension/stac/stac.duckdb_extension';
```
