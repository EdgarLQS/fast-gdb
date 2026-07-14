# Phase H-W Windows spatial I/O acceptance

## Scope

- Branch: `agent/windows-mmap-optimization`
- Pull request: `#10`
- Build configuration: Windows x64 Release, UCRT64, GDAL enabled
- Data sets: generated uniform point FileGDBs with 1,000,000 and 10,000,000 features
- Coverage matrix: 1%, 10%, 30%, 80%, 100%
- I/O modes:
  - `mmap`: `CreateFileMappingW` + `MapViewOfFile`
  - `fallback-sync`: forced mmap failure + bounded synchronous batch reads
  - `fallback-overlapped`: forced mmap failure + bounded `ReadFile/OVERLAPPED` prefetch
- Cache modes:
  - `cold`: RAMMap working-set/file-cache clear before the fresh-open benchmark process
  - `warm`: explicit warm-up followed by steady-state trials
- Trials: 20 per coverage row; median and P95 are reported

## P0-P3 implementation record

| Priority | Implementation | Failure behavior |
|---|---|---|
| P0 | UTF-8 `CreateFileW`; allocation-granularity aligned `CreateFileMappingW`/`MapViewOfFile`; logical-pointer registry; `UnmapViewOfFile` then mapping-handle close; existing `mapped_data_` zero-copy path | `FAST_GDB_WINDOWS_MMAP=0` or a mapping API failure leaves the file descriptor open and selects positional I/O |
| P1 | Dense geometry scans merge physical records into configurable 1-8 MiB windows and parse multiple rows from each read | A batch read is retried once; a persistent failure returns zero so `QueryEngine` uses the canonical candidate fallback |
| P2 | Sparse `.spx` candidates are resolved through `.gdbtablx`, sorted by physical offset, merged into bounded ranges, evaluated, then the final matched FIDs are restored to ascending order | Metrics and matched FIDs are snapshotted; a partial batch failure rolls back and re-runs the canonical per-FID locator |
| P3 | Bounded asynchronous prefetch uses `ReadFile` with `OVERLAPPED`; synchronous CRT handles are reopened with `ReOpenFile(...FILE_FLAG_OVERLAPPED)` and cached by file identity | P3 is opt-in (`FAST_GDB_WINDOWS_ASYNC_IO=1`); default fallback remains synchronous until the acceptance result justifies enabling it |

## Correctness gates

The benchmark fails when any of the following is false:

1. The complete fast-gdb 0-based FID vector equals the GDAL result vector for every trial.
2. `invalid_geometries == 0` for every trial.
3. 1% and 10% satisfy `fast <= GDAL + 200 ms` or `fast <= 0.90 * GDAL`.
4. 30%, 80%, and 100% satisfy `fast <= 0.90 * GDAL`.
5. Windows mmap failure does not crash and completes through a fallback path.

Additional evidence captured in `matrix.csv` and per-case logs:

- execution path;
- candidate count and ratio;
- median/P95 fast-gdb and GDAL wall time;
- last-trial `geometry_scan_ms` and query wall total;
- peak process working set;
- batch read call count, bytes read, and maximum asynchronous depth;
- result count and invalid geometry count.

## Reproduction

```powershell
cmake -S . -B build-windows -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DFAST_GDB_WITH_GDAL=ON `
  -DFAST_GDB_CURVE_BACKEND=BUILTIN `
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB `
  -DFAST_GDB_BUILD_TOOLS=ON `
  -DFAST_GDB_BUILD_FULL_TESTS=ON `
  -DBUILD_TESTING=ON

cmake --build build-windows `
  --target gdb_tutorial_test_runner generate_large_gdb --parallel 2

./scripts/windows/run_spatial_acceptance.ps1 `
  -BuildDir build-windows `
  -DataRoot test_data/spatial_matrix/windows `
  -OutputDir artifacts/windows-spatial-acceptance `
  -RamMapPath tools/rammap/RAMMap64.exe `
  -Generate
```

## Current execution status

**BLOCKED — no acceptance result is claimed.**

GitHub Actions workflow `windows-spatial-acceptance`, run `29318680722`, terminated before the runner executed any step. GitHub returned a failed job with `steps=[]` and no log URL. The repository's existing `release`, `spatial-query`, and `geometry-correctness` workflows show the same pre-step failure pattern on the same commit, so there is no compiler, test, or benchmark output to evaluate.

The workflow and evidence collector are committed and reproducible, but the Windows Release matrix remains unexecuted until GitHub-hosted Actions execution is restored or the script is run on an available Windows x64 Release machine. This document must be updated with the uploaded `matrix.csv`, run URL, machine details, and a PASS/FAIL decision before the pull request is marked ready.

## Known boundary

The current P0 implementation maps the complete table view on Windows x64 and uses 64-bit offsets and `SIZE_T`; mapping failure falls back safely. A true remapping sliding-window parser for address-space-constrained processes is not exercised by the 1M/10M acceptance target and is not claimed as validated here.
