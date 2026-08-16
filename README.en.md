# fast-gdb — C++ FileGDB Reader

[中文 README](README.md) | English

fast-gdb is a C++17 reader, query, and geometry parsing library for ESRI
FileGDB data. The supported product is **Reader-only**: it does not provide
FileGDB creation, append, update, delete, schema editing, transactions, or a
Writer API. Use GDAL/OpenFileGDB or ArcGIS for FileGDB editing.

Current development version: **v0.2.0**.

## Products

| Target | Purpose | GDAL | Current status |
|---|---|---:|---|
| `fast_gdb::linear` | Native C++ FileGDB reader, geometry, and queries | No | Available; locally built and tested |
| `fast_gdb::hybrid` | Native reader with optional GDAL geometry fallback | Yes | Available; locally built and tested |
| `fast_gdb::adaptive` | Coordinated Reader invalidation and fresh GDAL read-only fallback | Yes | Implemented; cross-platform, stress, and performance evidence pending |
| `fast_gdb::unified` | Source-neutral Dataset/Layer/Feature/Cursor facade | Usually | Implemented; S3 remains Experimental / Unverified |

These labels describe implementation and evidence boundaries; they are not a
claim that every platform or backend has completed release acceptance.

## Support matrix

| Scenario | Current status | Evidence boundary |
|---|---|---|
| Local `.gdb` + `linear` | Available | macOS build, full tests, and installed consumer verified locally |
| Local `.gdb` + `hybrid` | Available | macOS + GDAL build, full tests, and installed consumer verified locally |
| Coordinated `adaptive` | Implemented | Cross-platform, stress, performance, and multi-GDAL evidence pending |
| Local `unified` routing | Implemented | Local installed consumer verified; remote matrix pending |
| `s3://` / `/vsis3/` | Experimental / Unverified | AWS, directory enumeration, Range Read, offline, and performance evidence pending |
| Writer or same-GDB write/read concurrency | Unsupported | Edit exclusively with GDAL/OpenFileGDB, then recreate Readers |

`src/reference/usegdal` contains historical GDAL/OGR wrapper experiments. It
is not built, installed, exported, or covered by the supported API contract.

## Supported capabilities

- Point, MultiPoint, Polyline, Polygon, and Z/M/ZM dimensions;
- multipart geometries, holes, islands, and ring-direction-independent
  topology;
- built-in linearization for selected curve descriptors;
- `.spx`, `.atx`, WHERE, bounding-box, and combined attribute/spatial queries;
- random FID reads, sequential scans, and `FeatureCursor`;
- ISO WKB-first output with WKT conversion on demand.

MultiPatch has only degraded Hybrid support and does not preserve complete
surface topology. S3 and `/vsis3/` routing is **Experimental / Unverified**
until the documented remote acceptance evidence is complete.

## Build

### Native Reader

```bash
cmake -S . -B build-linear \
  -DFAST_GDB_WITH_GDAL=OFF \
  -DBUILD_TESTING=ON
cmake --build build-linear --parallel
ctest --test-dir build-linear --output-on-failure
```

### GDAL-backed build

```bash
cmake -S . -B build-hybrid \
  -DFAST_GDB_WITH_GDAL=ON \
  -DBUILD_TESTING=ON
cmake --build build-hybrid --parallel
ctest --test-dir build-hybrid --output-on-failure
```

The CMake project uses `find_package(GDAL)`. Full tests use GoogleTest; CMake
may fetch GoogleTest `v1.15.2` when no compatible installed package exists.
See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for dependency and
fixture provenance.

### Install

```bash
cmake --install build-linear --prefix install-fast-gdb
find install-fast-gdb/share/fast_gdb -maxdepth 1 -type f | sort
```

The install contains `README.md`, `README.en.md`, `CHANGELOG.md`, `LICENSE`,
and `THIRD_PARTY_NOTICES.md`. Replace `build-linear` with `build-hybrid` for
the GDAL-backed installation.

## FileGDB editing boundary

The supported lifecycle is:

```text
close all fast-gdb Readers
→ edit exclusively with GDAL/OpenFileGDB
→ close all GDAL objects
→ construct fresh fast-gdb Readers
```

Keeping a fast-gdb Reader open while GDAL updates the same `.gdb` directory is
unsupported and may expose old, new, mixed, or error states.

## Documentation and contribution

- [中文文档入口](docs/README.md)
- [English contribution guide](CONTRIBUTING.md)
- [Security policy](SECURITY.md)
- [Changelog](CHANGELOG.md)
- [Quality and acceptance documents](docs/quality/README.md)

## License

Original fast-gdb source code and documentation are licensed under
[Apache-2.0](LICENSE). Third-party dependencies and generated test fixtures
retain the notices described in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
