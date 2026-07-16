#!/usr/bin/env python3
"""Compare current/main/GDAL Writer benchmark evidence for M18.3."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class EvidenceKey:
    scenario: str
    engine: str


def load_json(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read JSON {path}: {error}") from error


def load_evidence(root: Path) -> dict[EvidenceKey, dict[str, Any]]:
    records: dict[EvidenceKey, dict[str, Any]] = {}
    for path in sorted(root.rglob("*.json")):
        payload = load_json(path)
        if payload.get("evidence_schema_version") != 2:
            continue
        scenario = payload.get("scenario")
        engine = payload.get("engine")
        if not isinstance(scenario, str) or not isinstance(engine, str):
            continue
        key = EvidenceKey(scenario, engine)
        if key in records:
            raise ValueError(
                f"duplicate evidence for scenario={scenario!r}, engine={engine!r}: "
                f"{records[key].get('_path')} and {path}"
            )
        payload["_path"] = str(path)
        records[key] = payload
    return records


def finite_positive(value: Any) -> bool:
    return isinstance(value, (int, float)) and value > 0


def percent_delta(current: float, baseline: float) -> float:
    return (current - baseline) * 100.0 / baseline


def ratio(numerator: float, denominator: float) -> float:
    return numerator / denominator


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--current", required=True, type=Path)
    parser.add_argument("--main", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--threshold-percent", type=float)
    args = parser.parse_args()

    manifest = load_json(args.manifest)
    threshold = (
        args.threshold_percent
        if args.threshold_percent is not None
        else float(manifest["current_vs_main_max_regression_percent"])
    )
    current = load_evidence(args.current)
    baseline = load_evidence(args.main)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    rows: list[dict[str, Any]] = []
    failures: list[str] = []

    for scenario in manifest["scenarios"]:
        if scenario.get("gate") == "manual-observe":
            continue
        scenario_id = scenario["id"]
        benchmark_scenario = scenario["benchmark_scenario"]
        expected_manifest = scenario["manifest"]

        current_writer = current.get(EvidenceKey(benchmark_scenario, "fast-gdb-writer"))
        main_writer = baseline.get(EvidenceKey(benchmark_scenario, "fast-gdb-writer"))
        current_gdal = current.get(EvidenceKey(benchmark_scenario, "gdal-single"))
        main_gdal = baseline.get(EvidenceKey(benchmark_scenario, "gdal-single"))

        missing = [
            name
            for name, value in (
                ("current writer", current_writer),
                ("main writer", main_writer),
                ("current GDAL", current_gdal),
                ("main GDAL", main_gdal),
            )
            if value is None
        ]
        if missing:
            failures.append(f"{scenario_id}: missing {', '.join(missing)} evidence")
            rows.append({"scenario_id": scenario_id, "status": "FAIL", "reason": failures[-1]})
            continue

        assert current_writer is not None
        assert main_writer is not None
        assert current_gdal is not None
        assert main_gdal is not None

        records = (current_writer, main_writer, current_gdal, main_gdal)
        invalid_reasons: list[str] = []
        for record in records:
            if record.get("correct") is not True:
                invalid_reasons.append(
                    f"correct=false in {record.get('_path', 'unknown evidence')}"
                )
            if record.get("manifest") != expected_manifest:
                invalid_reasons.append(
                    f"manifest mismatch in {record.get('_path', 'unknown evidence')}: "
                    f"{record.get('manifest')!r} != {expected_manifest!r}"
                )
            if record.get("platform") != "macOS":
                invalid_reasons.append(
                    f"platform is {record.get('platform')!r}, expected macOS"
                )
            if not finite_positive(record.get("median_ms")):
                invalid_reasons.append(
                    f"median_ms is not positive in {record.get('_path', 'unknown evidence')}"
                )

        cache_states = {str(record.get("cache_state")) for record in records}
        if len(cache_states) != 1:
            invalid_reasons.append(f"cache_state mismatch: {sorted(cache_states)}")

        if invalid_reasons:
            reason = "; ".join(invalid_reasons)
            failures.append(f"{scenario_id}: {reason}")
            rows.append({"scenario_id": scenario_id, "status": "FAIL", "reason": reason})
            continue

        current_ms = float(current_writer["median_ms"])
        main_ms = float(main_writer["median_ms"])
        current_gdal_ms = float(current_gdal["median_ms"])
        main_gdal_ms = float(main_gdal["median_ms"])
        regression = percent_delta(current_ms, main_ms)
        current_vs_gdal = ratio(current_ms, current_gdal_ms)
        gdal_drift = percent_delta(current_gdal_ms, main_gdal_ms)
        status = "PASS" if regression <= threshold else "FAIL"
        reason = ""
        if status == "FAIL":
            reason = (
                f"current regressed {regression:.3f}% versus main; "
                f"limit is {threshold:.3f}%"
            )
            failures.append(f"{scenario_id}: {reason}")

        rows.append(
            {
                "scenario_id": scenario_id,
                "priority": scenario["priority"],
                "benchmark_scenario": benchmark_scenario,
                "manifest": expected_manifest,
                "cache_state": current_writer.get("cache_state"),
                "current_code_version": current_writer.get("code_version"),
                "main_code_version": main_writer.get("code_version"),
                "current_writer_median_ms": current_ms,
                "main_writer_median_ms": main_ms,
                "current_gdal_median_ms": current_gdal_ms,
                "main_gdal_median_ms": main_gdal_ms,
                "current_vs_main_percent": regression,
                "current_vs_gdal_ratio": current_vs_gdal,
                "gdal_current_vs_main_percent": gdal_drift,
                "threshold_percent": threshold,
                "status": status,
                "reason": reason,
            }
        )

    result = {
        "schema_version": 1,
        "contract_id": manifest["contract_id"],
        "threshold_percent": threshold,
        "status": "FAIL" if failures else "PASS",
        "failures": failures,
        "rows": rows,
    }
    (args.output_dir / "writer-performance-comparison.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    columns = [
        "scenario_id",
        "priority",
        "benchmark_scenario",
        "manifest",
        "cache_state",
        "current_code_version",
        "main_code_version",
        "current_writer_median_ms",
        "main_writer_median_ms",
        "current_gdal_median_ms",
        "main_gdal_median_ms",
        "current_vs_main_percent",
        "current_vs_gdal_ratio",
        "gdal_current_vs_main_percent",
        "threshold_percent",
        "status",
        "reason",
    ]
    with (args.output_dir / "writer-performance-comparison.csv").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)

    markdown = [
        "# Writer macOS performance comparison",
        "",
        f"- Contract: `{manifest['contract_id']}`",
        f"- Current-vs-main regression limit: `{threshold:.3f}%`",
        f"- Result: **{result['status']}**",
        "",
        "| Scenario | Current ms | Main ms | Δ vs main | Current/GDAL | Status |",
        "|---|---:|---:|---:|---:|---|",
    ]
    for row in rows:
        if "current_writer_median_ms" not in row:
            markdown.append(
                f"| `{row['scenario_id']}` | — | — | — | — | {row['status']} |"
            )
            continue
        markdown.append(
            "| `{scenario_id}` | {current_writer_median_ms:.3f} | "
            "{main_writer_median_ms:.3f} | {current_vs_main_percent:+.3f}% | "
            "{current_vs_gdal_ratio:.3f}x | {status} |".format(**row)
        )
    if failures:
        markdown.extend(["", "## Failures", ""])
        markdown.extend(f"- {failure}" for failure in failures)
    (args.output_dir / "writer-performance-summary.md").write_text(
        "\n".join(markdown) + "\n", encoding="utf-8"
    )

    print(json.dumps({"status": result["status"], "failures": failures}, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
