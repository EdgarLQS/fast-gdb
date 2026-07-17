# Changelog

All notable changes to fast-gdb are documented in this file.

## Unreleased

### Added

- Branch-only `QueryKind::SpatialWhere` entry that combines an exact bbox query with the existing WHERE subset.
- `CombinedQueryMetrics` diagnostics for spatial candidates, attribute candidates, WHERE rechecks, final matches and stage timings.
- Internal WHERE tokenizer/parser/evaluator module shared by standalone and combined query paths.
- Field-to-attribute-index resolution through `.gdbindexes`, including direct-field versus functional-index classification.
- Sparse candidate-field scanning for mmap and fd reader paths.
- GDAL equivalence tests for Point, Polyline, Polygon with holes, MultiPoint, Z/M/ZM, NULL, Unicode, functional indexes and missing/damaged indexes.
- GDAL-OFF synthetic `.atx` safety tests and an opt-in 100K schema-v2 combined-query benchmark.
- Installed `fast_gdb::writer` target for the supported empty-schema bulk-write workflow.
- Writer benchmark JSON/CSV evidence with correctness, median/p95, throughput, peak RSS and disk metrics.

### Changed

- `.atx` parsing now fails closed on invalid file size, page bounds, page capacity, cyclic leaf chains, trailer count mismatches and zero FIDs.
- Attribute-index candidates are sorted and deduplicated before intersection and are always followed by a complete WHERE recheck in `SpatialWhere`.
- Unsafe candidate cases, including non-BMP strings, `!=`, functional indexes and ambiguous physical numeric encodings, fall back to exact spatial matches plus full WHERE evaluation.
- GDAL integration tests use per-test temporary FileGDB directories to support parallel CTest execution.
- Combined-query benchmark evidence records the observed execution path and correctness result rather than hard-coded values.
- Writer now rejects unsafe non-empty mutation, validates field/geometry/time values, supports atomic directory publication, and rebuilds spatial/attribute indexes after writing.
- DateTimeWithOffset exposes the date value and offset independently.

### Review status

- The spatial-and-attribute combined-query work exists only on `codex/spatial-attribute-query`; it has not entered `main` or a released version.
- Three static code-review rounds and a documentation consistency audit are complete.
- The feature is **Code review ready / Formal acceptance blocked** pending GDAL ON/OFF Release builds, full and parallel CTest, package consumer validation, 100K/10M performance matrices and the 5% regression gate.

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
- Missing M values now preserve measured dimensionality and surface as `NaN` instead of consuming descriptor bytes.

### Validation

- Real `testcurve.gdb` acceptance: 2/2 `Curve_Polyline_M_FC` features use `BuiltinCurve`, with zero GDAL fallback.
- FID mapping, WKB type, bbox, length and whole-layer spatial query agree with GDAL within the published tolerance.
- Release and ASan/UBSan/LSan real-data runs report zero failures.

### Known boundaries

- Pure C++ output is linearized standard WKB; native ArcGIS curve objects are not preserved.
- MultiPatch is available only through Hybrid degraded support and does not preserve complete surface semantics.
- Unknown or future FileGDB geometry encodings are not claimed as supported.
- The writer remains experimental and is not part of the v0.1.0 production support statement.