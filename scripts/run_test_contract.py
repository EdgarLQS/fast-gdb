#!/usr/bin/env python3
"""Run a fast-gdb test contract and emit schema-v2 evidence."""

from __future__ import annotations

import argparse
import csv
import fnmatch
import json
import math
import os
import platform
import shlex
import shutil
import statistics
import subprocess
import sys
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

RESULTS = {"PASS", "FAIL", "SKIP"}
GATES = {"required", "observe", "manual"}
PHASE_COLUMNS = (
    "open_ms",
    "schema_ms",
    "write_ms",
    "flush_ms",
    "close_ms",
    "index_ms",
    "reopen_ms",
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def display_path(path: Path, workspace: Path) -> str:
    try:
        return str(path.resolve().relative_to(workspace.resolve()))
    except ValueError:
        return str(path.resolve())


def safe_output(command: list[str]) -> str:
    try:
        completed = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=15,
        )
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    lines = completed.stdout.strip().splitlines()
    return lines[0] if lines else "unknown"


def git_commit(workspace: Path) -> str:
    if os.environ.get("GITHUB_SHA"):
        return os.environ["GITHUB_SHA"]
    try:
        return subprocess.check_output(
            ["git", "-C", str(workspace), "rev-parse", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def parse_gtest_listing(output: str) -> list[str]:
    tests: list[str] = []
    suite = ""
    for raw_line in output.splitlines():
        if not raw_line.strip() or raw_line.lstrip().startswith("Running main"):
            continue
        if not raw_line.startswith((" ", "\t")):
            suite = raw_line.split("#", 1)[0].strip()
            continue
        test_name = raw_line.split("#", 1)[0].strip()
        if suite and test_name:
            tests.append(f"{suite}{test_name}")
    return tests


def split_gtest_filter(expression: str) -> tuple[list[str], list[str]]:
    positive, separator, negative = expression.partition("-")
    positives = [value for value in positive.split(":") if value] or ["*"]
    negatives = [value for value in negative.split(":") if value] if separator else []
    return positives, negatives


def matched_tests(all_tests: Iterable[str], expression: str) -> list[str]:
    positives, negatives = split_gtest_filter(expression)
    return [
        test
        for test in all_tests
        if any(fnmatch.fnmatchcase(test, pattern) for pattern in positives)
        and not any(fnmatch.fnmatchcase(test, pattern) for pattern in negatives)
    ]


def flatten_gtest_cases(payload: dict[str, Any]) -> list[dict[str, Any]]:
    cases: list[dict[str, Any]] = []
    for suite in payload.get("testsuites", []):
        for case in suite.get("testsuite", []):
            cases.append(case)
    return cases


def case_result(case: dict[str, Any]) -> str:
    return str(case.get("result", "")).upper()


def infer_skip_reason(text: str, allowed: set[str]) -> str | None:
    lowered = text.lower()
    candidates = (
        ("manual_gate_disabled", ("set fast_gdb", "manual gate", "not enabled")),
        ("missing_gdal", ("missing gdal", "gdal is not available", "openfilegdb driver")),
        ("missing_dataset", ("missing dataset", "dataset is not present", "fixture is not present")),
        ("insufficient_disk", ("insufficient disk", "not enough disk", "free disk")),
        ("unsupported_platform", ("unsupported platform", "windows currently", "only supported on")),
    )
    for reason, markers in candidates:
        if reason in allowed and any(marker in lowered for marker in markers):
            return reason
    return None


def percentile95(values: list[float]) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, math.ceil(0.95 * len(ordered)) - 1)]


def format_command(command: list[str]) -> str:
    return shlex.join(command)


def execute(
    command: list[str],
    cwd: Path,
    env: dict[str, str],
    stdout_path: Path,
) -> tuple[subprocess.CompletedProcess[str], float]:
    started = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    stdout_path.write_text(completed.stdout, encoding="utf-8")
    return completed, elapsed_ms


def base_record(
    manifest: dict[str, Any],
    scenario: dict[str, Any],
    metadata: dict[str, Any],
    iteration: int,
) -> dict[str, Any]:
    record: dict[str, Any] = {
        "schema_version": 2,
        "contract_id": manifest["contract_id"],
        "scenario_id": scenario["id"],
        "scenario_title": scenario["title"],
        "iteration": iteration,
        "gate": scenario["gate"],
        "result": "FAIL",
        "skip_reason": None,
        "commit": metadata["commit"],
        "platform": metadata["platform"],
        "macos_version": metadata["macos_version"],
        "architecture": metadata["architecture"],
        "compiler": metadata["compiler"],
        "build_type": metadata["build_type"],
        "gdal_version": metadata["gdal_version"],
        "data_scale": scenario.get("data_scale", "unspecified"),
        "random_seed": manifest.get("random_seed", 42),
        "cache_state": scenario.get("cache_state", "unspecified"),
        "matched_tests": [],
        "command": "",
        "started_at_utc": utc_now(),
        "finished_at_utc": None,
        "total_ms": None,
        "median_ms": None,
        "p95_ms": None,
        "throughput_rows_per_sec": None,
        "rss_peak_mb": None,
        "disk_bytes": None,
        "assertion_scope": scenario.get("assertions", []),
        "assertions_verified": False,
        "stdout_path": None,
        "gtest_json_path": None,
        "message": None,
    }
    for column in PHASE_COLUMNS:
        record[column] = None
    return record


def skip_record(
    record: dict[str, Any],
    reason: str,
    message: str,
    stdout_path: Path,
) -> dict[str, Any]:
    record.update(
        result="SKIP",
        skip_reason=reason,
        message=message,
        finished_at_utc=utc_now(),
    )
    stdout_path.write_text(message + "\n", encoding="utf-8")
    return record


def fail_record(
    record: dict[str, Any],
    message: str,
    stdout_path: Path,
) -> dict[str, Any]:
    record.update(result="FAIL", message=message, finished_at_utc=utc_now())
    if not stdout_path.exists():
        stdout_path.write_text(message + "\n", encoding="utf-8")
    return record


def run_scenario(
    manifest: dict[str, Any],
    scenario: dict[str, Any],
    metadata: dict[str, Any],
    workspace: Path,
    output_dir: Path,
    binaries: dict[str, Path],
    listings: dict[str, list[str]],
    iteration: int,
    include_manual: bool,
) -> dict[str, Any]:
    record = base_record(manifest, scenario, metadata, iteration)
    stem = f"{scenario['id']}-run-{iteration}"
    stdout_path = output_dir / f"{stem}.log"
    record["stdout_path"] = display_path(stdout_path, workspace)

    if metadata["platform"] != manifest["platform"]:
        return skip_record(
            record,
            "unsupported_platform",
            f"Contract targets {manifest['platform']}; current platform is {metadata['platform']}.",
            stdout_path,
        )

    if scenario["gate"] == "manual":
        variable = scenario.get("manual_environment")
        if not (include_manual and variable and os.environ.get(variable) == "1"):
            return skip_record(
                record,
                "manual_gate_disabled",
                f"Manual gate requires --include-manual and {variable}=1.",
                stdout_path,
            )

    environment = os.environ.copy()
    environment.update({str(key): str(value) for key, value in scenario.get("environment", {}).items()})
    environment.setdefault("FAST_GDB_BENCHMARK_CODE_VERSION", metadata["commit"][:12])
    environment.setdefault("FAST_GDB_BENCHMARK_CACHE_STATE", scenario.get("cache_state", "unspecified"))
    environment.setdefault("FAST_GDB_BENCHMARK_OUTPUT_DIR", str(output_dir / "native-benchmarks"))
    environment.setdefault("FAST_GDB_RANDOM_SEED", str(manifest.get("random_seed", 42)))

    if scenario["kind"] == "command":
        command = [part.format(workspace=str(workspace)) for part in scenario["command"]]
        record["command"] = format_command(command)
        if not Path(command[0]).exists():
            return fail_record(record, f"Command executable does not exist: {command[0]}", stdout_path)
        completed, elapsed_ms = execute(command, workspace, environment, stdout_path)
        record["total_ms"] = elapsed_ms
        record["finished_at_utc"] = utc_now()
        if completed.returncode == 0:
            record["result"] = "PASS"
            record["assertions_verified"] = True
        else:
            record["message"] = f"Command exited with code {completed.returncode}."
        return record

    binary_key = scenario.get("binary", "full")
    binary = binaries.get(binary_key)
    if binary is None or not binary.exists():
        return fail_record(record, f"Test binary '{binary_key}' is unavailable: {binary}", stdout_path)

    selected = matched_tests(listings.get(binary_key, []), scenario["filter"])
    record["matched_tests"] = selected
    if not selected:
        return fail_record(
            record,
            f"Google Test filter matched no tests: {scenario['filter']}",
            stdout_path,
        )

    gtest_json_path = output_dir / f"{stem}.gtest.json"
    record["gtest_json_path"] = display_path(gtest_json_path, workspace)
    command = [
        str(binary),
        f"--gtest_filter={scenario['filter']}",
        "--gtest_color=no",
        f"--gtest_output=json:{gtest_json_path}",
    ]
    record["command"] = format_command(command)
    completed, elapsed_ms = execute(command, workspace, environment, stdout_path)
    record["total_ms"] = elapsed_ms
    record["finished_at_utc"] = utc_now()

    if not gtest_json_path.exists():
        record["message"] = "Google Test JSON was not produced."
        return record

    try:
        payload = json.loads(gtest_json_path.read_text(encoding="utf-8"))
        cases = flatten_gtest_cases(payload)
    except (OSError, json.JSONDecodeError) as error:
        record["message"] = f"Could not parse Google Test JSON: {error}"
        return record

    if not cases:
        record["message"] = "Google Test JSON contains no executed cases."
        return record

    skipped_cases = [case for case in cases if case_result(case) == "SKIPPED"]
    failed_cases = [
        case
        for case in cases
        if case.get("failures") or case_result(case) == "FAILED"
    ]

    if completed.returncode != 0 or failed_cases:
        record["message"] = f"Google Test exited with code {completed.returncode}."
    elif skipped_cases:
        reason = infer_skip_reason(completed.stdout, set(manifest["allowed_skip_reasons"]))
        if reason is None:
            record["message"] = "Google Test skipped without an allowed structured reason."
        else:
            record["result"] = "SKIP"
            record["skip_reason"] = reason
            record["message"] = f"{len(skipped_cases)} Google Test case(s) skipped."
    else:
        record["result"] = "PASS"
        record["assertions_verified"] = True
    return record


def validate_manifest(manifest: dict[str, Any]) -> None:
    if manifest.get("schema_version") != 2:
        raise ValueError("Manifest schema_version must be 2.")
    if set(manifest.get("allowed_results", [])) != RESULTS:
        raise ValueError("Manifest must freeze PASS, FAIL and SKIP as the only results.")
    allowed_skips = manifest.get("allowed_skip_reasons", [])
    if not allowed_skips or len(allowed_skips) != len(set(allowed_skips)):
        raise ValueError("Manifest skip reasons must be unique and non-empty.")
    scenarios = manifest.get("scenarios", [])
    ids = [scenario.get("id") for scenario in scenarios]
    if not scenarios or None in ids or len(ids) != len(set(ids)):
        raise ValueError("Manifest scenario IDs must be unique and non-empty.")
    for scenario in scenarios:
        if scenario.get("gate") not in GATES:
            raise ValueError(f"Scenario {scenario['id']} has invalid gate.")
        if scenario.get("kind") not in {"gtest", "command"}:
            raise ValueError(f"Scenario {scenario['id']} has invalid kind.")
        if scenario["kind"] == "gtest" and not scenario.get("filter"):
            raise ValueError(f"Scenario {scenario['id']} is missing a Google Test filter.")
        if scenario["kind"] == "command" and not scenario.get("command"):
            raise ValueError(f"Scenario {scenario['id']} is missing a command.")
        if scenario["gate"] == "manual" and not scenario.get("manual_environment"):
            raise ValueError(f"Manual scenario {scenario['id']} is missing manual_environment.")


def annotate_statistics(records: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for record in records:
        grouped[record["scenario_id"]].append(record)

    summaries: dict[str, dict[str, Any]] = {}
    for scenario_id, scenario_records in grouped.items():
        pass_times = [
            float(record["total_ms"])
            for record in scenario_records
            if record["result"] == "PASS" and record["total_ms"] is not None
        ]
        median_ms = statistics.median(pass_times) if pass_times else None
        p95_ms = percentile95(pass_times)
        for record in scenario_records:
            record["median_ms"] = median_ms
            record["p95_ms"] = p95_ms
        results = [record["result"] for record in scenario_records]
        summaries[scenario_id] = {
            "result": "FAIL" if "FAIL" in results else ("SKIP" if "SKIP" in results else "PASS"),
            "iterations": len(scenario_records),
            "pass_count": results.count("PASS"),
            "fail_count": results.count("FAIL"),
            "skip_count": results.count("SKIP"),
            "median_ms": median_ms,
            "p95_ms": p95_ms,
        }
    return summaries


def write_csv(path: Path, records: list[dict[str, Any]]) -> None:
    columns = [
        "schema_version",
        "contract_id",
        "scenario_id",
        "iteration",
        "gate",
        "result",
        "skip_reason",
        "commit",
        "platform",
        "macos_version",
        "architecture",
        "compiler",
        "build_type",
        "gdal_version",
        "data_scale",
        "random_seed",
        "cache_state",
        *PHASE_COLUMNS,
        "total_ms",
        "median_ms",
        "p95_ms",
        "throughput_rows_per_sec",
        "rss_peak_mb",
        "disk_bytes",
        "assertions_verified",
        "assertion_scope",
        "matched_tests",
        "command",
        "stdout_path",
        "gtest_json_path",
        "message",
    ]
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        for record in records:
            row = dict(record)
            row["assertion_scope"] = ";".join(record.get("assertion_scope", []))
            row["matched_tests"] = ";".join(record.get("matched_tests", []))
            writer.writerow(row)


def load_listing(binary: Path) -> list[str]:
    completed = subprocess.run(
        [str(binary), "--gtest_list_tests"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"Could not list Google Tests from {binary}:\n{completed.stdout}")
    tests = parse_gtest_listing(completed.stdout)
    if not tests:
        raise RuntimeError(f"Google Test listing from {binary} was empty.")
    return tests


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--full-test-binary", required=True, type=Path)
    parser.add_argument("--geometry-test-binary", type=Path)
    parser.add_argument("--workspace", type=Path, default=Path.cwd())
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--build-type", default="Release")
    parser.add_argument("--scenario", action="append", dest="scenarios")
    parser.add_argument("--include-manual", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.repeat < 1:
        raise ValueError("--repeat must be at least 1.")

    workspace = args.workspace.resolve()
    manifest_path = args.manifest.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "native-benchmarks").mkdir(exist_ok=True)

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    validate_manifest(manifest)

    selected_ids = set(args.scenarios or [])
    scenarios = [
        scenario
        for scenario in manifest["scenarios"]
        if not selected_ids or scenario["id"] in selected_ids
    ]
    unknown = selected_ids - {scenario["id"] for scenario in scenarios}
    if unknown:
        raise ValueError(f"Unknown scenario id(s): {', '.join(sorted(unknown))}")

    metadata = {
        "commit": git_commit(workspace),
        "platform": "macOS" if platform.system() == "Darwin" else platform.system(),
        "macos_version": platform.mac_ver()[0] or "not_macos",
        "architecture": platform.machine() or "unknown",
        "compiler": safe_output([os.environ.get("CXX", "c++"), "--version"]),
        "build_type": args.build_type,
        "gdal_version": safe_output(["gdal-config", "--version"]),
        "python_version": platform.python_version(),
    }

    binaries: dict[str, Path] = {"full": args.full_test_binary.resolve()}
    if args.geometry_test_binary:
        binaries["geometry"] = args.geometry_test_binary.resolve()

    listings: dict[str, list[str]] = {}
    if metadata["platform"] == manifest["platform"]:
        for key, binary in binaries.items():
            listings[key] = load_listing(binary) if binary.exists() else []
    else:
        listings = {key: [] for key in binaries}

    shutil.copyfile(manifest_path, output_dir / "manifest.snapshot.json")

    records: list[dict[str, Any]] = []
    for scenario in scenarios:
        repeat = int(
            scenario.get(
                "repeat",
                args.repeat if scenario["gate"] == "required" else 1,
            )
        )
        if repeat < 1:
            raise ValueError(f"Scenario {scenario['id']} has invalid repeat={repeat}")
        for iteration in range(1, repeat + 1):
            record = run_scenario(
                manifest,
                scenario,
                metadata,
                workspace,
                output_dir,
                binaries,
                listings,
                iteration,
                args.include_manual,
            )
            records.append(record)
            suffix = f" reason={record['skip_reason']}" if record.get("skip_reason") else ""
            print(
                f"[{record['result']}] {record['scenario_id']} "
                f"iteration={record['iteration']}{suffix}"
            )

    summaries = annotate_statistics(records)
    required_ids = {
        scenario["id"] for scenario in scenarios if scenario["gate"] == "required"
    }
    required_failures = sorted(
        scenario_id
        for scenario_id in required_ids
        if summaries.get(scenario_id, {}).get("result") != "PASS"
    )

    result_json = output_dir / "writer-contract-results-v2.json"
    result_csv = output_dir / "benchmark_results-v2.csv"
    evidence = {
        "schema_version": 2,
        "contract_id": manifest["contract_id"],
        "generated_at_utc": utc_now(),
        "metadata": metadata,
        "manifest_path": display_path(manifest_path, workspace),
        "output_directory": display_path(output_dir, workspace),
        "allowed_results": manifest["allowed_results"],
        "allowed_skip_reasons": manifest["allowed_skip_reasons"],
        "scenario_summaries": summaries,
        "required_failures": required_failures,
        "records": records,
    }
    result_json.write_text(
        json.dumps(evidence, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    write_csv(result_csv, records)

    execution_manifest = {
        "schema_version": 2,
        "contract_id": manifest["contract_id"],
        "commit": metadata["commit"],
        "complete_command": format_command([sys.executable, *sys.argv]),
        "manifest": display_path(manifest_path, workspace),
        "result_json": display_path(result_json, workspace),
        "result_csv": display_path(result_csv, workspace),
        "skip_items": [
            {
                "scenario_id": record["scenario_id"],
                "iteration": record["iteration"],
                "reason": record["skip_reason"],
            }
            for record in records
            if record["result"] == "SKIP"
        ],
    }
    (output_dir / "execution-manifest.json").write_text(
        json.dumps(execution_manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    if required_failures:
        print(
            "Required scenario failures: " + ", ".join(required_failures),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
