# Contributing to fast-gdb

Thank you for helping improve fast-gdb. The supported product is a C++17
FileGDB Reader. FileGDB creation and editing remain the responsibility of
GDAL/OpenFileGDB or ArcGIS; contributions must not silently turn historical
Writer experiments into a supported product.

## Before opening a change

- Search existing issues and documentation before proposing a new behavior.
- Keep changes focused and preserve the current public target names and
  Reader-only boundary.
- Do not commit credentials, private FileGDB data, or proprietary source data.
- For generated fixtures, document their provenance and redistribution terms.
- If a change affects performance, include same-environment before/after
  measurements for the representative main path. A local microbenchmark is
  not sufficient evidence for a performance claim.

## Local build and tests

### Dependency-free Reader

```bash
cmake -S . -B build-linear \
  -DFAST_GDB_WITH_GDAL=OFF \
  -DFAST_GDB_CURVE_BACKEND=BUILTIN \
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB \
  -DBUILD_TESTING=ON
cmake --build build-linear --parallel
ctest --test-dir build-linear --output-on-failure
```

### GDAL-backed products

```bash
cmake -S . -B build-hybrid \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_CURVE_BACKEND=BUILTIN \
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB \
  -DBUILD_TESTING=ON
cmake --build build-hybrid --parallel
ctest --test-dir build-hybrid --output-on-failure
```

`FAST_GDB_BUILD_ADAPTIVE_READER=ON` enables the optional Adaptive Reader and
requires GDAL. The full test suite may also fetch GoogleTest `v1.15.2` when a
compatible installed package is unavailable.

### Installed package consumer

Release-affecting changes should also install the package and run the
consumer under `tests/package_consumer`. Test `linear` with GDAL disabled and
test GDAL-backed variants with the matching GDAL installation.

## Code and documentation expectations

- Follow the existing C++17 style and keep public behavior explicit.
- Fail closed for unsupported or malformed FileGDB input.
- Do not claim S3, concurrent GDAL write/read, complete MultiPatch semantics,
  or arbitrary external Writer detection as supported without matching
  evidence and documentation updates.
- Update the README, changelog, compatibility notes, and relevant quality
  documents when behavior or support status changes.
- Keep historical material under `docs/archive/` clearly marked as historical.

## Pull requests

A pull request should explain the problem, the scope of the change, the
validation performed, and any skipped environment-gated tests. Include
reproducible commands for non-trivial changes. Do not include generated build
directories or local benchmark output unless the change specifically requires
reviewing an evidence artifact.

By submitting a contribution, you agree that it may be distributed under the
Apache-2.0 license used by this repository.
