# OpenFileGDB spatial-index capability characterization

## Environment and procedure

- macOS arm64, GDAL/OpenFileGDB 3.13.3.
- Created a temporary FileGDB using OpenFileGDB with one Point layer and ten
  features. The probe and its temporary dataset were removed after recording
  the output. The user-provided `testcurve.gdb` was only read elsewhere in this
  task and was not used for update operations.

## Observed behavior

- After writing and closing the ten spatial features, the driver had created a
  native `.spx` file before any explicit index SQL was issued.
- `CREATE SPATIAL INDEX ON points` returned GDAL error `Invalid layer name:
  points`; it did not create or rebuild the index.
- `DROP SPATIAL INDEX ON points` returned an OGR SQL parser error; the existing
  `.spx` file remained.
- After externally removing the `.spx` file, `REPACK points` did not recreate
  it. Updating a feature with OpenFileGDB `SetFeature()` recreated the native
  index. The new integration regression confirms that a fresh fast-gdb catalog
  then uses the recreated index and returns the same matching FIDs.
- The OpenFileGDB vector driver documentation describes native `.spx` use for
  spatial filtering and lists attribute-index creation, extent recomputation,
  and REPACK special SQL requests. It does not document a spatial-index
  create/drop SQL request: [OpenFileGDB vector driver](https://gdal.org/en/stable/drivers/vector/openfilegdb.html).
- The existing spatial-WHERE integration fixtures exercise the automatically
  generated native index and assert FastGDB index use. The existing Adaptive
  attribute-index delete test reports `SKIPPED` when OpenFileGDB returns
  `Indexes not supported`.

The external-delete/update/reopen regression is
[`test_spatial_where_index_fallback.cpp`](../../../tests/edgar/explorgdb/integration/gdal_parity/spatial_where/test_spatial_where_index_fallback.cpp)
(`OfficialSetFeatureRebuildsMissingSpxForFreshFastReader`). The complete
spatial-index fallback suite passed 9/9 with GDAL 3.13.3.

## GDAL 3.13.0 verification (2026-11-07)

The same lifecycle probe was rerun with the system Python bindings linked to
GDAL 3.13.0. It creates and removes its own temporary FileGDB; no user or
repository fixture is updated. Reproduction command:

```sh
python3 tests/createdata/python/characterize_openfilegdb_spatial_index.py
```

Observed result: closing the ten-feature point layer created
`a00000009.spx`; `CREATE SPATIAL INDEX ON points` returned `Invalid layer
name: points`; `DROP SPATIAL INDEX ON points` returned an OGR SQL parser
error; after deleting the `.spx`, `REPACK points` left it absent; a successful
`SetFeature(FID=1)` recreated it. Reopening the temporary GDB returned ten
features and the updated value `99`. This matches the previously recorded
GDAL 3.13.3 behavior.

## Status

This records the GDAL 3.13.0 and 3.13.3 capability boundary, not a fast-gdb
read failure. Keep the broader capability-matrix item open for other
GDAL/FileGDB versions and ArcGIS-authored index behavior. In both tested
GDAL releases, rebuilding occurred on feature update; `REPACK` alone and
explicit spatial-index SQL did not rebuild the index.
