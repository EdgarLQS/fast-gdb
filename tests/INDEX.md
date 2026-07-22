# Test Index

## Reader release gates

| Target | Scope |
|---|---|
| `fast_gdb_geometry_test_runner` | Portable geometry, topology and WKB contracts |
| `fast_gdb_hybrid_test_runner` | GDAL-backed curve fallback and real-data release contracts |
| `gdb_tutorial_test_runner` | Full Reader, query, index, GDAL parity and benchmark suite |
| `fast_gdb_gdal_read_write_boundary_test_runner` | GDAL edit / fast-gdb Reader lifecycle boundary |

## GDAL boundary tests

File:

```text
tests/usegdal/test_gdal_write_fast_reader_visibility.cpp
```

Tests:

- `SupportedQuiescedReaderWorkflowReopensWithNewData`
  - formal supported contract;
  - Reader closes before GDAL update;
  - GDAL closes before Reader reopen;
  - reopened Reader must see new data.

- `SameDirectoryReadWhileGdalWriterIsOpenIsCharacterizationOnly`
  - unsupported overlap characterization;
  - records old/new/mixed/error;
  - no overlap observation is treated as a product guarantee;
  - full close and reopen must still see new data.

Run:

```bash
cmake -S . -B build-boundary \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_BUILD_FULL_TESTS=OFF \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DBUILD_TESTING=ON
cmake --build build-boundary \
  --target fast_gdb_gdal_read_write_boundary_test_runner --parallel
ctest --test-dir build-boundary --output-on-failure \
  -R '^gdal-reader-boundary\.'
```

## Product-surface tests

`tests/package_consumer` supports only:

- `linear`;
- `hybrid`.

There is no Writer consumer mode.

## Removed test scope

The repository no longer builds or maintains:

- self-developed Writer tests;
- Append/Update/Delete/Transaction/Recovery tests;
- VersionedGdbStore tests;
- Writer performance tests;
- GDAL BatchWriter/Transaction wrapper tests.

GDAL may still be used inside Reader tests to generate FileGDB fixtures and provide correctness parity.

## Result policy

- `PASS` requires executed test steps and assertions;
- `SKIPPED` is not acceptance;
- runner failures without steps/logs are infrastructure failures;
- characterization output is diagnostic and cannot establish concurrent read/write support.
