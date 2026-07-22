# Changelog

All notable changes to fast-gdb are documented in this file.

## Unreleased

### Added

- `VersionedGdbStore` as the sole public Writer API.
- Immutable FileGDB generations selected through an atomic single-line `CURRENT` manifest.
- Move-only `GdbReaderSnapshot` leases that pin existing readers to their generation until explicit refresh or destruction.
- Move-only `GdbWriteTransaction` with a process-local single-writer gate and private `working_path()`.
- macOS `clonefile` and Linux `FICLONE` working-copy acceleration with full-copy fallback.
- Explicit `GdbPublishState`: `NotPublished`, `PublishedDurable`, and `PublishedDurabilityUncertain`.
- Recovery barriers that preserve both old and new generations when CURRENT switched but final directory durability is uncertain.
- Mandatory reopen validator for directory magic, catalog/layer resolution, record counts, full scans, FIDs, WKB-first geometry, `.spx`, and `.atx`.
- Path canonicalization and Windows case folding for process-local writer-gate aliases.
- Managed-tree rejection of symbolic links and unsupported special files.
- Independent VersionedGdbStore test target in both GDAL ON and OFF builds.
- Installed package consumer coverage for the new managed Writer API.
- Detailed usage, ADR-007, lifecycle, limitations, roadmap, and three-round self-review documentation.
- @深度研究 architecture review documentation split into architecture conclusions, GDAL/OpenFileGDB comparison, implementation risks, unsupported/fail-fast policy, production recovery runbook, and cross-platform acceptance matrix.
- Branch-only `QueryKind::SpatialWhere` entry that combines exact bbox filtering with the existing WHERE subset.
- `CombinedQueryMetrics` diagnostics for candidates, rechecks, final matches and stage timings.
- Detailed attribute-planning metrics for `.gdbindexes` metadata, `.atx` file load, tree navigation, leaf scan, candidate ordering, final WHERE recheck, page counts and scanned entries.
- Fused selective SpatialWhere metrics for raw candidates, one-pass candidate scan time and path confirmation.
- Internal WHERE tokenizer/parser/evaluator shared by standalone and combined paths.
- Field-to-index resolution through `.gdbindexes`, including functional-index classification.
- Sparse candidate-field scanning for mmap and fd paths.
- Public `QueryFeature` and move-only `FeatureCursor` full-feature streaming API.
- `QueryEngine::open_cursor()` for every existing QueryKind.
- True SequentialScan streaming mode that does not materialize the whole FID set.
- `FeatureCursor::move_to(fid)` for forward, backward, rewind and arbitrary zero-based FID jumps.
- Cursor EOF/error diagnostics through `done()`, `query_result()` and `error()`.
- Cursor and engine generations for single-active-cursor ownership and reopen invalidation.
- One-pass `GdbTableParser::read_feature_by_fid()` that locates a row once, materializes fields once, decodes one `GeometryModel`, and writes ISO WKB without eager WKT.
- Pure C++ `GeometryValue::to_wkt()` for explicit on-demand WKB conversion, including EMPTY members in Multi geometries.
- Request-scoped `profile_feature_reads` and `FeatureCursorMetrics` for row lookup, field materialization, geometry decode and WKB stages.
- Direct `.atx` query APIs that validate the complete leaf chain but materialize only matching FIDs instead of all index entries.
- Snapshot-scoped `.gdbindexes` metadata cache on `GdbCatalog` without removing catalog copy/move value semantics.
- Full-object GDAL tests for fields, NULL, Binary and ISO WKB.
- WKB-first parity tests for Point, MultiPoint, Polyline and Polygon with a hole.
- Adaptive SpatialWhere tests for low-coverage `.atx` bypass and high-coverage direct-index execution.
- Fused SpatialWhere GDAL parity tests for MultiPoint, Polyline and Polygon.
- Exact Point ZM WKT/WKB writer contract and `.spx` merged-range candidate-superset tests.
- Tests for deleted slots, no-geometry tables, NULL geometry and ObjectID-only zero-length rows.
- Opt-in 100K full-feature cursor/legacy/GDAL checksum benchmark.
- Draft-PR strict performance workflow requiring correct fused execution and `cursor_median_ms < gdal_median_ms`.
- GDAL-OFF cursor public API and ownership contract tests.

### Changed

- `fast_gdb::writer` now exposes only `include/fast_gdb/writer/versioned_gdb_store.h` and `versioned_gdb_validator.h`.
- Writer publication is a complete immutable-generation commit; callers edit only `GdbWriteTransaction::working_path()`.
- Publication failure must be interpreted together with `publish_state()` because CURRENT may already have switched.
- Reader access must originate from `GdbReaderSnapshot::path()` for lease and garbage-collection safety.
- The package description and Writer documentation now describe a managed store rather than staged direct publication.
- Writer documentation now explicitly defines VersionedGdbStore as a versioned-publication protocol rather than a transparent replacement for all ArcGIS/GDAL FileGDB behavior.
- Cross-process Reader/Writer access, NFS/SMB/FUSE/cloud-sync/object-storage deployment, direct generation access, and manual CURRENT edits are explicitly unsupported.
- Validator success is now documented as proof of configured repository invariants only; relationships, domains, hierarchy, native curves, MultiPatch, raster, and sparse 64-bit ObjectID require separate compatibility profiles.
- GDAL/OpenFileGDB integration documentation now requires complete Dataset/Layer/Feature/SQL-result/handle shutdown before VersionedGdbStore publication.
- CoW documentation now distinguishes initial clone efficiency from capacity reservation and covers ENOSPC after clone, long-lived Reader pinning, and GC-driven disk growth.
- `.atx` parsing fails closed on invalid sizes, page bounds/capacity, cyclic leaf chains, count mismatches and zero FIDs.
- SpatialWhere bypasses `.atx` data-page reads when exact spatial matches are no more than 65,536 and no more than 12.5% of active features; the full WHERE is evaluated directly on those candidates.
- Selective SpatialWhere parses each `.spx` candidate row once and performs exact geometry plus WHERE evaluation from the same `FieldRef` array; incomplete scans fall back transactionally.
- Full-height selective queries may merge per-X `.spx` navigation into one raw-key range per grid level while retaining exact geometry filtering.
- Dense candidate bitsets are enumerated directly in FID order instead of sorting a second vector.
- High-coverage SpatialWhere queries use the direct `.atx` leaf path rather than constructing `all_entries_`.
- Numeric direct-index comparisons operate on leaf-page bytes without a temporary `AttributeIndexEntry` per row.
- Attribute candidates are sorted/deduplicated and always receive a complete WHERE recheck.
- Non-BMP strings, `!=`, functional indexes and ambiguous numeric encodings fall back safely.
- Candidate field scans reuse `FieldRef` scratch buffers and skip redundant physical sorting for monotonic mmap offsets.
- `CatalogResolver::resolve()` carries the already-known `GDB_SpatialRefs` capability so fresh `QueryEngine::open()` does not reread the system catalog; manual legacy `ResolvedTable` construction retains the old fallback.
- WKB output reserves the estimated final byte size, and WKT output writes directly into a reserved string stream buffer while preserving the existing iostream format.
- GDAL integration tests use per-test temporary FileGDB directories for parallel CTest.
- Active FeatureCursor instances block QueryEngine read entry points.
- QueryEngine is non-copyable, move-constructible, and not move-assignable. Cursor state uses stable heap-owned control and table objects across a move; the moved-from engine remains safely unavailable.
- Reopening QueryEngine invalidates exhausted cursors and resets cached spatial-index state.
- Zero-length legal rows are normalized into ObjectID and nullable NULL values.
- FeatureCursor now uses the one-pass full-object reader instead of `read_record_by_fid()` followed by `read_geometry_value()`.
- Full-feature benchmark schema v3 rotates Cursor/record+geometry/GDAL order across five samples, keeps profile outside median/p95, records WKB-first stages and measures explicit `to_wkt()` separately.
- FID-only benchmark records the adaptive execution path and detailed attribute-planning stages; default evidence is written outside the repository.
- Feature profiling is bound to each `QueryRequest`; normal reads do not call the profile clock or depend on process-global state.
- Combined-query and full-feature evidence records observed paths and computed correctness rather than hard-coded claims.
- DateTimeWithOffset exposes the date and offset independently.

### Removed

- `fast_gdb::writer_legacy`.
- The compatibility option `FAST_GDB_INSTALL_LEGACY_WRITER_API`.
- Installed `writer_session.h`, `writer_recovery.h`, `writer_index.h`, `writer_append.h`, `writer_update.h`, `writer_delete.h`, and `writer_transaction.h`.
- Legacy/direct Writer package-consumer modes and migration guarantees.
- Duplicate public VersionedGdbStore headers from the implementation source directory.
- User documentation for the old direct-publication Writer APIs.

### Review status

- VersionedGdbStore completed three rounds of concurrency/lifetime, crash-consistency, and API/platform/test self-review.
- A separate @深度研究 review compared the current architecture with GDAL/OpenFileGDB and added explicit risk, unsupported-scenario, runbook, and acceptance documents under `docs/review/`.
- Linux local validation passed strict C++17 syntax checks, single- and multi-thread smoke tests, path-alias tests, ASan and UBSan.
- Formal acceptance remains blocked pending complete CMake/CTest, macOS/Linux/Windows runtime matrices, ENOSPC/crash fault injection, real FileGDB validator evidence, cross-process misuse rejection, and usable GitHub Actions logs/artifacts.
- Combined-query, FeatureCursor, one-pass and performance optimization work exists only on `codex/spatial-attribute-query`; it has not entered `main` or a release.

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
- The writer is not part of the v0.1.0 production support statement.
