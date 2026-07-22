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

## Main flow

```text
GdbCatalog
  → CatalogResolver
  → GdbTableParser
  → QueryEngine
  → FeatureCursor
  → FeatureRecord + GeometryValue
```

## Correctness rules

- `.spx` and `.atx` return candidates only;
- final WHERE and geometry checks are mandatory;
- malformed metadata/pages fail closed or fall back;
- FID density and physical row-offset stability are not assumed;
- WKB is the formal geometry output; WKT is on demand.

## Lifetime rules

Reader objects may retain mmap regions, file descriptors, table schema, FID offsets and index pages. Before GDAL/OpenFileGDB updates the same `.gdb` directory, destroy every Reader object and close every mapping/handle.

After GDALClose, build a new Reader object graph beginning with `GdbCatalog::scan()`. Reusing an old parser, engine, cursor or mapping is unsupported.

```text
open fast-gdb Reader + GDAL update same directory = unsupported
```

The overlap may expose old, new, mixed or error states. Tests that characterize this behavior do not establish a support contract.

## Related documents

- `docs/technical/06_Reader读取流程专题.md`
- `docs/usage/11_GDAL写入与fast-gdb读取边界.md`
- `docs/adr/ADR-007-reader-only-gdal-edit-boundary.md`
