#!/usr/bin/env python3
"""Stage a relocatable fast-gdb binary release directory."""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

LIBRARY_TARGETS = [
    "fast_gdb_geometry_core",
    "explorgdb_common_lib",
    "explorgdb_reader_lib",
    "explorgdb_writer_lib",
    "fast_gdb_curve_gdal",
]
LIBRARY_SUFFIXES = {".a", ".lib", ".so", ".dylib"}


def find_library(build_dir: Path, target: str) -> Path | None:
    candidates = []
    for path in build_dir.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in LIBRARY_SUFFIXES:
            continue
        if target in path.name:
            candidates.append(path)
    return sorted(candidates, key=lambda p: (len(p.parts), len(p.name)))[0] if candidates else None


def copy_headers(repo: Path, output: Path, variant: str) -> None:
    components = ["common", "reader", "writer"]
    if variant == "hybrid":
        components.append("curve_gdal")
    for component in components:
        source = repo / "src" / "edgar" / "explorgdb" / component
        destination = output / "include" / "explorgdb" / component
        destination.mkdir(parents=True, exist_ok=True)
        for header in source.rglob("*"):
            if header.is_file() and header.suffix.lower() in {".h", ".hpp"}:
                relative = header.relative_to(source)
                target = destination / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(header, target)


def write_cmake_config(output: Path, libraries: dict[str, str], variant: str) -> None:
    cmake_dir = output / "lib" / "cmake" / "fast_gdb"
    cmake_dir.mkdir(parents=True, exist_ok=True)
    lines = [
        "include_guard(GLOBAL)",
        "get_filename_component(_FAST_GDB_PREFIX \"${CMAKE_CURRENT_LIST_DIR}/../../..\" ABSOLUTE)",
    ]
    if variant == "hybrid":
        lines += ["include(CMakeFindDependencyMacro)", "find_dependency(GDAL)"]

    include_map = {
        "fast_gdb_geometry_core": ["reader"],
        "explorgdb_common_lib": ["common"],
        "explorgdb_reader_lib": ["reader"],
        "explorgdb_writer_lib": ["writer"],
        "fast_gdb_curve_gdal": ["curve_gdal", "reader"],
    }
    for target, filename in libraries.items():
        include_dirs = ["${_FAST_GDB_PREFIX}/include"]
        include_dirs.extend(
            f"${{_FAST_GDB_PREFIX}}/include/explorgdb/{component}"
            for component in include_map[target]
        )
        lines += [
            f"add_library(fast_gdb::{target} STATIC IMPORTED)",
            f"set_target_properties(fast_gdb::{target} PROPERTIES",
            f"  IMPORTED_LOCATION \"${{_FAST_GDB_PREFIX}}/lib/{filename}\"",
            f"  INTERFACE_INCLUDE_DIRECTORIES \"{';'.join(include_dirs)}\")",
        ]

    if "explorgdb_reader_lib" in libraries:
        lines += [
            "set_property(TARGET fast_gdb::explorgdb_reader_lib PROPERTY INTERFACE_LINK_LIBRARIES",
            "  \"fast_gdb::explorgdb_common_lib;fast_gdb::fast_gdb_geometry_core\")",
            "add_library(fast_gdb::fast_gdb_linear INTERFACE IMPORTED)",
            "set_property(TARGET fast_gdb::fast_gdb_linear PROPERTY INTERFACE_LINK_LIBRARIES",
            "  fast_gdb::explorgdb_reader_lib)",
        ]
    if variant == "hybrid" and "fast_gdb_curve_gdal" in libraries:
        lines += [
            "set_property(TARGET fast_gdb::fast_gdb_curve_gdal PROPERTY INTERFACE_LINK_LIBRARIES",
            "  \"fast_gdb::explorgdb_reader_lib;GDAL::GDAL\")",
            "add_library(fast_gdb::fast_gdb_hybrid INTERFACE IMPORTED)",
            "set_property(TARGET fast_gdb::fast_gdb_hybrid PROPERTY INTERFACE_LINK_LIBRARIES",
            "  fast_gdb::fast_gdb_curve_gdal)",
        ]
    (cmake_dir / "fast_gdbConfig.cmake").write_text("\n".join(lines) + "\n")
    (cmake_dir / "fast_gdbConfigVersion.cmake").write_text(
        "set(PACKAGE_VERSION \"0.1.0\")\n"
        "if(PACKAGE_FIND_VERSION_MAJOR EQUAL 0)\n"
        "  set(PACKAGE_VERSION_COMPATIBLE TRUE)\n"
        "endif()\n"
        "if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)\n"
        "  set(PACKAGE_VERSION_EXACT TRUE)\n"
        "endif()\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--variant", choices=("linear", "hybrid"), required=True)
    args = parser.parse_args()

    repo = args.repo.resolve()
    build = args.build.resolve()
    output = args.output.resolve()
    if output.exists():
        shutil.rmtree(output)
    (output / "lib").mkdir(parents=True)
    (output / "bin").mkdir(parents=True)

    libraries: dict[str, str] = {}
    required = LIBRARY_TARGETS[:4]
    if args.variant == "hybrid":
        required.append("fast_gdb_curve_gdal")
    for target in required:
        source = find_library(build, target)
        if source is None:
            raise FileNotFoundError(f"built library not found for target {target}")
        destination = output / "lib" / source.name
        shutil.copy2(source, destination)
        libraries[target] = destination.name

    for executable_name in ("explorgdb_cli", "explorgdb_cli.exe"):
        matches = list(build.rglob(executable_name))
        if matches:
            shutil.copy2(matches[0], output / "bin" / executable_name)

    copy_headers(repo, output, args.variant)
    write_cmake_config(output, libraries, args.variant)
    for filename in ("VERSION", "CHANGELOG.md", "CLAUDE.md"):
        source = repo / filename
        if source.exists():
            shutil.copy2(source, output / filename)
    release_notes = repo / "docs" / "releases" / "v0.1.0.md"
    if release_notes.exists():
        shutil.copy2(release_notes, output / "RELEASE_NOTES.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
