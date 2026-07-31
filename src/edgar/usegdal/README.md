# usegdal reference layer

This directory is retained as **reference material only**.

It contains an earlier C++ RAII/wrapper exploration around the official GDAL/OGR API, including datasource, dataset, recordset, field, feature, query, connection-pool, transaction, and batch-write examples.

## Status

- not part of the fast-gdb Reader product;
- not compiled by the root `CMakeLists.txt`;
- not installed;
- not exported through `fast_gdbConfig.cmake`;
- no API/ABI compatibility promise;
- no correctness, transaction, concurrency, or performance guarantee;
- may be used for design comparison, experiments, or future standalone work.

## Reader-only boundary

The supported fast-gdb product remains:

- `fast_gdb::linear`;
- optional `fast_gdb::hybrid`.

FileGDB editing in production code should call the official GDAL/OpenFileGDB API directly. The presence of write-oriented examples in this reference directory does **not** mean fast-gdb provides a Writer product.

For the supported lifecycle contract, see:

- `docs/governance/adr/ADR-007-reader-only-gdal-edit-boundary.md`;
- `docs/quality/03_GDAL边界与读写测试.md`;
- `tests/usegdal/test_gdal_write_fast_reader_visibility.cpp`.

## Maintenance rule

Changes here must not add dependencies from the Reader libraries, install surface, package consumer, or release gates back to this directory. If this code is ever promoted into a supported component, it requires a separate ADR, target name, compatibility policy, and test matrix.
