# DuckDB STAC Extension

A DuckDB extension for reading data from [SpatioTemporal Asset Catalogs](https://stacspec.org/) (STAC) using SQL.

The STAC specification is a common language to describe geospatial information, so it can more easily be
worked with, indexed, and discovered.

The extension exposes STAC catalogs as relational tables, following the
[GeoParquet STAC specification](https://radiantearth.github.io/stac-geoparquet-spec/latest/).

Each row represents a single STAC item. Almost all item fields are mapped to columns;
nested JSON structures are preserved as Parquet structs where possible, but item properties
are promoted to the top level for easier filtering and querying.

```sql
SELECT * FROM STAC_Read('https://example.com/stac/collection.json');
```

## How do I get it?

### Loading from community

The DuckDB **STAC Extension** is available as a signed [community extension](https://duckdb.org/community_extensions/list_of_extensions).
See more details on its [DuckDB CE web page](https://duckdb.org/community_extensions/extensions/stac.html).

To install and load it, you can run the following SQL commands in DuckDB:

```sql
INSTALL stac FROM community;
LOAD stac;
```

## Function Reference

**[Table Functions](docs/functions.md#table-functions)**

| Function | Summary |
| --- | --- |
| [`STAC_Read`](docs/functions.md#stac_read) | Reads the content of a STAC catalog from the given URL or JSON file and returns it as a table. |
| [`STAC_Search`](docs/functions.md#stac_search) | Searches a STAC catalog based on the given criteria and returns matching items as a table. |

## Examples

More examples are available in the [SQL tests](test/sql) used by the CI pipeline.

### Reading STAC items from a static Catalog

```sql
LOAD httpfs;
SELECT
    id, bbox, "cs:anomalous_pixels", "cs:sat_id"
FROM
    STAC_Read('https://raw.githubusercontent.com/radiantearth/stac-spec/refs/heads/master/examples/catalog.json')
WHERE
    "cs:sat_id" IS NOT NULL
;
```
```sql
┌────────────────────────┬──────────────────────────────────────────────────────────────────────────────────────────┬─────────────────────┬───────────┐
│           id           │                                           bbox                                           │ cs:anomalous_pixels │ cs:sat_id │
│        varchar         │                                        stac_bbox                                         │       double        │  varchar  │
├────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────┼─────────────────────┼───────────┤
│ CS3-20160503_132131_08 │ {'minx': -122.59750209, 'miny': 37.48803556, 'maxx': -122.2880486, 'maxy': 37.613537207} │                0.14 │ CS3       │
└────────────────────────┴──────────────────────────────────────────────────────────────────────────────────────────┴─────────────────────┴───────────┘
```

Of course, you can also read STAC items from a local JSON file, and a STAC API endpoint:

```sql
SELECT
    collection, id, geometry, "eo:constellation", "aac:collection_display_name"
FROM
    STAC_Read('https://eod-catalog-svc-prod.astraea.earth/search?limit=10')
LIMIT
    5
;
```
```sql
┌───────────────┬────────────────────────────────────────────────────────┬────────────────────────────────────────────────────────┬──────────────────┬─────────────────────────────┐
│  collection   │                           id                           │                        geometry                        │ eo:constellation │ aac:collection_display_name │
│    varchar    │                        varchar                         │                 geometry('epsg:4326')                  │     varchar      │           varchar           │
├───────────────┼────────────────────────────────────────────────────────┼────────────────────────────────────────────────────────┼──────────────────┼─────────────────────────────┤
│ sentinel2_l2a │ S2A_OPER_MSI_L2A_TL_EPAE_20190527T094026_A020508_T46VC │ MULTIPOLYGON (((91.2213 62.9634, 91.2702 62.1332, 90.6 │ sentinel-2       │ Sentinel-2 L2A              │
│               │ Q_N02.12                                               │ 357 62.1238, 91.2213 62.9634)))                        │                  │                             │
├───────────────┼────────────────────────────────────────────────────────┼────────────────────────────────────────────────────────┼──────────────────┼─────────────────────────────┤
│ sentinel2_l2a │ S2A_OPER_MSI_L2A_TL_MTI__20190518T203951_A020386_T24XW │ MULTIPOLYGON (((-39.0008 78.373, -34.1321 78.3321, -34 │ sentinel-2       │ Sentinel-2 L2A              │
│               │ M_N02.12                                               │ .2049 78.1528, -36.0053 77.8995, -38.9116 77.4454, -39 │                  │                             │
│               │                                                        │ .0008 77.4285, -39.0008 78.373)))                      │                  │                             │
├───────────────┼────────────────────────────────────────────────────────┼────────────────────────────────────────────────────────┼──────────────────┼─────────────────────────────┤
│ sentinel2_l2a │ S2A_OPER_MSI_L2A_TL_SGS__20190517T193936_A020371_T18QU │ MULTIPOLYGON (((-75.8478 17.0947, -75.8438 16.1908, -7 │ sentinel-2       │ Sentinel-2 L2A              │
│               │ D_N02.12                                               │ 6.0528 16.1898, -75.8478 17.0947)))                    │                  │                             │
├───────────────┼────────────────────────────────────────────────────────┼────────────────────────────────────────────────────────┼──────────────────┼─────────────────────────────┤
│ sentinel2_l2a │ S2B_OPER_MSI_L2A_TL_EPAE_20190709T203410_A012221_T16RC │ MULTIPOLYGON (((-87.9334 29.8112, -87.9246 28.8363, -8 │ sentinel-2       │ Sentinel-2 L2A              │
│               │ T_N02.13                                               │ 8.18 28.8343, -87.9334 29.8112)))                      │                  │                             │
├───────────────┼────────────────────────────────────────────────────────┼────────────────────────────────────────────────────────┼──────────────────┼─────────────────────────────┤
│ sentinel2_l2a │ S2B_OPER_MSI_L2A_TL_EPAE_20190804T064309_A012585_T52WD │ MULTIPOLYGON (((129.268 70.9839, 129.258 70.2181, 128. │ sentinel-2       │ Sentinel-2 L2A              │
│               │ D_N02.13                                               │ 263 70.2168, 128.754 70.5973, 129.268 70.9839)))       │                  │                             │
└───────────────┴────────────────────────────────────────────────────────┴────────────────────────────────────────────────────────┴──────────────────┴─────────────────────────────┘
```

### Search STAC items from a STAC API item-search endpoint

Searches the content of a SpatioTemporal Asset Catalog (STAC) catalog based on the given STAC API - Item Search
filtering criteria (https://api.stacspec.org/v1.0.0/item-search/) and returns matching items as a table.

The `url` parameter specifies the base URL of the STAC API - Item Search endpoint to query.
The optional parameters allow filtering by different criteria:

* `collections`: A list of collection IDs to filter the search results.
* `ids`: A list of item IDs to filter the search results.
* `bbox`: A bounding box to filter items by spatial intersection, specified as an array of four floats representing the minimum longitude, minimum latitude, maximum longitude, and maximum latitude.
* `intersects`: A geometry object (EPSG:4326) to filter items by spatial intersection.
* `datetime`: A string representing a temporal range to filter the search results, specified in the format "start_datetime/end_datetime" (e.g., "2021-01-01T00:00:00Z/2021-12-31T23:59:59Z").
* `max_items`: An integer specifying the maximum number of items to return in each result page.

```sql
SELECT
    collection, id, bbox, geometry, "eo:cloud_cover"
FROM
    STAC_Search(
        'https://earth-search.aws.element84.com/v0/search',
        collections := ['sentinel-s2-l2a-cogs'],
        ids := ['S2B_30TWN_20210930_0_L2A', 'S2B_30TXN_20210930_0_L2A']
    )
;
```
```sql
┌──────────────────────┬──────────────────────────┬───────────────────────────────────────────────────────┬───────────────────────────────────────────────────────┬────────────────┐
│      collection      │            id            │                         bbox                          │                       geometry                        │ eo:cloud_cover │
│       varchar        │         varchar          │                       stac_bbox                       │                 geometry('epsg:4326')                 │     int32      │
├──────────────────────┼──────────────────────────┼───────────────────────────────────────────────────────┼───────────────────────────────────────────────────────┼────────────────┤
│ sentinel-s2-l2a-cogs │ S2B_30TWN_20210930_0_L2A │ {                                                     │ POLYGON ((-1.667 42.3563, -3.00023 42.3641, -3.00023  │             13 │
│                      │                          │   'minx': -3.0002344509650487,                        │ 43.3528, -1.64557 43.3448, -1.667 42.3563))           │                │
│                      │                          │   'miny': 42.356331534440834,                         │                                                       │                │
│                      │                          │   'maxx': -1.6455652046460576,                        │                                                       │                │
│                      │                          │   'maxy': 43.35284638738093                           │                                                       │                │
│                      │                          │ }                                                     │                                                       │                │
├──────────────────────┼──────────────────────────┼───────────────────────────────────────────────────────┼───────────────────────────────────────────────────────┼────────────────┤
│ sentinel-s2-l2a-cogs │ S2B_30TXN_20210930_0_L2A │ {                                                     │ POLYGON ((-0.453381 42.3358, -1.7857 42.3577, -1.7661 │             10 │
│                      │                          │   'minx': -1.7857005522228677,                        │ 8 43.3462, -0.412477 43.3236, -0.453381 42.3358))     │                │
│                      │                          │   'miny': 42.335791505536065,                         │                                                       │                │
│                      │                          │   'maxx': -0.4124765189393069,                        │                                                       │                │
│                      │                          │   'maxy': 43.3461908738427                            │                                                       │                │
│                      │                          │ }                                                     │                                                       │                │
└──────────────────────┴──────────────────────────┴───────────────────────────────────────────────────────┴───────────────────────────────────────────────────────┴────────────────┘
```

Or using spatial and temporal filters:

```sql
SELECT
    collection, id, bbox, geometry, "eo:cloud_cover"
FROM
    STAC_Search(
        'https://earth-search.aws.element84.com/v0/search',
        collections := ['sentinel-s2-l2a-cogs'],
        datetime := '2021-09-30/2021-10-30',
        bbox := [-1.695007724869786, 42.788757186108654, -1.604482013650674, 42.84244150196227]
    )
WHERE
    "eo:cloud_cover" < 13
;
```

```sql
SELECT
    collection, id, bbox, geometry, "eo:cloud_cover"
FROM
    STAC_Search(
        'https://earth-search.aws.element84.com/v0/search',
        collections := ['sentinel-s2-l2a-cogs'],
        datetime := '2021-09-30/2021-10-30',
        intersects := ST_MakeEnvelope(-1.695007724869786, 42.788757186108654, -1.604482013650674, 42.84244150196227)::GEOMETRY('EPSG::4326')
    )
WHERE
    "eo:cloud_cover" < 13
;
```

For the full function reference and all available options, see [docs/functions.md](docs/functions.md).

## TODO

* Support for STAC API filtering with CQL2 (https://github.com/stac-api-extensions/filter)

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

Enjoy!
