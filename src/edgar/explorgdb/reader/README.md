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

The embedded public seam is `Reader → Layer → QueryRequest/FeatureCursor`. It
keeps catalog resolution, table paths and index files behind the facade while
still exposing `Layer::metadata()` and `Layer::capabilities()` for advanced
readers. Query requests support field projection, offset/limit, result budgets,
optional FID ordering and cancellation callbacks.
Sequential and WHERE scans stop once the requested result window or budget is
known; index-backed and spatial plans still preserve their existing candidate
diagnostics before applying the public window.

The current public FID contract is zero-based `uint32_t`. Every tablx loading
path rejects a physical slot domain larger than `UINT32_MAX`; it never silently
truncates a v4 64-bit feature count. A future 64-bit FID change must be an
end-to-end API decision rather than a parser-only widening.

`Layer::read_metadata()` is the canonical facade for layer fields, definition,
workspace domains, field-domain bindings, relationship summaries and complete
relationship definitions, dataset grouping and capabilities. It returns a
`MetadataReadResult` with a structured `ReaderError`; a source change is
reported as `SourceChanged` and the snapshot must be discarded. The older
`Layer::metadata_snapshot()` remains as a compatibility convenience and does
not replace structured error handling. `MetadataReader` remains available for
advanced catalog-specific lookups.

Field descriptors retain default-value bytes in `default_value_raw`. The
bytes are deliberately exposed without guessing a scalar representation;
callers can decode them according to the field type and source version.

`Reader`, `Layer` and `QueryEngine` are not shared mutable thread-safe objects.
Independent Reader/Layer object graphs may be used concurrently; one
`QueryEngine` permits at most one active cursor. Cursors borrow that Layer's
open source and must be destroyed before the source is edited or the Layer is
reopened. Any source change invalidates the complete object graph; callers must
close it and build a fresh Reader. `Layer::capabilities()` reports supported or
degraded behavior, while query status/error fields classify execution failures.

fast-gdb does not implement FileGDB editing. There is no Writer dependency from this directory.

ADR-008 defines a higher-level Adaptive Reader orchestration layer for writer-activity observation, source-change validation, Reader invalidation and fresh GDAL read-only recovery. The implementation lives in the optional `fast_gdb::adaptive` target; this low-level directory remains Writer-free.

## Main flow

```text
GdbCatalog
  → CatalogResolver
  → GdbTableParser
  → QueryEngine
  → FeatureCursor
  → FeatureRecord + GeometryValue
```

The optional Adaptive layer remains above this low-level flow:

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

`Reader` stores a snapshot of the catalog's immediate regular files. `Reader::source_is_current()` and `Layer::source_is_current()` provide explicit safe-point checks; opening a layer, starting a cursor or reading by FID after a detected change returns `SourceChanged`. Callers must still discard the old object graph and reopen it. A mutation during an already-running cursor is not made safe by this check.

After GDALClose, build a new Reader object graph beginning with `GdbCatalog::scan()`. Reusing an old parser, engine, cursor or mapping is unsupported.

```text
open fast-gdb Reader + GDAL update same directory = unsupported
```

The overlap may expose old, new, mixed or error states. Tests that characterize this behavior do not establish a support contract.

The implemented Adaptive behavior is fail-closed rather than concurrent-read support:

- `writer_active=true` returns `SourceBusy` without invoking either read backend;
- fast query records `generation_before` and `generation_after`; a generation or
  source-verification change discards the fully materialized result and expires
  this Reader state;
- a query that acquired its fast lease may finish while `WriterPending` is
  observed; it remains a verified old-generation result and records
  `writer_pending_seen`;
- GDAL recovery is allowed only after quiescence and must use a fresh read-only Dataset;
- uncoordinated external Writer detection is best-effort only.

## Build boundary

Current Reader targets are `fast_gdb::linear`, optional `fast_gdb::hybrid`, and optional `fast_gdb::adaptive`. Adaptive is disabled by default, requires GDAL, must not link `src/edgar/usegdal`, and is currently verified only for its same-process coordination, Busy, expiry and fresh-fallback contracts.

## Related documents

- `docs/technical/06_Reader读取流程专题.md`
- `docs/testing/03_GDAL边界与读写测试.md`
- `docs/adr/ADR-007-reader-only-gdal-edit-boundary.md`
- `docs/adr/ADR-008-adaptive-reader-write-detection-gdal-fallback.md`
- `docs/planning/22_AdaptiveReader写入检测与GDAL回退计划.md`
