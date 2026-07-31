# Changelog

All notable changes to fast-gdb are documented in this file.

## [0.2.0] - 2026-07-31

### Changed

- fast-gdb is now explicitly a **Reader-only** FileGDB product.
- FileGDB creation and editing are delegated to GDAL/OpenFileGDB or ArcGIS.
- The installed package exports only Reader products: `fast_gdb::linear`,
  optional `fast_gdb::hybrid`, optional `fast_gdb::adaptive`, and optional
  `fast_gdb::unified`.
- The supported edit workflow is now: close all fast-gdb Reader objects, edit exclusively with GDAL, close all GDAL objects, then construct a fresh Reader.
- Reader objects, mmap regions, table parsers, catalogs, query engines, cursors, FID mappings and index caches must not survive an external GDAL edit.
- Same-directory GDAL update plus fast-gdb reading is explicitly classified as unsupported and may expose old, new, mixed or error states.
- Online copy/edit/switch publication is an application responsibility outside fast-gdb.
- `src/edgar/usegdal` is retained as reference-only historical GDAL/OGR wrapper code. It is not built, installed, exported, release-gated, or covered by API/ABI compatibility promises.
- The optional Adaptive Reader is implemented as a Reader-only orchestration layer. Writer-active requests fail closed; source changes expire old Reader state; recovery uses a fresh GDAL read-only Dataset only after the source is quiescent.
- Coordinated `writer_active/generation` detection is the accepted deterministic contract. Uncoordinated external Writer detection is explicitly best-effort and cannot guarantee discovery of every Writer lifecycle.

### Added

- `fast_gdb::unified` source-neutral C++17 facade with owned
  `Dataset/Group/Layer/Feature/FeatureCursor` values.
- `Auto`, `FastOnly` and `GdalOnly` backend selection for local FileGDB and
  `s3://`/`/vsis3/` source classification.
- Shared `fast_gdb_runtime` dynamic library for routing, OpenFileGDB access and
  the process-wide Reader/Writer coordinator.
- Read-only `gdal_FastFileGDB` plugin with explicit registration, runtime build
  ID validation and update-open rejection.
- Frozen schemas, field Unset/Null/Value states, positive signed 64-bit FIDs,
  complete Feature Dataset groups, bounded `read_all()`, query reports and
  FastOnly native record extensions.
- Installed `fast_gdb::unified` package-consumer mode and GDAL-versioned plugin
  directory.

### Fixed

- Unified query validation now rejects invalid filters, projections and
  envelopes before backend selection.
- `FidAscending` and native raw reads enforce allocation budgets before
  materialization.
- Feature Dataset paths use exact GDAL Group traversal; unique layer/group
  lookup is ASCII case-insensitive.
- Fast/GDAL schema parity now includes aliases, defaults, generated
  Shape_Area/Shape_Length fields and normalized ISO Z/M geometry types.
- GDAL-only opens honor `include_system_tables`, and FastFileGDB Layer metadata
  follows the actual backend and route after query fallback.
- NULL and Empty geometry states remain distinct through Reader, facade and
  `FastFileGDB`; plugin schemas preserve field domain names.
- Runtime DLL public classes are exported on Windows, and release packages
  build every installed target before installation.
- Release publishing now waits for Linux GDAL 3.9–3.13 plus macOS/Windows
  unified matrices, including installed-plugin auto-load verification.

- Focused real OpenFileGDB boundary target: `fast_gdb_gdal_read_write_boundary_test_runner`.
- A release-gate test proving that a fully closed Reader followed by GDAL edit and complete Reader reopen observes new data.
- A characterization test that records old/new/mixed/error while GDAL holds an update Dataset against the same GDB as an open fast-gdb Reader.
- ADR-007 for the Reader-only product decision and GDAL edit boundary.
- Architecture, usage and evidence documents defining Reader quiescence, GDAL lifecycle and result interpretation.
- CI checks that verify no Writer target or installed Writer headers remain.
- `src/edgar/usegdal/README.md` documenting the non-product reference boundary.
- Accepted ADR-008 defining Adaptive Reader write-activity detection, source-change validation, Reader invalidation and fresh GDAL read-only recovery.
- Active design-first implementation plan `docs/planning/22_AdaptiveReader写入检测与GDAL回退计划.md`, including phased implementation, test names, stress gates and performance budgets.

### Removed

- `fast_gdb::writer`.
- `VersionedGdbStore`, `GdbReaderSnapshot`, `GdbWriteTransaction` and generation publication APIs.
- Installed Writer headers and the `include/fast_gdb/writer` tree.
- The self-developed FileGDB binary Writer implementation under `src/edgar/explorgdb/writer`.
- Append, Update, Delete, Transaction, Recovery, table/index Writer and atomic publication code from the supported product surface.
- Writer tests, Writer performance tools, Writer-specific workflows, ADRs, roadmaps, design documents and evidence reports.
- Writer package-consumer mode.
- The `usegdal` wrapper library from root CMake targets, installation, package exports and formal test gates; its source remains available as reference material.

### Validation status

- The Reader-only build graph, install surface and boundary tests are implemented on the development branch.
- Formal acceptance requires usable CMake/CTest logs and artifacts for the pure Reader surface and the GDAL boundary target.
- Characterization observations are diagnostic only and never constitute a concurrent read/write support statement.
- Reference-only `usegdal` code is intentionally not claimed as build-validated or production-ready.
- ADR-008 is Accepted for the optional Adaptive target. Its local runtime, coordination tests, GDAL write/reopen matrix and install consumer are implemented; cross-platform, sanitizer, multi-GDAL and performance evidence remain separate gates. ADR-007 remains the external-edit contract for uncoordinated Writer workflows.
- ADR-009 is Accepted for the local facade and plugin. S3 routing remains
  Experimental / Unverified until real AWS fixtures, failure injection and
  performance characterization pass.
- Current local evidence: GDAL 3.13 has 461 PASS, 21 SKIPPED and 0 FAIL;
  GDAL OFF has 120 PASS; GDAL 3.9.3, ASan/UBSan and TSan focused gates each
  have 31 PASS plus one real-AWS SKIPPED; all three package consumers pass.

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
