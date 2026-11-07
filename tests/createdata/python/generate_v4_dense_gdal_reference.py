#!/usr/bin/env python3
"""Generate a small GDAL-authored, dense v4 OpenFileGDB reference fixture.

The OpenFileGDB writer currently emits a v3 table. This script creates the
dataset with GDAL, then converts only the feature table and index headers to
the dense v4 layout understood by GDAL's OpenFileGDB reader. The result is a
synthetic compatibility fixture, not an ArcGIS-authored geodatabase.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


LAYER_NAME = "v4probe"
EXPECTED_FEATURES = 4


def run(command: list[str]) -> str:
    result = subprocess.run(command, check=True, text=True, capture_output=True)
    return result.stdout + result.stderr


def find_table_id(ogrinfo: str, gdb_path: Path) -> int:
    catalog = run([
        ogrinfo, "-ro", "-al", "-q", str(gdb_path), "GDB_SystemCatalog"
    ])
    pattern = re.compile(
        r"OGRFeature\(GDB_SystemCatalog\):(\d+)[^\n]*\n"
        r"\s+Name \(String\) = " + re.escape(LAYER_NAME) + r"(?:\n|$)"
    )
    match = pattern.search(catalog)
    if not match:
        raise RuntimeError(f"Could not find {LAYER_NAME!r} in GDB_SystemCatalog")
    return int(match.group(1))


def patch_dense_v4(gdb_path: Path, table_id: int) -> tuple[int, int]:
    stem = f"a{table_id:08x}"
    table_path = gdb_path / f"{stem}.gdbtable"
    tablx_path = gdb_path / f"{stem}.gdbtablx"
    table = bytearray(table_path.read_bytes())
    tablx = bytearray(tablx_path.read_bytes())

    if len(table) < 40 or struct.unpack_from("<I", table)[0] != 3:
        raise RuntimeError("Expected a GDAL-created v3 feature table")
    if len(tablx) < 32 or struct.unpack_from("<I", tablx)[0] != 3:
        raise RuntimeError("Expected a GDAL-created v3 feature index")

    record_count = struct.unpack_from("<I", table, 4)[0]
    blocks = struct.unpack_from("<I", tablx, 4)[0]
    indexed_count = struct.unpack_from("<I", tablx, 8)[0]
    offset_width = struct.unpack_from("<I", tablx, 12)[0]
    if record_count != EXPECTED_FEATURES or indexed_count != record_count:
        raise RuntimeError(
            f"Expected {EXPECTED_FEATURES} dense records, got table={record_count}, "
            f"index={indexed_count}"
        )
    if offset_width not in (4, 5, 6):
        raise RuntimeError(f"Unsupported offset width {offset_width}")

    table_end = 16 + blocks * 1024 * offset_width
    if table_end + 16 != len(tablx):
        raise RuntimeError("Unexpected v3 tablx trailer or file length")
    entries = [
        int.from_bytes(
            tablx[16 + i * offset_width:16 + (i + 1) * offset_width], "little"
        )
        for i in range(blocks * 1024)
    ]
    if any(value == 0 for value in entries[:record_count]):
        raise RuntimeError("Feature offsets are not a dense prefix")
    if any(entries[record_count:]):
        raise RuntimeError("Unexpected offsets after the dense feature prefix")

    # v4 table header: valid-record count is an int64 at byte offset 16.
    struct.pack_into("<I", table, 0, 4)
    struct.pack_into("<I", table, 4, 0)  # no deleted records in this fixture
    struct.pack_into("<Q", table, 16, record_count)

    # v4 tablx header: 64-bit block count, then the same offset width.
    struct.pack_into("<I", tablx, 0, 4)
    struct.pack_into("<Q", tablx, 4, blocks)
    struct.pack_into("<QI", tablx, table_end, record_count, 0)
    tablx[table_end + 12:table_end + 16] = b"\x00" * 4

    table_path.write_bytes(table)
    tablx_path.write_bytes(tablx)
    return record_count, table_end


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path,
                        help="new output path ending in .gdb; must not exist")
    parser.add_argument("--ogr2ogr", default="ogr2ogr")
    parser.add_argument("--ogrinfo", default="ogrinfo")
    args = parser.parse_args()

    output = args.output.expanduser().resolve()
    if output.suffix.lower() != ".gdb":
        parser.error("--output must end in .gdb")
    if output.exists():
        parser.error(f"output already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)

    features = []
    for index in range(EXPECTED_FEATURES):
        features.append({
            "type": "Feature",
            "id": index + 1,
            "properties": {
                "name": f"row-{index}",
                "wide_value": 2**40 + index,
            },
            "geometry": {"type": "Point", "coordinates": [index, index]},
        })
    source = {"type": "FeatureCollection", "features": features}

    with tempfile.TemporaryDirectory(prefix="fast-gdb-v4-reference-") as temp:
        temp_path = Path(temp)
        source_path = temp_path / "source.geojson"
        generated_gdb = temp_path / "generated.gdb"
        source_path.write_text(json.dumps(source), encoding="utf-8")
        run([
            args.ogr2ogr, "-f", "OpenFileGDB", str(generated_gdb), str(source_path),
            "-nln", LAYER_NAME, "-lco",
            "TARGET_ARCGIS_VERSION=ARCGIS_PRO_3_2_OR_LATER",
        ])
        table_id = find_table_id(args.ogrinfo, generated_gdb)
        record_count, _ = patch_dense_v4(generated_gdb, table_id)
        layer_info = run([
            args.ogrinfo, "-ro", "-so", str(generated_gdb), LAYER_NAME
        ])
        if not re.search(r"Feature Count:\s*4\b", layer_info):
            raise RuntimeError("GDAL could not read back all four v4 features")
        shutil.move(str(generated_gdb), str(output))

    print(f"Created {output}")
    print(f"Layer={LAYER_NAME}, table_id={table_id}, records={record_count}")
    print("Provenance: GDAL-created v3 data with dense v4 table/tablx headers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
