# DuckDB STAC Extension Function Reference

## Function Index

**[Table Functions](#table-functions)**

| Function | Summary |
| --- | --- |
| [`STAC_Read`](#stac_read) | Reads the content of a STAC catalog from the given URL or JSON file and returns it as a table. |

----

## Table Functions

### STAC_Read

#### Signature

```sql
STAC_Read (catalog VARCHAR)
```

#### Description

Reads the content of a SpatioTemporal Asset Catalog (STAC) catalog from the given URL or JSON file
and returns it as a table.

This function exposes a STAC catalog as a relational table, following the
[GeoParquet STAC specification](https://radiantearth.github.io/stac-geoparquet-spec/latest/).

Each row represents a single STAC item. Almost all item fields are mapped to columns;
nested JSON structures are preserved as Parquet structs where possible, but item properties
are promoted to the top level for easier filtering and querying.

#### Example

```sql
SELECT * FROM STAC_Read('https://example.com/stac/collection.json');
```

----
