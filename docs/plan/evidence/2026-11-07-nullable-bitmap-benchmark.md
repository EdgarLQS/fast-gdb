# Nullable bitmap schema growth and benchmark

## Environment

- macOS arm64, Release build, GDAL/OpenFileGDB 3.13.3.
- User-provided read-only sample: `/Users/edgarlqs/Downloads/testcurve.gdb`.
- The sample has 53 `.gdbtable` files and 53 `.gdbtablx` files. GDAL opens it;
  `Point_FC` contains three features and nine business fields. The source
  directory was not modified.
- A read-only header scan of all 53 tables found at most 15 nullable fields
  (`a00000004.gdbtable`); the sample has no 33-field nullable table.

## Schema-growth behavior

Two temporary GDAL/OpenFileGDB datasets were used; neither is kept in the
repository.

1. A temporary copy of `Point_FC` was expanded to 33 nullable fields. GDAL
   reopened all three features, retained existing values such as `Origin`, and
   returned NULL for added fields. fast-gdb `dump-records` read the same three
   rows and reported the added fields as NULL.
2. A focused writer probe created seven nullable Int32 fields and three rows,
   closed the dataset, then added 26 nullable Int32 fields. After reopening,
   GDAL returned all seven original values and NULL for the new fields. The
   first row payload uses the expanded five-byte bitmap (`80 ff ff ff ff`)
   followed by the original values. fast-gdb decoded the same values and NULLs.

The repeatable GDAL-on regression is
[`test_nullable_schema_evolution.cpp`](../../../tests/edgar/explorgdb/integration/gdal_parity/feature_cursor/test_nullable_schema_evolution.cpp).
It validates GDAL reads, direct record reads, one-pass reads, sequential scan,
and candidate scan after adding fields. The GDAL-free wide regression uses the
same current-width bitmap representation in
[`test_nullable_bitmap_compat.cpp`](../../../tests/edgar/explorgdb/reader/format/test_nullable_bitmap_compat.cpp).

An earlier hand-built row kept a one-byte bitmap while claiming a 33-field
current schema. Its bytes admit multiple parses, and the existing heuristic can
return shifted values. GDAL 3.13.3 did not produce that layout when fields were
added, so it is not retained as a GDAL regression. Whether ArcGIS or historical
writers produce it remains unresolved and is the reason the compatibility item
stays open.

This establishes the behavior of GDAL 3.13.3 field addition. It does not
establish whether ArcGIS writes the same layout or whether historical files
retain a shorter bitmap after schema growth.

## Opt-in large-row benchmark

The benchmark uses 100,000 in-memory rows with seven non-NULL Int32 values and
the rest NULL, encoded using the current bitmap width with unused trailing bits
set as OpenFileGDB does. It measures direct row materialization, sequential
scan, and candidate scan for 33 and 129 nullable fields. Each reported value is
the median of five measured rounds after a warm up. Three independent benchmark
invocations produced:

| Nullable fields | Path | Batch 1 median (ms) | Batch 2 median (ms) | Batch 3 median (ms) |
|---:|---|---:|---:|---:|
| 33 | Direct read | 17.341 | 17.423 | 17.632 |
| 33 | Sequential scan | 9.888 | 10.002 | 10.144 |
| 33 | Candidate scan | 5.398 | 5.300 | 5.464 |
| 129 | Direct read | 49.173 | 48.889 | 46.468 |
| 129 | Sequential scan | 33.479 | 33.488 | 33.454 |
| 129 | Candidate scan | 17.924 | 17.853 | 17.936 |

Reproduce with:

```bash
cmake --build /private/tmp/fast-gdb-gdal-3.13.3-build \
  --target gdb_tutorial_test_runner --parallel 4
FAST_GDB_RUN_NULLABLE_BITMAP_BENCHMARK=1 \
  /private/tmp/fast-gdb-gdal-3.13.3-build/bin/gdb_tutorial_test_runner \
  '--gtest_filter=NullableBitmapCompatTest.LargeCurrentBitmapReadBenchmark' \
  --gtest_brief=1
```

These are warm in-memory parser timings, not cold disk I/O, an end-to-end GDAL
comparison, or a benchmark of historical short-bitmap candidates. They do not
support an asymptotic complexity claim.

## Status

The GDAL 3.13.3 current-width path now has a 100K-row benchmark and a
repeatable schema-growth regression. Keep the checklist item open until an
ArcGIS-authored or historical fixture establishes the shorter-bitmap candidate
layout and its performance. No production bitmap-selection algorithm was
changed based on this evidence.
