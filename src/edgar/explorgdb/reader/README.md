# explorgdb Reader

This directory contains the fast-gdb FileGDB Reader implementation.

## Scope

- catalog and system-table discovery;
- `.gdbtable` and `.gdbtablx` parsing;
- FID lookup and sequential scan;
- `.spx` spatial candidates;
- `.atx` attribute candidates;
- WHERE, spatial and combined query planning;
- FeatureCursor;
- geometry decoding and ISO WKB output.

fast-gdb does not implement FileGDB editing. There is no Writer dependency from this directory.

ADR-008 proposes a future higher-level Adaptive Reader orchestration layer for writer-activity observation, source-change validation, Reader invalidation and fresh GDAL read-only recovery. That layer is not implemented here yet and must not add FileGDB write behavior to this directory.

## Main flow

```text
GdbCatalog
  → CatalogResolver
  → GdbTableParser
  → QueryEngine
  → FeatureCursor
  → FeatureRecord + GeometryValue
```

The proposed Adaptive layer remains above this low-level flow:

```text
activity/generation + source snapshot
  → materialized low-level Reader result
  → source validation
  → return / expire / SourceBusy / fresh GDAL read-only recovery
```

## Correctness rules

- `.spx` and `.atx` return candidates only;
- final WHERE and geometry checks are mandatory;
- malformed metadata/pages fail closed or fall back;
- FID density and physical row-offset stability are not assumed;
- WKB is the formal geometry output; WKT is on demand;
- source or generation changes invalidate the complete Reader object graph;
- a result requiring post-read validation must be fully materialized before publication.

## Lifetime rules

Reader objects may retain mmap regions, file descriptors, table schema, FID offsets and index pages. Before GDAL/OpenFileGDB updates the same `.gdb` directory, the current supported contract requires destroying every Reader object and closing every mapping/handle.

After GDALClose, build a new Reader object graph beginning with `GdbCatalog::scan()`. Reusing an old parser, engine, cursor or mapping is unsupported.

```text
open fast-gdb Reader + GDAL update same directory = unsupported
```

The overlap may expose old, new, mixed or error states. Tests that characterize this behavior do not establish a support contract.

The proposed Adaptive behavior is fail-closed rather than concurrent-read support:

- `writer_active=true` returns `SourceBusy` without invoking either read backend;
- source/generation change discards the result and expires this Reader state;
- GDAL recovery is allowed only after quiescence and must use a fresh read-only Dataset;
- uncoordinated external Writer detection is best-effort only.

## Build boundary

Current Reader targets are `fast_gdb::linear` and optional `fast_gdb::hybrid`. There is no Adaptive target yet. Any future `fast_gdb::adaptive` target must remain Reader-only, must not link `src/edgar/usegdal`, and requires ADR-008 acceptance plus independent correctness, stress, performance and install gates.

## Related documents

- `docs/technical/06_Reader读取流程专题.md`
- `docs/usage/11_GDAL写入与fast-gdb读取边界.md`
- `docs/adr/ADR-007-reader-only-gdal-edit-boundary.md`
- `docs/adr/ADR-008-adaptive-reader-write-detection-gdal-fallback.md`
- `docs/planning/22_AdaptiveReader写入检测与GDAL回退计划.md`
