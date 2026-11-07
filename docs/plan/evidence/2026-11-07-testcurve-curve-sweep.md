# Complete testcurve curve sweep

## Scope and fixture

- Read `/Users/edgarlqs/Downloads/testcurve.gdb` directly; the external source was not copied into or modified in the repository.
- Compared ten curve or curved-surface layers containing 27 rows: circular arcs, Bezier and mixed/multipart curves, Z and M polylines, curved polygons with holes/islands, circles, ellipses, rotated ellipses and ellipse arcs.
- The FastGDB integration test uses the Release build linked to GDAL 3.13.0. A separate read-only `ogrinfo` inventory used GDAL 3.13.3. The sample itself is FileGDB version 3; its creator and creation procedure are unknown.

## Results

- Every row's FastGDB `ReadByFid` preserve-source path returned `UnsupportedCurve`, marked the source as a curve, and retained a byte-for-byte copy of the geometry payload (27/27).
- The default built-in linearizer produced valid WKB for 16/27 rows. Their flattened WKB type, Z/M flags and envelope matched GDAL's linearized geometry within `max(1e-3, max(envelope width, height) * 1e-3)`.
- The remaining 11 rows fail closed without WKB: 9 `TouchingRings`, 1 `InvalidEncoding` (`bezier exceeds maximum segment count`), and 1 `SelfIntersection`. Their raw source payload remains available through the preserve-source query path. These statuses are characterization of this fixture, not proof that ArcGIS intended the same topology classification.
- For the two `Curve_Polyline_M_FC` rows, FastGDB retains the M flag in its model and WKB, while GDAL 3.13.0's per-feature geometry is unmeasured. FastGDB's decoded M ordinates are NaN for this sample representation. The separate M-curve assertion compares geometry type, envelope, length and full-layer query results; it does not claim M-ordinate parity.

## Verification

```sh
cmake --build build-s4-release --target gdb_tutorial_test_runner --parallel 4
FAST_GDB_CURVE_DATASET=/Users/edgarlqs/Downloads/testcurve.gdb \
FAST_GDB_REAL_DATASET=test_data/gdb/acceptance_metadata.gdb \
  ./build-s4-release/bin/gdb_tutorial_test_runner \
  --gtest_filter='RealDataReleaseContractTest.CurvePolylineMMatchesGdalWithoutHybridFallback:RealDataReleaseContractTest.SuppliedCurvesPreserveSourceAndCompareLinearizationsToGdal:RealDataReleaseContractTest.Sparse32BitFidsMatchGdalFeatureRows:RealDataReleaseContractTest.RealBadPolygonTopologyIsReportedInsteadOfSilentlyRepaired:RealDataReleaseContractTest.SpatialReferenceNamesAndEllipsoidsMatchGdalForSuppliedCrsLayers' \
  --gtest_brief=1
```

Result: **5/5 tests passed**. The run also retained the existing sparse-FID, selected bad-topology, and CRS checks.

The full Release CTest suite was then run with absolute paths for both real-data
fixtures. Result: **526 discovered, 515 passed, 11 skipped, 0 failed**. The
skips were configured AWS/curve-backend and opt-in benchmark cases; the new
curve sweep, million-point scan and all configured real-data contracts passed.

```sh
FAST_GDB_CURVE_DATASET=/Users/edgarlqs/Downloads/testcurve.gdb \
FAST_GDB_REAL_DATASET=/Users/edgarlqs/Downloads/fast-gdb/test_data/gdb/acceptance_metadata.gdb \
  ctest --test-dir build-s4-release --output-on-failure -j4
```

## Remaining boundary

This sweep does not make GDAL output an ArcGIS semantic oracle. The external sample has no provenance or independent expected-value manifest, is version 3, and contains no nonempty MultiPatch, attachments, or advanced ArcGIS object instances. Exact M ordinates, complete topology intent, per-type native curve structure, v4/high-bit ObjectIDs, and Windows/Linux behavior remain open.
