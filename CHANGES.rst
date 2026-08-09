Release history
---------------

0.2.0
++++++++++++++++++

- Support for STAC API - Filter Extension (https://github.com/stac-api-extensions/filter)
- Support for STAC API - Fields Extension (https://github.com/stac-api-extensions/fields)
- Support for STAC API - Sort Extension (https://github.com/stac-api-extensions/sort)
- Catch error message when the STAC API endpoint returns an error with content_type as JSON.
- Fix management of STAC API endpoints that require one collection at least to be specified.

0.1.1
++++++++++++++++++

- Fix performance of requests caching with the default TTL to 30 seconds.
- Fix recursive reading of STAC catalogs

0.1.0
++++++++++++++++++

- First release as DuckDB Community Extension.
