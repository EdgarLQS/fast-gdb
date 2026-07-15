# Phase H-W Windows spatial I/O acceptance

## Scope

- Branch: `agent/windows-mmap-optimization`
- Pull request: `#10`
- Windows build: x64 Release, MSYS2 UCRT64, GDAL enabled
- Data sets: generated uniform point FileGDBs with 1,000,000 and 10,000,000 features
- Coverage matrix: 1%, 10%, 30%, 80%, 100%
- Current-branch I/O modes:
  - `mmap`: full view when bounded and available; parser-owned sliding `MapViewOfFile` views otherwise
  - `fallback-sync`: mmap disabled and true synchronous positional batch reads
  - `fallback-overlapped`: mmap disabled and bounded asynchronous `ReadFile/OVERLAPPED` batches
- Cache modes:
  - `cold`: before every fast-gdb sample and every GDAL sample, RAMMap empties process working sets and the standby list; fast-gdb's process-level `.gdbtablx` cache is bypassed
  - `warm`: explicit warm-up followed by steady-state trials
- Trials: 20 per independent coverage process; median and P95 are reported
- Baselines:
  - Windows `fallback-sync` has an isolated current-versus-`main` A/B run; current must not be slower
  - Linux and macOS run a separate 10M warm current-versus-`main` matrix with a 5% regression limit

## P0-P3 implementation record

| Priority | Implementation | Failure behavior |
|---|---|---|
| P0 | UTF-8 `CreateFileW`; allocation-granularity aligned full mappings; parser-owned one-handle/one-view sliding mapping for large or forced-windowed files; dense and windowed sparse paths use mapped geometry bytes | Mapping creation/view failure leaves the CRT descriptor open and selects the synchronous positional-I/O path; sliding views are unmapped and their mapping handle is closed before descriptor teardown |
| P1 | Dense fallback reads configurable 1-8 MiB physical windows. Record prefixes are parsed from the window; only a record that actually crosses the window receives one exact follow-up read | Persistent synchronous read or parse failure returns zero so `QueryEngine` rolls back the sequential optimization and evaluates canonical candidates |
| P2 | Sparse `.spx` FIDs are resolved through `.gdbtablx`, sorted by physical offset, grouped by bounded physical windows, evaluated, and final matched FIDs restored to ascending order | Metrics and matched FIDs are snapshotted; partial failure rolls back and runs the canonical exact per-FID locator |
| P3 | Opt-in bounded prefetch uses short-lived `ReOpenFile(...FILE_FLAG_OVERLAPPED)` handles. Live handles are bounded by in-flight depth; no process-lifetime handle cache remains | Async launch, future retrieval, allocation, and overlapped-read failures all retry the affected batch with the true synchronous positional reader |

The parser's Windows mapping declaration is macro-free. POSIX compatibility macros remain within the Windows shim boundary. For MinGW/UCRT64, the UTF-8 `open` redirect is scoped through CMake to `gdb_table.cpp` only, avoiding a public or translation-unit-wide macro leak into unrelated standard-library headers.

## Self-review fixes covered by tests or acceptance logic

1. A 2 MiB binary record among small rows must not make every fallback batch use the table-wide maximum record size.
2. Synchronous and OVERLAPPED positional reads must preserve the shared CRT cursor.
3. Repeated OVERLAPPED reads must not grow the process handle count.
4. Forced full-map deferral must exercise bounded sliding views and remap at a new offset.
5. Forced async launch failure must complete through synchronous batches without changing FIDs.
6. Forced windowed mmap must preserve both dense and sparse query results.
7. The canonical per-FID fallback no longer retains a cache keyed only by a reusable integer descriptor.
8. Windows and POSIX current-versus-`main` A/B runs disable only the benchmark's independent fast-versus-GDAL timing gate; complete FID equality and invalid-geometry checks remain active.
9. Windows A/B execution order alternates current-first and main-first, and strict-cold subprocesses receive an absolute cache-clear command path.
10. Changes under `cmake/**` trigger both Windows and POSIX regression workflows.

Relevant suites and runners:

- `WindowsMmapIoTest`
- `SpatialQueryAdaptiveTest`
- `SpatialDensityBenchmark`
- `scripts/windows/run_spatial_main_ab.ps1`
- `scripts/run_spatial_regression.py`

## Correctness and path gates

Every formal Windows acceptance process fails unless all applicable conditions hold:

1. The complete fast-gdb 0-based FID vector equals the GDAL vector for every trial.
2. `invalid_geometries == 0` for every trial.
3. 1% and 10% satisfy `fast <= GDAL + 200 ms` or `fast <= 0.90 * GDAL`.
4. 30%, 80%, and 100% satisfy `fast <= 0.90 * GDAL`.
5. `mmap` observes an actual successful full or windowed mapping and no fallback geometry scan.
6. `fallback-sync` observes a synchronous fallback path, zero OVERLAPPED batches, and no parallel depth.
7. `fallback-overlapped` observes OVERLAPPED batches and an actual in-flight depth of at least two.

Separate A/B gates then require:

8. Windows fallback median is not greater than the same-machine `main` Release median for any scale/cache/coverage row.
9. Linux and macOS 10M warm medians do not regress more than 5% against `main` for any coverage row.

## Evidence layout

Formal Windows `matrix.csv` and per-case logs report one process per scale/mode/cache/coverage combination:

- execution path and candidate ratio;
- median/P95 fast-gdb and GDAL wall time;
- last-trial `geometry_scan_ms` and query wall total;
- per-process peak working set;
- bounded-batch calls/bytes;
- exact wide-record calls/bytes;
- OVERLAPPED batch calls and observed maximum depth;
- result count and invalid geometry count.

The isolated Windows A/B runner writes `windows-fallback-main-ab.csv`. The POSIX workflow writes `posix-10m-main-regression.csv` and per-case logs separately for Linux and macOS.

## Windows reproduction

Build the current branch and a `main` worktree with the same configuration. Create a cache-clear command that runs both `RAMMap64.exe -Ew` and `RAMMap64.exe -Es`.

Run the formal current-branch acceptance matrix:

```powershell
./scripts/windows/run_spatial_acceptance.ps1 `
  -BuildDir build-windows `
  -DataRoot test_data/spatial_matrix/windows `
  -OutputDir artifacts/windows-spatial-acceptance `
  -RamMapPath tools/rammap/clear-file-cache.cmd `
  -Generate
```

Then run the isolated fallback current-versus-`main` gate:

```powershell
./scripts/windows/run_spatial_main_ab.ps1 `
  -CurrentBuildDir build-windows `
  -BaselineBuildDir baseline/build-windows `
  -DataRoot test_data/spatial_matrix/windows `
  -OutputDir artifacts/windows-spatial-main-ab `
  -CacheClearCommand tools/rammap/clear-file-cache.cmd
```

The committed workflow creates this wrapper and accepts the Sysinternals EULA before sampling.

## POSIX reproduction

Set `FAST_GDB_SPATIAL_PROFILE=1` so the A/B runner owns the regression threshold while the benchmark retains FID and invalid-geometry validation:

```bash
FAST_GDB_SPATIAL_PROFILE=1 \
python3 scripts/run_spatial_regression.py \
  --current-build build-posix \
  --baseline-build baseline/build-posix \
  --gdb test_data/spatial_matrix/posix/point_10m/point_10m.gdb \
  --output artifacts/posix-spatial-regression/local \
  --trials 20 \
  --max-regression 0.05
```

## Current execution status

**BLOCKED — no acceptance result or PASS is claimed.**

For the current branch head `2579dba3508f6bc2725b05eb2b3592965d2b6f2e`, no hosted acceptance artifact is recorded in the repository. The earlier Windows and Linux/macOS jobs completed before executing a step (`steps=None`) and exposed no job log URL. The repository's existing release, spatial-query, and geometry-correctness workflows exhibit the same pre-step failure pattern.

This demonstrates that the updated workflows are syntactically registered, but no hosted runner has executed checkout, configuration, compilation, tests, or benchmarks. The implementation, deterministic tests, strict cache protocol, path assertions, and isolated A/B workflows are committed, but none has produced runtime evidence. This document must be updated with successful run URLs, uploaded CSV files, machine details, and the final PASS/FAIL decision before the draft pull request is marked ready.
