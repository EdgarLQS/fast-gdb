# Changelog

All notable changes to fast-gdb are documented in this file.

## Unreleased

### Changed

- fast-gdb is now explicitly a **Reader-only** FileGDB product.
- FileGDB creation and editing are delegated to GDAL/OpenFileGDB or ArcGIS.
- The installed package exports only Reader products: `fast_gdb::linear` and optional `fast_gdb::hybrid`.
- The supported edit workflow is now: close all fast-gdb Reader objects, edit exclusively with GDAL, close all GDAL objects, then construct a fresh Reader.
- Reader objects, mmap regions, table parsers, catalogs, query engines, cursors, FID mappings and index caches must not survive an external GDAL edit.
- Same-directory GDAL update plus fast-gdb reading is explicitly classified as unsupported and may expose old, new, mixed or error states.
- Online copy/edit/switch publication is an application responsibility outside fast-gdb.

### Added

- Focused real OpenFileGDB boundary target: `fast_gdb_gdal_read_write_boundary_test_runner`.
- A release-gate test proving that a fully closed Reader followed by GDAL edit and complete Reader reopen observes new data.
- A characterization test that records old/new/mixed/error while GDAL holds an update Dataset against the same GDB as an open fast-gdb Reader.
- ADR-007 for the Reader-only product decision and GDAL edit boundary.
- Architecture, usage and evidence documents defining Reader quiescence, GDAL lifecycle and result interpretation.
- CI checks that verify no Writer target or installed Writer headers remain.

### Removed

- `fast_gdb::writer`.
- `VersionedGdbStore`, `GdbReaderSnapshot`, `GdbWriteTransaction` and generation publication APIs.
- Installed Writer headers and the `include/fast_gdb/writer` tree.
- The self-developed FileGDB binary Writer implementation under `src/edgar/explorgdb/writer`.
- Append, Update, Delete, Transaction, Recovery, table/index Writer and atomic publication code.
- GDAL `GdbBatchWriter` and `GdbTransaction` wrapper abstractions.
- Writer tests, Writer performance tools, Writer-specific workflows, ADRs, roadmaps, design documents and evidence reports.
- Writer package-consumer mode.

### Validation status

- The Reader-only build graph, install surface and boundary tests are implemented on the development branch.
- Formal acceptance requires usable CMake/CTest logs and artifacts for the pure Reader surface and the GDAL boundary target.
- Characterization observations are diagnostic only and never constitute a concurrent read/write support statement.

## [0.1.0] - 2026-07-13

### Added

- C++17 FileGDB reader products: dependency-free `fast_gdb_linear` and optional GDAL-backed `fast_gdb_hybrid`.
- Unified `GeometryModel` / `GeometryValue` and ISO WKB-first output.
- Point, MultiPoint, Polyline and Polygon support, including multipart, holes, islands, Z, M and ZM.
- Built-in linearization for CircularArc, CubicBezier and EllipticArc descriptors.
- Exact bbox predicates and `.spx` candidate filtering through the shared geometry model.
- Explicit Hybrid GDAL fallback with configurable fast-gdb/GDAL FID offset mapping.
- CMake install targets, relocatable package configuration and CPack archives.
- Windows, Linux and macOS pure C++ CI; Linux Hybrid and sanitizer release gates.

### Fixed

- ArcGIS M-enabled 2D curves that encode the entire missing M array as a single FileGDB `0x42` marker.
- Transactional curve-tail parsing so canonical M arrays remain preferred and malformed descriptors fail closed.
- Missing M values preserve measured dimensionality and surface as `NaN` instead of consuming descriptor bytes.

### Validation

- Real `testcurve.gdb` acceptance: 2/2 `Curve_Polyline_M_FC` features use `BuiltinCurve`, with zero GDAL fallback.
- FID mapping, WKB type, bbox, length and whole-layer spatial query agree with GDAL within the published tolerance.
- Release and ASan/UBSan/LSan real-data runs report zero failures.

### Known boundaries

- Pure C++ output is linearized standard WKB; native ArcGIS curve objects are not preserved.
- MultiPatch is available only through Hybrid degraded support and does not preserve complete surface semantics.
- Unknown or future FileGDB geometry encodings are not claimed as supported.
- FileGDB editing is outside the fast-gdb product scope.
