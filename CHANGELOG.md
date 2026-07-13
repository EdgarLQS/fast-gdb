# Changelog

All notable changes to fast-gdb are documented in this file.

## [0.1.0] - 2026-07-13

### Added

- C++17 FileGDB reader products: dependency-free `fast_gdb_linear` and optional GDAL-backed `fast_gdb_hybrid`.
- Unified `GeometryModel` / `GeometryValue` pipeline with ISO WKB-first output.
- Point, MultiPoint, Polyline and Polygon support, including multipart geometries, holes, islands, Z, M and ZM dimensions.
- Built-in linearization for CircularArc, CubicBezier and EllipticArc descriptors.
- Exact spatial predicates after `.spx` candidate filtering.
- Explicit Hybrid FID offset mapping and diagnostic GDAL fallback.
- Cross-platform geometry CI for Windows, Linux and macOS, plus Linux Hybrid and ASan/UBSan/LSan gates.
- CMake install targets, package configuration files and CPack archives.

### Fixed

- ArcGIS FileGDB M-enabled 2D curves that encode an omitted M array with the single-byte `0x42` marker.
- Transactional curve-tail parsing now preserves canonical M arrays, represents missing M values as `NaN`, and fails closed for damaged descriptors.

### Verified

- Real `testcurve.gdb` acceptance: `Curve_Polyline_M_FC` passed 2/2 features using `BuiltinCurve`, with zero GDAL fallback.
- FID mapping, WKB type, bounding boxes, lengths and full-layer spatial query matched the GDAL reference within documented tolerances.
- The same real-data assertions passed Release and ASan/UBSan/LSan builds with zero failures.

### Known limitations

- Built-in curve output is linearized ISO WKB; native ArcGIS curve object semantics are not preserved.
- MultiPatch is available only as Hybrid degraded support and does not preserve the complete surface model, materials, textures or normals.
- Unknown or future FileGDB geometry encodings are not claimed as supported.
- The writer remains experimental; complete production-grade system-table synchronization is outside the v0.1.0 scope.
