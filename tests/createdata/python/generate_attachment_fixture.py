#!/usr/bin/env python3
"""Generate a small ArcGIS-authored FileGDB attachment fixture.

Run with ArcGIS Pro's Python environment. The output GDB is ignored test data;
the companion TSV maps each attachment table row to the exact source payload so
the Reader contract test can compare bytes without using GDAL as the oracle.
"""

import argparse
import csv
import hashlib
import json
import shutil
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_GDB = REPO_ROOT / "test_data" / "gdb" / "attachment_semantics.gdb"
MAX_OVERSIZED_BYTES = 16 * 1024 * 1024


def normalize_guid(value):
    return str(value).strip().strip("{}").lower()


def make_payloads(payload_dir, oversized_bytes):
    payload_dir.mkdir(parents=True, exist_ok=True)
    large_bytes = bytes((index * 31 + 7) & 0xFF
                        for index in range(oversized_bytes))
    specifications = [
        ("PARENT-01", "binary-nul.bin",
         bytes((0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF, 0x00))),
        ("PARENT-01", "道路附件-🚉.txt",
         "地理附件 UTF-8\n".encode("utf-8") + bytes((0x00, 0xFF))),
        ("PARENT-02", "plain-note.txt", b"second parent attachment\n"),
        ("PARENT-02", "oversized.bin", large_bytes),
    ]

    payloads = []
    for parent_key, filename, contents in specifications:
        path = payload_dir / filename
        path.write_bytes(contents)
        payloads.append({
            "parent_key": parent_key,
            "filename": filename,
            "path": path,
            "byte_size": len(contents),
            "sha256": hashlib.sha256(contents).hexdigest(),
        })
    return payloads


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-gdb", type=Path, default=DEFAULT_GDB,
                        help=f"output FileGDB (default: {DEFAULT_GDB})")
    parser.add_argument(
        "--oversized-bytes", type=int, default=131073,
        help="size of the payload used to exercise caller byte limits "
             "(default: 131073)")
    parser.add_argument(
        "--overwrite", action="store_true",
        help="remove only this fixture's GDB and companion output files")
    return parser


def import_arcpy():
    try:
        import arcpy
    except ImportError as error:
        raise SystemExit(
            "ArcPy is required; run this script with ArcGIS Pro's Python "
            "environment") from error
    return arcpy


def run(args):
    if args.oversized_bytes <= 65536 or args.oversized_bytes > MAX_OVERSIZED_BYTES:
        raise SystemExit(
            "--oversized-bytes must be greater than 65536 and no more than "
            f"{MAX_OVERSIZED_BYTES}")

    out_gdb = args.out_gdb.resolve()
    out_dir = out_gdb.parent
    stem = out_gdb.stem
    payload_dir = out_dir / f"{stem}_payloads"
    expected_tsv = out_dir / f"{stem}_expected.tsv"
    manifest_json = out_dir / f"{stem}_manifest.json"
    artifacts = (out_gdb, payload_dir, expected_tsv, manifest_json)

    existing = [path for path in artifacts if path.exists()]
    if existing and not args.overwrite:
        paths = "\n".join(str(path) for path in existing)
        raise SystemExit(
            "fixture outputs already exist; pass --overwrite to replace "
            "these exact outputs:\n" + paths)

    arcpy = import_arcpy()
    if args.overwrite:
        if arcpy.Exists(str(out_gdb)):
            arcpy.management.Delete(str(out_gdb))
        if payload_dir.exists():
            shutil.rmtree(payload_dir)
        for path in (expected_tsv, manifest_json):
            if path.exists():
                path.unlink()

    out_dir.mkdir(parents=True, exist_ok=True)
    payloads = make_payloads(payload_dir, args.oversized_bytes)
    arcpy.env.overwriteOutput = False

    arcpy.management.CreateFileGDB(str(out_dir), out_gdb.name)
    parent_name = "attachment_features"
    parent_path = str(out_gdb / parent_name)
    spatial_reference = arcpy.SpatialReference(4326)
    arcpy.management.CreateFeatureclass(
        str(out_gdb), parent_name, "POINT", spatial_reference=spatial_reference)
    arcpy.management.AddField(parent_path, "PARENT_KEY", "TEXT",
                              field_length=20)
    arcpy.management.AddField(parent_path, "PARENT_NAME", "TEXT",
                              field_length=80)
    arcpy.management.AddGlobalIDs(parent_path)

    with arcpy.da.InsertCursor(
            parent_path, ["SHAPE@", "PARENT_KEY", "PARENT_NAME"]) as cursor:
        cursor.insertRow([
            arcpy.PointGeometry(arcpy.Point(116.4, 39.9), spatial_reference),
            "PARENT-01", "含多份附件的点位",
        ])
        cursor.insertRow([
            arcpy.PointGeometry(arcpy.Point(116.5, 39.9), spatial_reference),
            "PARENT-02", "含较大附件的点位",
        ])
        cursor.insertRow([
            arcpy.PointGeometry(arcpy.Point(116.6, 39.9), spatial_reference),
            "PARENT-03", "没有附件的点位",
        ])

    global_id_field = next(
        field.name for field in arcpy.ListFields(parent_path)
        if field.type == "GlobalID")
    parent_global_ids = {}
    with arcpy.da.SearchCursor(
            parent_path, ["PARENT_KEY", global_id_field]) as cursor:
        for parent_key, global_id in cursor:
            parent_global_ids[parent_key] = str(global_id)
    if set(parent_global_ids) != {"PARENT-01", "PARENT-02", "PARENT-03"}:
        raise RuntimeError("ArcGIS did not return all three parent GlobalIDs")

    match_table_name = "attachment_match"
    match_table_path = str(out_gdb / match_table_name)
    arcpy.management.CreateTable(str(out_gdb), match_table_name)
    arcpy.management.AddField(match_table_path, "PARENT_GUID", "GUID")
    arcpy.management.AddField(match_table_path, "FILE_PATH", "TEXT",
                              field_length=512)
    with arcpy.da.InsertCursor(
            match_table_path, ["PARENT_GUID", "FILE_PATH"]) as cursor:
        for payload in payloads:
            cursor.insertRow([
                parent_global_ids[payload["parent_key"]],
                str(payload["path"]),
            ])

    arcpy.management.EnableAttachments(parent_path)
    arcpy.management.AddAttachments(
        parent_path, global_id_field, match_table_path,
        "PARENT_GUID", "FILE_PATH")
    arcpy.management.Delete(match_table_path)

    attachment_name = f"{parent_name}__ATTACH"
    attachment_path = str(out_gdb / attachment_name)
    if not arcpy.Exists(attachment_path):
        raise RuntimeError(f"ArcGIS did not create {attachment_name}")

    source_by_parent_and_name = {
        (normalize_guid(parent_global_ids[item["parent_key"]]),
         item["filename"]): item
        for item in payloads
    }
    parent_by_global_id = {
        normalize_guid(global_id): parent_key
        for parent_key, global_id in parent_global_ids.items()
    }
    descriptor = arcpy.Describe(attachment_path)
    oid_field = descriptor.OIDFieldName
    field_names = {field.name.upper(): field.name
                   for field in arcpy.ListFields(attachment_path)}
    required_fields = (oid_field, "REL_GLOBALID", "ATT_NAME", "DATA_SIZE", "DATA")
    missing_fields = [name for name in required_fields
                      if name.upper() not in field_names]
    if missing_fields:
        raise RuntimeError(
            "ArcGIS attachment table lacks fields: " + ", ".join(missing_fields))
    cursor_fields = [field_names[name.upper()] for name in required_fields]

    rows = []
    observed = set()
    with arcpy.da.SearchCursor(attachment_path, cursor_fields) as cursor:
        for object_id, related_global_id, attachment_name_value, data_size, blob in cursor:
            key = (normalize_guid(related_global_id), str(attachment_name_value))
            source = source_by_parent_and_name.get(key)
            if source is None:
                raise RuntimeError(
                    "unexpected attachment row: " + repr(key))
            payload = bytes(blob) if blob is not None else b""
            digest = hashlib.sha256(payload).hexdigest()
            if len(payload) != int(data_size) or digest != source["sha256"]:
                raise RuntimeError(
                    f"ArcGIS payload differs from source file: {source['filename']}")
            observed.add(key)
            rows.append({
                "row_slot": int(object_id) - 1,
                "object_id": int(object_id),
                "parent_key": parent_by_global_id[
                    normalize_guid(related_global_id)],
                "parent_globalid": str(related_global_id),
                "attachment_name": str(attachment_name_value),
                "data_size": int(data_size),
                "sha256": digest,
                "payload_path": str(
                    source["path"].relative_to(out_dir).as_posix()),
            })

    if observed != set(source_by_parent_and_name):
        raise RuntimeError("ArcGIS did not create every requested attachment")
    rows.sort(key=lambda row: row["row_slot"])

    with expected_tsv.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow([
            "row_slot", "object_id", "parent_key", "parent_globalid",
            "attachment_name", "data_size", "sha256", "payload_path",
        ])
        for row in rows:
            writer.writerow([row[key] for key in (
                "row_slot", "object_id", "parent_key", "parent_globalid",
                "attachment_name", "data_size", "sha256", "payload_path",
            )])

    manifest = {
        "generator": Path(__file__).name,
        "arcgis_pro_version": arcpy.GetInstallInfo().get("Version", "unknown"),
        "python_version": sys.version.split()[0],
        "gdb": out_gdb.name,
        "parent_feature_class": parent_name,
        "attachment_table": attachment_name,
        "attachment_relationship": f"{parent_name}__ATTACHREL",
        "global_id_field": global_id_field,
        "attachment_count": len(rows),
        "parents": [
            {"parent_key": key, "globalid": parent_global_ids[key]}
            for key in sorted(parent_global_ids)
        ],
        "oversized_payload_bytes": args.oversized_bytes,
        "expected_tsv": expected_tsv.name,
        "oracle": "ArcPy SearchCursor over the ArcGIS-authored attachment table",
    }
    manifest_json.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")

    print(f"GDB: {out_gdb}")
    print(f"Expected rows: {expected_tsv}")
    print(f"Manifest: {manifest_json}")
    print(f"Attachment rows verified by ArcPy: {len(rows)}")


def main():
    parser = build_parser()
    args = parser.parse_args()
    run(args)


if __name__ == "__main__":
    main()
