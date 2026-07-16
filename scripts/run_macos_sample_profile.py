#!/usr/bin/env python3
"""Run one Writer profiling command under macOS sample(1)."""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
import sys
import time
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable


CATEGORY_PATTERNS: tuple[tuple[str, tuple[str, ...]], ...] = (
    (
        "field_lookup_validation",
        ("validate_field", "validate_field_index", "mark_field_written"),
    ),
    (
        "geometry_encoding",
        (
            "geometryserializer",
            "serialize_point",
            "serialize_parts",
            "serialize_multipoint",
        ),
    ),
    (
        "row_encoding_memory_copy",
        ("rowbuffer", "memcpy", "memmove", "__memcpy", "__memmove"),
    ),
    (
        "buffered_write",
        ("append_encoded_row", "internal_flush", "fwrite", "__sfvwrite"),
    ),
    (
        "flush_close_tablx",
        (
            "gdbtablewriter::close",
            "update_table_header",
            "write_v3_header",
            "write_v4_header",
            "tablxwriter",
        ),
    ),
    (
        "open_discovery",
        (
            "open_existing",
            "discover_table_layout",
            "resolve_pristine_empty_table",
            "parse_table_layout",
            "catalogresolver",
        ),
    ),
    (
        "publish",
        (
            "atomicgdbwritesession::commit",
            "writersession::commit",
            "renamex_np",
            "renameat2",
        ),
    ),
    (
        "gdal_schema_reopen",
        ("gdal", "ogr", "openfilegdb"),
    ),
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def classify(text: str) -> str:
    lowered = text.lower()
    for category, patterns in CATEGORY_PATTERNS:
        if any(pattern in lowered for pattern in patterns):
            return category
    return "unclassified"


def parse_sample_lines(lines: Iterable[str]) -> tuple[Counter[str], list[dict[str, object]]]:
    counts: Counter[str] = Counter()
    symbols: Counter[str] = Counter()
    pattern = re.compile(r"^\s*(\d+)\s+(.+?)\s*$")
    for raw_line in lines:
        match = pattern.match(raw_line)
        if not match:
            continue
        count = int(match.group(1))
        symbol = match.group(2)
        # Exclude section headings and thread summaries that also start with counts.
        lowered = symbol.lower()
        if lowered.startswith(("thread ", "process ", "binary images:")):
            continue
        category = classify(symbol)
        counts[category] += count
        symbols[symbol] += count
    top = [
        {"inclusive_samples": count, "symbol_line": symbol, "category": classify(symbol)}
        for symbol, count in symbols.most_common(40)
    ]
    return counts, top


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario-id", required=True)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--duration-seconds", type=int, default=5)
    parser.add_argument("--interval-ms", type=int, default=1)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    command = list(args.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("a command is required after --")
    if sys.platform != "darwin":
        raise SystemExit("sample profiling is supported only on macOS")

    sample_tool = Path("/usr/bin/sample")
    if not sample_tool.exists():
        raise SystemExit("/usr/bin/sample is unavailable")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    raw_path = args.output_dir / f"{args.scenario_id}.sample.txt"
    command_log = args.output_dir / f"{args.scenario_id}.command.log"
    result_path = args.output_dir / f"{args.scenario_id}.profile.json"

    started_at = utc_now()
    started = time.perf_counter()
    with command_log.open("w", encoding="utf-8") as log:
        log.write("$ " + shlex.join(command) + "\n")
        log.flush()
        process = subprocess.Popen(
            command,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
        )
        time.sleep(0.15)
        if process.poll() is not None:
            exit_code = process.returncode
            payload = {
                "schema_version": 1,
                "scenario_id": args.scenario_id,
                "result": "FAIL",
                "message": "profile target exited before sample could attach",
                "command": command,
                "command_exit_code": exit_code,
                "started_at_utc": started_at,
                "finished_at_utc": utc_now(),
            }
            result_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
            return 1

        sample_command = [
            str(sample_tool),
            str(process.pid),
            str(max(1, args.duration_seconds)),
            str(max(1, args.interval_ms)),
            "-file",
            str(raw_path),
            "-mayDie",
        ]
        sample_result = subprocess.run(
            sample_command,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
        command_exit_code = process.wait()

    elapsed_ms = (time.perf_counter() - started) * 1000.0
    raw_text = raw_path.read_text(encoding="utf-8", errors="replace") if raw_path.exists() else ""
    category_counts, top_symbols = parse_sample_lines(raw_text.splitlines())
    total_inclusive = sum(category_counts.values())
    categories = [
        {
            "category": category,
            "inclusive_samples": count,
            "inclusive_percent": (count * 100.0 / total_inclusive) if total_inclusive else 0.0,
        }
        for category, count in category_counts.most_common()
    ]

    failures: list[str] = []
    if sample_result.returncode != 0:
        failures.append(f"sample exited with code {sample_result.returncode}")
    if command_exit_code != 0:
        failures.append(f"profile command exited with code {command_exit_code}")
    if not raw_text.strip():
        failures.append("sample produced no raw profile")
    if total_inclusive == 0:
        failures.append("sample profile contained no parseable inclusive symbol counts")

    payload = {
        "schema_version": 1,
        "scenario_id": args.scenario_id,
        "result": "FAIL" if failures else "PASS",
        "message": "; ".join(failures) if failures else None,
        "command": command,
        "sample_command": sample_command,
        "command_exit_code": command_exit_code,
        "sample_exit_code": sample_result.returncode,
        "duration_seconds": args.duration_seconds,
        "interval_ms": args.interval_ms,
        "elapsed_ms": elapsed_ms,
        "started_at_utc": started_at,
        "finished_at_utc": utc_now(),
        "raw_profile": str(raw_path),
        "command_log": str(command_log),
        "count_semantics": (
            "inclusive call-tree counts parsed from sample(1); categories can overlap "
            "in the original stack, so they are diagnostic rather than wall-clock phases"
        ),
        "categories": categories,
        "top_symbols": top_symbols,
    }
    result_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"result": payload["result"], "failures": failures}, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
