#!/usr/bin/env python3
"""Create or verify the content manifest for a versioned FileGDB fixture."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = Path("test_data/gdb/acceptance_metadata/manifest.json")
VOLATILE_SUFFIXES = (".lock",)


def digest_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def fixture_files(gdb_path: Path) -> list[dict[str, Any]]:
    files = []
    for path in sorted(gdb_path.rglob("*")):
        if not path.is_file() or path.name.endswith(VOLATILE_SUFFIXES):
            continue
        files.append(
            {
                "path": path.relative_to(gdb_path).as_posix(),
                "size": path.stat().st_size,
                "sha256": digest_file(path),
            }
        )
    return files


def validate_table_pairs(gdb_path: Path) -> list[str]:
    tables = {path.stem for path in gdb_path.rglob("*.gdbtable")}
    indexes = {path.stem for path in gdb_path.rglob("*.gdbtablx")}
    errors = []
    for stem in sorted(tables - indexes):
        errors.append(f"{stem}.gdbtable has no matching .gdbtablx")
    for stem in sorted(indexes - tables):
        errors.append(f"{stem}.gdbtablx has no matching .gdbtable")
    return errors


def resolve_manifest_path(raw_path: str) -> Path:
    relative = Path(raw_path)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError("manifest path must stay inside the repository")
    resolved = (ROOT / relative).resolve()
    if not resolved.is_relative_to(ROOT):
        raise ValueError("manifest path must stay inside the repository")
    return resolved


def load_manifest(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        manifest = json.load(stream)
    if not isinstance(manifest, dict):
        raise ValueError("manifest root must be a JSON object")
    return manifest


def verify(manifest_path: Path) -> int:
    manifest = load_manifest(manifest_path)
    gdb_path = resolve_manifest_path(manifest["gdb_path"])
    if not gdb_path.is_dir():
        print(f"missing FileGDB directory: {gdb_path.relative_to(ROOT)}")
        return 1

    errors = validate_table_pairs(gdb_path)
    expected = manifest.get("file_inventory")
    if not isinstance(expected, list) or not expected:
        errors.append("manifest has no file_inventory; regenerate it with --update")
        expected = []

    expected_by_path = {item["path"]: item for item in expected}
    if len(expected_by_path) != len(expected):
        errors.append("manifest has duplicate file paths")

    actual = {item["path"]: item for item in fixture_files(gdb_path)}
    for name in sorted(expected_by_path.keys() - actual.keys()):
        errors.append(f"missing fixture file: {name}")
    for name in sorted(actual.keys() - expected_by_path.keys()):
        errors.append(f"unmanifested fixture file: {name}")
    for name in sorted(expected_by_path.keys() & actual.keys()):
        wanted = expected_by_path[name]
        got = actual[name]
        if wanted.get("size") != got["size"]:
            errors.append(
                f"size changed: {name} ({wanted.get('size')} != {got['size']})"
            )
        if wanted.get("sha256") != got["sha256"]:
            errors.append(f"SHA-256 changed: {name}")

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print(
        f"PASS: {manifest_path.relative_to(ROOT)} "
        f"({len(actual)} files, table/tablx pairs complete)"
    )
    return 0


def update(manifest_path: Path) -> int:
    manifest = load_manifest(manifest_path)
    gdb_path = resolve_manifest_path(manifest["gdb_path"])
    if not gdb_path.is_dir():
        print(f"missing FileGDB directory: {gdb_path}", file=sys.stderr)
        return 1
    errors = validate_table_pairs(gdb_path)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    manifest["file_inventory_schema"] = 1
    manifest["volatile_files_excluded"] = ["*.lock"]
    manifest["file_inventory"] = fixture_files(gdb_path)
    with manifest_path.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(
        f"Updated {manifest_path.relative_to(ROOT)} "
        f"({len(manifest['file_inventory'])} files)"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--update", action="store_true")
    args = parser.parse_args()
    manifest_path = resolve_manifest_path(args.manifest.as_posix())
    return update(manifest_path) if args.update else verify(manifest_path)


if __name__ == "__main__":
    raise SystemExit(main())
