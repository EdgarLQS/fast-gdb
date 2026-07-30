# Test Index

文档说明与验收规则见 [`docs/testing/07_测试索引.md`](../docs/testing/07_测试索引.md)。

元数据、Domain、Relationship Class 和 Feature Dataset 的真实数据生成要求见
[`docs/testing/12_元数据完整解析验收数据清单.md`](../docs/testing/12_元数据完整解析验收数据清单.md)。

## Reader release gates

| Target | Scope |
|---|---|
| `fast_gdb_geometry_test_runner` | Portable geometry, topology and WKB contracts |
| `fast_gdb_hybrid_test_runner` | GDAL-backed curve fallback and real-data release contracts |
| `gdb_tutorial_test_runner` | Full Reader, query, index and direct-GDAL parity suite |
| `fast_gdb_gdal_read_write_boundary_test_runner` | GDAL edit / fast-gdb Reader lifecycle boundary |
| `fast_gdb_adaptive_reader_test_runner` | Adaptive coordinator, independent Reader concurrency and GDAL matrix |

## GDAL boundary tests

File:

```text
tests/usegdal/test_gdal_write_fast_reader_visibility.cpp
```

Tests:

- `SupportedQuiescedReaderWorkflowReopensWithNewData`
  - formal supported contract;
  - Reader closes before GDAL update;
  - GDAL closes before Reader reopen;
  - reopened Reader must see new data.

- `SameDirectoryReadWhileGdalWriterIsOpenIsCharacterizationOnly`
  - unsupported overlap characterization;
  - records old/new/mixed/error;
  - no overlap observation is treated as a product guarantee;
  - full close and reopen must still see new data.

Run:

```bash
cmake -S . -B build-boundary \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_BUILD_FULL_TESTS=OFF \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DBUILD_TESTING=ON
cmake --build build-boundary \
  --target fast_gdb_gdal_read_write_boundary_test_runner --parallel
ctest --test-dir build-boundary --output-on-failure \
  -R '^gdal-reader-boundary\.'
```

## Adaptive Reader tests

ADR-008 and planning document 22 define the implemented Adaptive Reader target:

```text
fast_gdb_adaptive_reader_test_runner
adaptive-reader.unit.*
adaptive-reader.coordinated.*
adaptive-reader.gdal-unverified.*
adaptive-reader.parity.*
adaptive-reader.lifecycle.*
adaptive-reader.stress.*
```

Contract tests:

- `StableSourceUsesFastVerified`;
- `WriterPendingStopsNewFastReads`;
- `DeterministicPendingDrainKeepsLeaseUntilMaterialized`;
- `PendingCancellationIsRecordedByFastRead`;
- `FastCursorExpiresAtNextSafePoint`;
- `PendingTimeoutClearsPendingAndRecovers`;
- `UpdatePermitRequiresFastReadersDrained`;
- `UpdateOpenFailureCancelsPending`;
- `AbandonedActiveTokenRemainsFailClosed`;
- `DefaultPolicyReturnsBusyWithoutCallingBackends`;
- `ExplicitPolicyUsesFreshGdalUnverified`;
- `UnverifiedResultIsNeverReportedVerified`;
- `GdalFailureKeepsDiagnosticAndConsistency`;
- `ClosedUpdateNotificationIncrementsGeneration`;
- `OldReaderExpiresAfterClosedUpdateNotification`;
- `PostWriteRebuildReturnsFastVerified`;
- `AllQueryKindsMatchOnStableSource`;
- `RepackNeverOverlapsFastMmap`;
- `MultipleReadersSingleWriterStress`.

The full Reader runner also includes:

- `ReaderConcurrencyTest.IndependentReadersReturnIdenticalFeatureDigests`;
- `CorruptInputConformance.*`.

The coordinated gate distinguishes two contracts:

- stable fast reads and post-write rebuilds are correctness gates and must be
  `Verified`;
- GDAL reads during `WriterPending` or `WriterActive` are routing and ownership
  gates only. A successful result must be `UnverifiedConcurrentRead`, and its
  old/new/mixed value is characterization rather than a correctness assertion.

The Writer may open its update Dataset only after all fast Reader leases are
released. External-process Writer detection is outside the first implementation
scope and cannot be inferred from these planned tests.

The local GDAL reopen matrix additionally covers Create/DeleteFeature,
Create/DeleteField, index creation and extent refresh; index deletion is recorded
as `SKIPPED` when the selected OpenFileGDB driver does not implement that edit.

## Product-surface tests

`tests/package_consumer` supports:

- `linear`;
- `hybrid`;
- `adaptive`.

There is no Writer consumer mode or `usegdal` consumer mode. `adaptive` is an
optional GDAL/threaded package variant; the `linear` consumer is separately
verified with GDAL disabled.

## Reference-only `usegdal`

`src/edgar/usegdal` is retained as historical GDAL/OGR wrapper source. It is not linked into any release-gate target. Its old wrapper-specific tests are not part of the active test suite.

Formal Reader parity and lifecycle tests call the official GDAL API directly so the supported boundary is not coupled to reference wrappers.

## Removed active test scope

The repository no longer builds or maintains as release gates:

- self-developed FileGDB Writer tests;
- Append/Update/Delete/Transaction/Recovery product tests;
- VersionedGdbStore tests;
- Writer performance tests;
- GDAL BatchWriter/Transaction wrapper tests.

The corresponding `usegdal` source may remain for reference, but it is not considered tested or production-supported.

## Result policy

- `PASS` requires executed test steps and assertions;
- `SKIPPED` is not acceptance;
- runner failures without steps/logs are infrastructure failures;
- characterization output is diagnostic and cannot establish concurrent read/write support;
- `UnverifiedConcurrentRead` must never be counted as a verified data-correctness result;
- Adaptive Reader test discovery is a release-gate requirement;
- reference-only source presence is not release evidence.

## Windows acceptance (2026-07-30)

Full acceptance report: [`ACCEPTANCE_REPORT.md`](../ACCEPTANCE_REPORT.md)
Evidence archive: `release-evidence/2026-07-30-win-acceptance/`

| Test runner | Total | Pass | Skip | Fail | Notes |
|---|---|---|---|---|---|
| `gdb_tutorial_test_runner` | 302 | 284 | 17 | 1 | 1 env-limit (GDAL data lacks XML metadata) |
| `fast_gdb_geometry_test_runner` | 101 | 101 | 0 | 0 |  |
| `fast_gdb_hybrid_test_runner` | 11 | 4 | 7 | 0 | All overlap with tutorial runner |
| `fast_gdb_adaptive_reader_test_runner` | 41 | 40 | 1 | 0 | 1 skip (OpenFileGDB no index delete) |
| `fast_gdb_gdal_read_write_boundary_test_runner` | 2 | 2 | 0 | 0 |  |
| **Unique total** | **446** | **428** | **17** | **1** | **0 product code defects** |
