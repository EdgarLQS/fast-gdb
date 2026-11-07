# Adaptive Reader end-to-end example

This standalone package consumer demonstrates the read-only Adaptive API and its
coordination with an external GDAL/OpenFileGDB updater. It creates a small GDB in
a unique temporary directory, reads it through fast-gdb, coordinates a GDAL
update, closes the GDAL dataset, reports the new generation, and reads the
updated value through a newly constructed Adaptive session. The temporary GDB is
removed when the process exits.

The example calls GDAL directly for fixture creation and update. fast-gdb does
not expose or perform writes.

Build and run it against an installation that includes `fast_gdb::adaptive`:

```sh
cmake -S examples/adaptive_reader -B build-adaptive-example \
  -DCMAKE_PREFIX_PATH="/path/to/fast-gdb/install;/path/to/gdal/install"
cmake --build build-adaptive-example --parallel
ctest --test-dir build-adaptive-example --output-on-failure
```

The package must have been built with both `FAST_GDB_WITH_GDAL=ON` and
`FAST_GDB_BUILD_ADAPTIVE_READER=ON`. If GDAL is installed in a nonstandard
runtime location, provide its library directory through the platform's runtime
loader search path when running the example.
