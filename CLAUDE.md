# fast-gdb Development Guide

## Product scope

fast-gdb is a FileGDB **Reader-only** project.

Allowed product work:

- catalog and system-table parsing;
- table/tablx/spx/atx reading;
- FID, attribute and spatial queries;
- geometry decoding and ISO WKB output;
- FeatureCursor and QueryEngine;
- Reader performance and compatibility;
- optional GDAL Hybrid read fallback;
- GDAL-generated fixtures and Reader parity tests;
- Reader-only orchestration that detects source changes, rejects reads while a coordinated Writer is active, expires stale Reader state and uses a fresh GDAL read-only fallback after the source is stable, subject to ADR-008.

Not part of the supported product:

- FileGDB creation or editing APIs;
- `.gdbtable/.gdbtablx/.spx/.atx` writing;
- Append/Update/Delete/Schema Writer APIs;
- Writer transactions or recovery;
- generation publication or VersionedGdbStore;
- installed Writer headers or CMake targets;
- any claim that arbitrary uncoordinated external Writers can always be detected.

## Reference-only `usegdal`

`src/reference/usegdal` is intentionally retained as historical GDAL/OGR wrapper and design-reference code. It may contain datasource, query, transaction and batch-write examples, but it is:

- not built by the root CMake project;
- not installed or exported;
- not part of package consumers or release gates;
- not covered by API/ABI, correctness or production-support promises.

Changes inside that directory are acceptable only when they remain isolated from Reader targets. Promoting any part of it into a supported component requires a new ADR and independent target/test/compatibility policy.

## External edit boundary

The only currently supported sequence is:

```text
close all fast-gdb Reader objects
→ edit exclusively with GDAL/OpenFileGDB
→ close all GDAL objects
→ rebuild every fast-gdb Reader object
```

Never keep `GdbCatalog`, `GdbTableParser`, `QueryEngine`, `FeatureCursor`, mmap, fd or HANDLE alive while GDAL updates the same `.gdb` directory.

Same-directory overlap is unsupported and may produce old/new/mixed/error. Characterization tests must never be described as support evidence.

ADR-008 is Accepted for the optional Adaptive target and does not change the
Reader-only external-edit contract. Its implemented behavior is fail-closed:

- coordinated `writer_active=true` returns `SourceBusy` without calling either backend;
- a source or generation change discards the result and expires the old Reader graph;
- GDAL recovery occurs only after the source is quiescent and uses a fresh read-only Dataset;
- uncoordinated detection must always be labeled best-effort.

## Build targets

Current targets:

- `fast_gdb_linear`
- `fast_gdb_hybrid`
- `fast_gdb_geometry_test_runner`
- `fast_gdb_hybrid_test_runner`
- `fast_gdb_adaptive`
- `fast_gdb_adaptive_reader_test_runner`
- `gdb_tutorial_test_runner`
- `fast_gdb_gdal_read_write_boundary_test_runner`

There is no Writer target and no `usegdal` target. The optional
`fast_gdb::adaptive` target is implemented when
`FAST_GDB_BUILD_ADAPTIVE_READER=ON`; cross-platform and multi-GDAL evidence
remain separate release gates.

## Review rules

Reject or require an explicit accepted ADR for changes that:

- add FileGDB write code to supported Reader/product targets;
- export a Writer API;
- reintroduce `include/fast_gdb/writer` or `src/edgar/explorgdb/writer`;
- link Reader targets to `src/reference/usegdal`;
- install or export `usegdal` reference types;
- reuse Reader state after an external edit;
- claim concurrent GDAL write / fast-gdb read support;
- treat absence of a `.lock` file or unchanged mtime alone as proof that writing ended;
- reuse the thread-local curve-fallback GDAL Dataset for Adaptive recovery;
- return a result after source/generation change instead of discarding it;
- remove final candidate rechecks from `.spx/.atx` query paths.

## Validation

For Reader changes, prefer:

1. focused unit tests;
2. GDAL parity;
3. real FileGDB fixtures;
4. linear and hybrid builds;
5. installed package consumer;
6. boundary tests when external GDAL edits are involved;
7. coordinated and uncoordinated Adaptive Reader tests, with uncoordinated
   overlap remaining characterization only.

Reference-only `usegdal` source is not considered release-validated unless a future standalone component explicitly adds its own gates.

A GitHub Actions job that terminates before steps execute is infrastructure failure, not test evidence.
