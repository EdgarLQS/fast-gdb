# Third-party notices

This file records third-party software and generated test data that may be
used by fast-gdb. These components keep their own licenses and are not
relicensed by the Apache-2.0 license in [`LICENSE`](LICENSE).

## GDAL / OpenFileGDB

GDAL is an optional build and runtime dependency for the `hybrid`, `adaptive`
and `unified` products, and is used by several integration tests. The
OpenFileGDB driver is part of GDAL. GDAL is distributed under an MIT-style
license by the GDAL project:

- Project: <https://github.com/OSGeo/gdal>
- License information: <https://github.com/OSGeo/gdal/blob/master/LICENSE.TXT>

The `linear` product does not require GDAL.

## GoogleTest

GoogleTest is a test-only dependency. If a compatible installed package is
not found, CMake may fetch GoogleTest `v1.15.2` through `FetchContent`.
GoogleTest is distributed under the BSD-3-Clause license:

- Project: <https://github.com/google/googletest>
- License: <https://github.com/google/googletest/blob/main/LICENSE>

fast-gdb does not vendor GoogleTest in the source tree.

## ArcGIS Pro generated acceptance fixture

`test_data/gdb/acceptance_metadata.gdb` and the expected-value files under
`test_data/gdb/acceptance_metadata/` were generated with ArcGIS Pro arcpy
3.5.0.57366. They are test fixtures, not ArcGIS software or ArcGIS API code.

The repository owner has confirmed that this generated fixture may be
redistributed with the repository. Its provenance and generation details are
recorded in
[`test_data/gdb/acceptance_metadata/source-notes.md`](test_data/gdb/acceptance_metadata/source-notes.md).

## Project-owned material

Unless a file or directory contains a more specific notice, original
fast-gdb source code and documentation are licensed under Apache-2.0. See
[`LICENSE`](LICENSE) for the complete license text.
