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

`QueryRequest::include_geometry` defaults to `true`. Set it to `false` when the
caller needs attributes or filtered features but not output geometry. Returned
geometry is marked `GeometryStatus::NotRequested`; the row blob is bounds
checked and skipped without constructing a geometry model or writing WKB.
Geometry needed to evaluate a spatial predicate is still decoded by the query
planner. `field_projection` selects materialized attribute slots; fields needed
to evaluate a filter remain available to the planner.

Example using the Unified API:

```cpp
fast_gdb::unified::Query query;
query.projected_fields = {"name"};
query.attribute_filter = "road_id = 1";
query.include_geometry = false;

auto cursor = layer.open_cursor(query);
// Returned features contain the requested output fields and report
// GeometryState::NotRequested; filter inputs are still used during planning.
```

`QueryRequest::geometry_read_options` defaults to `PreserveSource`. When the
native WKB model cannot represent a curve or another geometry type, the cursor
returns the attributes, an explicit unsupported geometry status and the exact
FileGDB geometry blob, subject to `max_source_payload_bytes` (64 MiB by
default). A payload over the limit is reported as `LimitExceeded` without a
copy. Set `CurveReadPolicy::Linearize` to request built-in curve linearization;
the returned geometry records that it was linearized and the requested
parameters. `Reject` keeps the legacy fail-on-unsupported-geometry behavior.
For OGR curve WKB, choose the Unified GDAL-only backend explicitly.

MultiPatch retains the bounded source payload and also exposes an immutable
`multipatch` model through Reader and Unified. It contains decoded coordinates,
raw and normalized part types, triangle index access for strips/fans/triangle
groups, and ring groups in their source encoding. Complete known structures
emit ISO WKB: triangle parts become TIN members, connected OuterRing triangles
follow GDAL's TIN heuristic, other ring groups become a MultiPolygon, and mixed
parts become a GeometryCollection. Z/M ordinates are preserved. Unknown part
codes, opaque trailing bytes, and malformed structure
remain unsupported or invalid and do not emit normalized WKB. `CapabilityReport`
keeps MultiPatch degraded until real ArcGIS parity is checked. For complete
known structures, the legacy WKT field uses the same normalized mapping as
ISO WKB; unknown parts, opaque tails, and invalid topology fail closed. This
WKT is not a lossless representation of ArcGIS part labels, so use the
structured model and raw payload when those distinctions matter. A real ArcGIS
MultiPatch sample is still required to verify binary layout and compare surface
mapping with GDAL. See the [GDAL comparison matrix](../../../../docs/gdal/01_GDAL功能对比矩阵.md).

The current public FID contract is zero-based `uint32_t`. Every tablx loading
path rejects a physical slot domain larger than `UINT32_MAX`; it never silently
truncates a v4 64-bit feature count. A future 64-bit FID change must be an
end-to-end API decision rather than a parser-only widening.

`Reader::read_workspace_metadata()` reads workspace metadata without opening a
layer. Its `catalog_objects` inventory distinguishes complete, partial, empty
and unavailable reads and carries row-level integrity issues. Use
`Reader::read_catalog_object_by_uuid()` to retrieve one object's typed
attributes and exact source `Definition`; duplicate UUIDs are reported as
ambiguous, and a partial inventory cannot claim a missing UUID. The Unified
API exposes the same summaries in `Dataset::metadata()` and the detailed read
in `Dataset::catalog_object_by_uuid()`. The GDAL-only backend reports this
system-catalog capability as unsupported.

Workspace and layer metadata also include attachment relationships marked by
`IsAttachmentRelationship`, with endpoint UUIDs/paths, keys, the original
relationship XML, and whether the attachment table resolves in the catalog.
Names ending in `__ATTACH` identify the attachment table; other endpoint layouts
are retained with an explicit inferred flag. `Layer::read_binary_field_by_fid()`
reads one Binary field under a caller byte limit, distinguishes NULL from an
empty payload, and skips an oversized value without copying its bytes. The
Unified API exposes the same primitive as `Layer::read_binary_field_by_fid()`;
GDAL-only reports bounded Binary reads and detailed attachment definitions as
unsupported. The checked-in ArcGIS sample has no attachment table, so real
attachment relationship and payload conformance remains pending.

Catalog summaries resolve item type names and read dataset/geometry type
identifiers from Definition XML. Annotation, Dimension, Topology, Geometric and
Utility Network, Parcel Fabric, Terrain/TIN and Mosaic Dataset rows carry an
explicit semantic status. `RawDefinitionOnly` means the original catalog row
and Definition remain retrievable by UUID; it does not claim specialized
ArcGIS behavior. `Unsupported` means the Definition is absent or the semantic
reader is unavailable; business semantics remain unsupported for both states.
A resolved physical table is reported separately from this semantic status.
The checked-in sample includes these type names in
`GDB_ItemTypes` but no instances, so representative ArcGIS objects remain
necessary for conformance.

`Layer::read_metadata()` returns layer fields and definitions, domains,
field-domain bindings, relationship-class definitions, Feature Dataset grouping,
Subtype overrides and system-table audits. Both revalidate the source snapshot
and report source changes as `SourceChanged`. The older
`Layer::metadata_snapshot()` remains a compatibility convenience.

The unified API adds owning `Dataset::metadata()` and `Layer::metadata()`
snapshots. Each section reports its source and status and raw metadata payloads
remain available to the caller. The GDAL-only adapter marks ArcGIS-specific
Subtype XML and system-table audits as `Unsupported` when its public API cannot
provide them. `MetadataReader` remains available for catalog-specific lookups.

FastGdb parses metadata XML with privately vendored pugixml 1.16. Element lookup
matches QName local names, so prefixed and default-namespace documents use the
same schema path; XML entities are decoded for values while the original source
text remains available in raw payloads. Metadata sections distinguish missing,
empty, malformed and unrecognized structures and include parse diagnostics.

Subtype definitions include code/name/default values and per-field default and
domain overrides when present in the source XML. Missing values are not
inferred; additional ArcGIS versions still need conformance samples.

`LayerMetadataSnapshot::system_table_audit` reports each relevant metadata
system table as `ok`, `empty`, `missing`, or `unreadable`, with row count and a
deterministic digest when readable. This prevents a missing system table from
being confused with a valid empty metadata result.

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

When an external writer has finished, callers must explicitly refresh the
Unified dataset or close and reopen the Reader before expecting the new source
state. `Dataset::refresh()` builds a complete new generation and invalidates
old Dataset/Layer handles. Source snapshots detect observed changes at safe
points; detection of uncoordinated concurrent external writes remains
best-effort, so callers must not read while a writer is active.

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
- Direct `.atx` queries descend to relevant pages and use a bounded per-query
  16-page LRU; complete predicates still decide final matches;
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

Current Reader targets are `fast_gdb::linear`, optional `fast_gdb::hybrid`, and optional `fast_gdb::adaptive`. Adaptive is disabled by default, requires GDAL, must not link `src/reference/usegdal`, and is currently verified only for its same-process coordination, Busy, expiry and fresh-fallback contracts.

## Related documents

- `docs/gdb/06_Reader读取流程专题.md`
- `docs/quality/03_GDAL边界与读写测试.md`
- `docs/governance/adr/ADR-007-reader-only-gdal-edit-boundary.md`
- `docs/governance/adr/ADR-008-adaptive-reader-write-detection-gdal-fallback.md`
- `docs/gdal/02_AdaptiveReader写入检测与GDAL回退计划.md`
