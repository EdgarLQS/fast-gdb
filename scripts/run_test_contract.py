#!/usr/bin/env python3
"""Run the fast-gdb macOS Writer contract and emit schema-v2 evidence.

The runner intentionally treats PASS, FAIL and SKIP as distinct results. A SKIP
must use one of the reasons frozen by M18.1 and never contributes to a pass
count. Required scenarios fail the process unless every executed iteration
passes. Observation scenarios are recorded without becoming release gates;
manual scenarios remain explicit SKIP entries until enabled.
"""

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
import subprocess
import sys
import time
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


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
    first_line = completed.stdout.strip().splitlines()
    return first_line[0] if first_line else "unknown"


def git_commit(workspace: Path) -> str:
    value = os.environ.get("GITHUB_SHA")
    if value:
        return value
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
        if not suite:
            continue
        test_name = raw_line.split("#", 1)[0].strip()
        if test_name:
            tests.append(f"{suite}{test_name}")
    return tests


def split_gtest_filter(expression: str) -> tuple[list[str], list[str]]:
    positive, separator, negative = expression.partition("-")
    positives = [item for item in positive.split(":") if item] or ["*"]
    negatives = [item for item in negative.split(":") if item] if separator else []
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


def infer_skip_reason(text: str, allowed: set[str]) -> str | None:
    lowered = text.lower()
    candidates = (
        ("manual_gate_disabled", ("set fast_gdb", "manual gate", "not enabled")),
        ("missing_gdal", ("missing gdal", "gdal is not available", "openfilegdb driver")),
        ("missing_dataset", ("missing dataset", "dataset is not present", "fixture is not present")),
        ("insufficient_disk", ("insufficient disk", "not enough disk", "free disk")),
        ("unsupported_platform", ("unsupported platform", "windows currently", "only supported on")),
    )
    for reason, needles in candidates:
        if reason in allowed and any(needle in lowered for needle in needles):
            return reason
    return None


def percentile95(values: list[float]) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, math.ceil(0.95 * len(ordered)) - 1)
    return ordered[index]


def format_command(command: list[str]) -> str:
    return shlex.join(command)


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
    for phase in PHASE_COLUMNS:
        record[phase] = None
    return record


def execute_process(
    command: list[str], cwd: Path, env: dict[str, str], stdout_path: Path
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


def run_scenario(
    manifest: dict[str, Any],
    scenario: dict[str, Any],
    metadata: dict[str, Any],
    workspace: Path,
    output_dir: Path,
    binaries: dict[str, Path],
    listed_tests: dict[str, list[str]],
    iteration: int,
    include_manual: bool,
) -> dict[str, Any]:
    record = base_record(manifest, scenario, metadata, iteration)
    allowed_skip_reasons = set(manifest["allowed_skip_reasons"])
    stem = f"{scenario['id']}-run-{iteration}"
    stdout_path = output_dir / f"{stem}.log"
    record["stdout_path"] = str(stdout_path.relative_to(workspace))

    if metadata["platform"] != manifest["platform"]:
        record.update(
            result="SKIP",
            skip_reason="unsupported_platform",
            message=f"Contract targets {manifest['platform']}; current platform is {metadata['platform']}.",
            finished_at_utc=utc_now(),
        )
        stdout_path.write_text(record["message"] + "\n", encoding="utf-8")
        return record

    if scenario["gate"] == "manual":
        manual_variable = scenario.get("manual_environment")
        enabled = include_manual and manual_variable and os.environ.get(manual_variable) == "1"
        if not enabled:
            record.update(
                result="SKIP",
                skip_reason="manual_gate_disabled",
                message=f"Manual gate requires --include-manual and {manual_variable}=1.",
                finished_at_utc=utc_now(),
            )
            stdout_path.write_text(record["message"] + "\n", encoding="utf-8")
            return record

    environment = os.environ.copy()
    environment.update({str(k): str(v) for k, v in scenario.get("environment", {}).items()})
    environment.setdefault("FAST_GDB_BENCHMARK_CODE_VERSION", metadata["commit"][:12])
    environment.setdefault("FAST_GDB_BENCHMARK_CACHE_STATE", scenario.get("cache_state", "unspecified"))
    environment.setdefault("FAST_GDB_BENCHMARK_OUTPUT_DIR", str(output_dir / "native-benchmarks"))
    environment.setdefault("FAST_GDB_RANDOM_SEED", str(manifest.get("random_seed", 42)))

    if scenario["kind"] == "command":
        command = [part.format(workspace=str(workspace)) for part in scenario["command"]]
        record["command"] = format_command(command)
        if not Path(command[0]).exists():
            record.update(
                result="FAIL",
                message=f"Command executable does not exist: {command[0]}",
                finished_at_utc=utc_now(),
            )
            stdout_path.write_text(record["message"] + "\n", encoding="utf-8")
            return record
        completed, elapsed_ms = execute_process(command, workspace, environment, stdout_path)
        record["total_ms"] = elapsed_ms
        record["result"] = "PASS" if completed.returncode == 0 else "FAIL"
        record["assertions_verified"] = completed.returncode == 0
        if completed.returncode != 0:
            record["message"] = f"Command exited with code {completed.returncode}."
        record["finished_at_utc"] = utc_now()
        return record

    binary_key = scenario.get("binary", "full")
    binary = binaries.get(binary_key)
    if binary is None or not binary.exists():
        record.update(
            result="FAIL",
            message=f"Test binary '{binary_key}' is unavailable: {binary}",
            finished_at_utc=utc_now(),
        )
        stdout_path.write_text(record["message"] + "\n", encoding="utf-8")
        return record

    selected_tests = matched_tests(listed_tests[binary_key], scenario["filter"])
    record["matched_tests"] = selected_tests
    if not selected_tests:
        record.update(
            result="FAIL",
            message=f"Google Test filter matched no tests: {scenario['filter']}",
            finished_at_utc=utc_now(),
        )
        stdout_path.write_text(record["message"] + "\n", encoding="utf-8")
        return record

    gtest_json_path = output_dir / f"{stem}.gtest.json"
    record["gtest_json_path"] = str(gtest_json_path.relative_to(workspace))
    command = [
        str(binary),
        f"--gtest_filter={scenario['filter']}",
        "--gtest_color=no",
        f"--gtest_output=json:{gtest_json_path}",
    ]
    record["command"] = format_command(command)
    completed, elapsed_ms = execute_process(command, workspace, environment, stdout_path)
    record["total_ms"] = elapsed_ms

    cases: list[dict[str, Any]] = []
    if gtest_json_path.exists():
        try:
            cases = flatten_gtest_cases(json.loads(gtest_json_path.read_text(encoding="utf-8")))
        except (OSError, json.JSONDecodeError) as error:
            record["message"] = f"Could not parse Google Test JSON: {error}"

    skipped_cases = [case for case in cases if str(case.get("result", "")).upper() == "SKIPPED"]
    failed_cases = [
        case
        for case in cases
        if case.get("failures") or str(case.get("result", "")).upper() == "FAILED"
    ]

    if completed.returncode != 0 or failed_cases:
        record["result"] = "FAIL"
        record["message"] = record["message"] or f"Google Test exited with code {completed.returncode}."
    elif skipped_cases:
        reason = infer_skip_reason(completed.stdout, allowed_skip_reasons)
        if reason is None:
            record["result"] = "FAIL"
            record["message"] = "Google Test skipped without an allowed structured reason."
        else:
            record["result"] = "SKIP"
            record["skip_reason"] = reason
            record["message"] = f"{len(skipped_cases)} Google Test case(s) skipped."
    else:
        record["result"] = "PASS"
        record["assertions_verified"] = True

    record["finished_at_utc"] = utc_now()
    return record


def annotate_statistics(records: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for record in records:
        grouped[record["scenario_id"]].append(record)

    summaries: dict[str, dict[str, Any]] = {}
    for scenario_id, scenario_records in grouped.items():
        passed_times = [
            float(record["total_ms"])
            for record in scenario_records
            if record["result"] == "PASS" and record["total_ms"] is not None
        ]
        median = None
        if passed_times:
            ordered = sorted(passed_times)
            middle = len(ordered) // 2
            median = (
                ordered[middle]
                if len(ordered) % 2
                else (ordered[middle - 1] + ordered[middle]) / 2.0
            )
        p95 = percentile95(passed_times)
        for record in scenario_records:
            record["median_ms"] = median
            record["p95_ms"] = p95
        results = [record["result"] for record in scenario_records]
        summaries[scenario_id] = {
            "result": "FAIL" if "FAIL" in results else ("SKIP" if "SKIP" in results else "PASS"),
            "iterations": len(scenario_records),
            "pass_count": results.count("PASS"),
            "fail_count": results.count("FAIL"),
            "skip_count": results.count("SKIP"),
            "median_ms": median,
            "p95_ms": p95,
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
    return parse_gtest_listing(completed.stdout)


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
    workspace = args.workspace.resolve()
    manifest_path = args.manifest.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "native-benchmarks").mkdir(exist_ok=True)

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    allowed_results = set(manifest.get("allowed_results", []))
    if allowed_results != {"PASS", "FAIL", "SKIP"}:
        raise ValueError("Manifest must freeze PASS, FAIL and SKIP as the only results.")

    selected_ids = set(args.scenarios or [])
    scenarios = [
        scenario
        for scenario in manifest["scenarios"]
        if not selected_ids or scenario["id"] in selected_ids
    ]
    if selected_ids - {scenario["id"] for scenario in scenarios}:
        missing = sorted(selected_ids - {scenario["id"] for scenario in scenarios})
        raise ValueError(f"Unknown scenario id(s): {', '.join(missing)}")

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

    listed_tests: dict[str, list[str]] = {}
    if metadata["platform"] == manifest["platform"]:
        for key, binary in binaries.items():
            if binary.exists():
                listed_tests[key] = load_listing(binary)
            else:
                listed_tests[key] = []
    else:
        listed_tests = {key: [] for key in binaries}

    shutil.copyfile(manifest_path, output_dir / "manifest.snapshot.json")

    records: list[dict[str, Any]] = []
    for scenario in scenarios:
        repeat = int(scenario.get("repeat", args.repeat if scenario["gate"] == "required" else 1))
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
                listed_tests,
                iteration,
                args.include_manual,
            )
            records.append(record)
            print(
                f"[{record['result']}] {record['scenario_id']} "
                f"iteration={record['iteration']}"
                + (f" reason={record['skip_reason']}" if record.get("skip_reason") else "")
            )

    scenario_summaries = annotate_statistics(records)
    required_ids = {scenario["id"] for scenario in scenarios if scenario["gate"] == "required"}
    required_failures = sorted(
        scenario_id
        for scenario_id in required_ids
        if scenario_summaries.get(scenario_id, {}).get("result") != "PASS"
    )

    evidence = {
        "schema_version": 2,
        "contract_id": manifest["contract_id"],
        "generated_at_utc": utc_now(),
        "metadata": metadata,
        "manifest_path": str(manifest_path.relative_to(workspace)),
        "output_directory": str(output_dir.relative_to(workspace)),
        "allowed_results": manifest["allowed_results"],
        "allowed_skip_reasons": manifest["allowed_skip_reasons"],
        "scenario_summaries": scenario_summaries,
        "required_failures": required_failures,
        "records": records,
    }
    (output_dir / "writer-contract-results-v2.json").write_text(
        json.dumps(evidence, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    write_csv(output_dir / "benchmark_results-v2.csv", records)

    execution_manifest = {
        "schema_version": 2,
        "contract_id": manifest["contract_id"],
        "commit": metadata["commit"],
        "complete_command": format_command(sys.argv),
        "manifest": str(manifest_path.relative_to(workspace)),
        "result_json": str((output_dir / "writer-contract-results-v2.json").relative_to(workspace)),
        "result_csv": str((output_dir / "benchmark_results-v2.csv").relative_to(workspace)),
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
        print("Required scenario failures: " + ", ".join(required_failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
