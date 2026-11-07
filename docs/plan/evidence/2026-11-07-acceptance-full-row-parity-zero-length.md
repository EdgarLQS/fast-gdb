# Full-row GDAL parity and zero-length row semantics

## Real data comparisons

- The repository ArcGIS `acceptance_metadata.gdb` is compared layer by layer
  against GDAL 3.13.0 OpenFileGDB. The test checks layer/schema types, active
  FID order, all attribute values, and normalized type/dimension/coordinates
  for every non-empty geometry. Latest recorded counts are 8 layers, 56 rows,
  460 attribute values, 42 non-empty geometries, 3 empty geometries, and 0
  raw zero-length rows, with no mismatches.
- The user-provided `/Users/edgarlqs/Downloads/testcurve.gdb` is a separate
  real-data corpus. Its full-row test covers 44 layers, 1,120,080 records,
  1,150,884 scalar field values, and 1,120,046 non-curve non-empty geometries;
  the 27 curve records use dedicated curve contracts. The directory has 53
  `.gdbtable` files paired with 53 `.gdbtablx` files, including
  `a00000004.gdbtable`. GDAL opened it read-only. The source directory was not
  modified.
- The current code was rechecked against this supplied path with the real-data
  contract suite: 17 tests, 16 passed and 1 REJECT-backend-only curve test
  skipped by design.

## Zero-length row contracts

Synthetic tests cover direct record reads, one-pass feature reads, Binary NULL,
geometry read and peek, sequential scans, and candidate scans. They require
consistent implicit ObjectID, nullable NULL values, and empty geometry state.
When a zero-length row omits a required attribute, every read path fails closed.

No available real ArcGIS sample contains a proven raw zero-length row: the
`acceptance_metadata.gdb` all-row check found zero such rows, and the supplied
curve corpus is not a substitute for a representative zero-length record.
Real-data comparison for that case remains open.

## Verification

- Focused command:

  ```sh
  FAST_GDB_REAL_DATASET=/Users/edgarlqs/Downloads/fast-gdb/test_data/gdb/acceptance_metadata.gdb \
  FAST_GDB_CURVE_DATASET=/Users/edgarlqs/Downloads/testcurve.gdb \
  build-s4-release/bin/gdb_tutorial_test_runner \
    --gtest_filter='RealDataReleaseContractTest.*' --gtest_brief=1
  ```

- Latest full GDAL 3.13.0 Release CTest run after adding the Unified sparse-FID
  contract: 541 discovered, 527 passed, 14 configured skips, 0 failures,
  61.36 seconds. The skipped tests include
  platform/backend-specific and opt-in benchmark cases and are not counted as
  passes.
