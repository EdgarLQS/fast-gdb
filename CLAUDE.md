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
- GDAL-generated fixtures and Reader parity tests.

Not part of the product:

- FileGDB creation or editing APIs;
- `.gdbtable/.gdbtablx/.spx/.atx` writing;
- Append/Update/Delete/Schema Writer code;
- Writer transactions or recovery;
- generation publication or VersionedGdbStore;
- installed Writer headers or CMake targets.

## External edit boundary

The only supported sequence is:

```text
close all fast-gdb Reader objects
→ edit exclusively with GDAL/OpenFileGDB
→ close all GDAL objects
→ rebuild every fast-gdb Reader object
```

Never keep `GdbCatalog`, `GdbTableParser`, `QueryEngine`, `FeatureCursor`, mmap, fd or HANDLE alive while GDAL updates the same `.gdb` directory.

Same-directory overlap is unsupported and may produce old/new/mixed/error. Characterization tests must never be described as support evidence.

## Build targets

- `fast_gdb_linear`
- `fast_gdb_hybrid`
- `fast_gdb_geometry_test_runner`
- `fast_gdb_hybrid_test_runner`
- `gdb_tutorial_test_runner`
- `fast_gdb_gdal_read_write_boundary_test_runner`

There is no Writer target.

## Review rules

Reject or require a new ADR for changes that:

- add FileGDB write code;
- export a Writer API;
- reintroduce removed Writer files;
- reuse Reader state after an external edit;
- claim concurrent GDAL write / fast-gdb read support;
- remove final candidate rechecks from `.spx/.atx` query paths.

## Validation

For Reader changes, prefer:

1. focused unit tests;
2. GDAL parity;
3. real FileGDB fixtures;
4. linear and hybrid builds;
5. installed package consumer;
6. boundary tests when external GDAL edits are involved.

A GitHub Actions job that terminates before steps execute is infrastructure failure, not test evidence.
