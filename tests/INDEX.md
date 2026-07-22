# Test Index

## Reader release gates

| Target | Scope |
|---|---|
| `fast_gdb_geometry_test_runner` | Portable geometry, topology and WKB contracts |
| `fast_gdb_hybrid_test_runner` | GDAL-backed curve fallback and real-data release contracts |
| `gdb_tutorial_test_runner` | Full Reader, query, index and direct-GDAL parity suite |
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

There is no Writer consumer mode and no `usegdal` consumer mode.

## Reference-only `usegdal`

`src/edgar/usegdal` is retained as historical GDAL/OGR wrapper source. It is not linked into any release-gate target. Its old wrapper-specific tests are not part of the active test suite.

Formal Reader parity and lifecycle tests call the official GDAL API directly so the supported boundary is not coupled to reference wrappers.

## Removed active test scope

The repository no longer builds or maintains as release gates:

- self-developed FileGDB Writer tests;
- Append/Update/Delete/Transaction/Recovery product tests;
- VersionedGdbStore tests;
- Writer performance tests;
- GDAL BatchWriter/Transaction wrapper tests.

The corresponding `usegdal` source may remain for reference, but it is not considered tested or production-supported.

## Result policy

- `PASS` requires executed test steps and assertions;
- `SKIPPED` is not acceptance;
- runner failures without steps/logs are infrastructure failures;
- characterization output is diagnostic and cannot establish concurrent read/write support;
- reference-only source presence is not release evidence.
