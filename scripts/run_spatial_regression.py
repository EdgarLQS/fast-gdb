#!/usr/bin/env python3
"""Compare the current 10M spatial benchmark with main on POSIX runners."""

from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

COVERAGES = (
    "coverage_01pct",
    "coverage_10pct",
    "coverage_30pct",
    "coverage_80pct",
    "coverage_full",
)
SUMMARY_RE = re.compile(
    r"^(coverage_[^ ]+)\s+([0-9.]+)\s+([0-9.]+)\s+"
    r"([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)$",
    re.MULTILINE,
)
FUNNEL_RE = re.compile(
    r"^\s+funnel:.*invalid=([0-9]+) result=([0-9]+)",
    re.MULTILINE,
)


@dataclass(frozen=True)
class Sample:
    reference: str
    coverage: str
    fast_median_ms: float
    gdal_median_ms: float
    fast_p95_ms: float
    gdal_p95_ms: float
    fast_gdal_ratio: float
    invalid_geometries: int
    result_count: int
    log: str


def resolve_runner(build_dir: Path) -> Path:
    candidates = (
        build_dir / "bin" / "gdb_tutorial_test_runner",
        build_dir / "gdb_tutorial_test_runner",
        build_dir / "bin" / "Release" / "gdb_tutorial_test_runner",
        build_dir / "Release" / "gdb_tutorial_test_runner",
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError(f"cannot find gdb_tutorial_test_runner under {build_dir}")


def run_case(
    runner: Path,
    gdb_path: Path,
    coverage: str,
    reference: str,
    trials: int,
    output_dir: Path,
) -> Sample:
    env = os.environ.copy()
    env.update(
        {
            "FAST_GDB_RUN_SPATIAL_BENCHMARKS": "1",
            "FAST_GDB_BENCHMARK_PATH": str(gdb_path.resolve()),
            "FAST_GDB_BENCHMARK_LABEL": f"{reference} / 10m / warm / {coverage}",
            "FAST_GDB_BENCHMARK_CASE": coverage,
            "FAST_GDB_BENCHMARK_TRIALS": str(trials),
        }
    )
    for name in (
        "FAST_GDB_BENCHMARK_MODE",
        "FAST_GDB_BENCHMARK_STRICT_COLD",
        "FAST_GDB_BENCHMARK_CACHE_CLEAR_COMMAND",
        "FAST_GDB_SPATIAL_PROFILE",
    ):
        env.pop(name, None)

    completed = subprocess.run(
        [str(runner), "--gtest_filter=SpatialDensityBenchmark.DensityMatrixConfigured"],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    log_path = output_dir / f"{reference}-10m-warm-{coverage}.log"
    log_path.write_text(completed.stdout, encoding="utf-8")
    sys.stdout.write(completed.stdout)
    if completed.returncode != 0:
        raise RuntimeError(
            f"{reference}/{coverage} benchmark failed with exit code "
            f"{completed.returncode}; see {log_path}"
        )

    matches = list(SUMMARY_RE.finditer(completed.stdout))
    if len(matches) != 1:
        raise RuntimeError(
            f"expected one summary row for {reference}/{coverage}, found {len(matches)}"
        )
    summary = matches[0]
    if summary.group(1) != coverage:
        raise RuntimeError(
            f"expected {coverage}, parsed {summary.group(1)} for {reference}"
        )

    funnel_matches = list(FUNNEL_RE.finditer(completed.stdout))
    if len(funnel_matches) != 1:
        raise RuntimeError(
            f"expected one funnel row for {reference}/{coverage}, "
            f"found {len(funnel_matches)}"
        )
    invalid = int(funnel_matches[0].group(1))
    result_count = int(funnel_matches[0].group(2))
    if invalid != 0:
        raise RuntimeError(
            f"invalid geometries observed for {reference}/{coverage}: {invalid}"
        )

    return Sample(
        reference=reference,
        coverage=coverage,
        fast_median_ms=float(summary.group(2)),
        gdal_median_ms=float(summary.group(3)),
        fast_p95_ms=float(summary.group(4)),
        gdal_p95_ms=float(summary.group(5)),
        fast_gdal_ratio=float(summary.group(6)),
        invalid_geometries=invalid,
        result_count=result_count,
        log=str(log_path),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--current-build", required=True, type=Path)
    parser.add_argument("--baseline-build", required=True, type=Path)
    parser.add_argument("--gdb", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--trials", type=int, default=20)
    parser.add_argument("--max-regression", type=float, default=0.05)
    args = parser.parse_args()

    if not 1 <= args.trials <= 100:
        parser.error("--trials must be between 1 and 100")
    if args.max_regression < 0:
        parser.error("--max-regression must be non-negative")
    if not args.gdb.is_dir():
        parser.error(f"GDB directory does not exist: {args.gdb}")

    current_runner = resolve_runner(args.current_build)
    baseline_runner = resolve_runner(args.baseline_build)
    samples: list[Sample] = []
    current_by_coverage: dict[str, Sample] = {}
    baseline_by_coverage: dict[str, Sample] = {}

    # Alternate execution order to reduce monotonic thermal/cache bias.
    for index, coverage in enumerate(COVERAGES):
        order = (
            (("current", current_runner), ("main", baseline_runner))
            if index % 2 == 0
            else (("main", baseline_runner), ("current", current_runner))
        )
        for reference, runner in order:
            sample = run_case(
                runner,
                args.gdb,
                coverage,
                reference,
                args.trials,
                args.output,
            )
            samples.append(sample)
            if reference == "current":
                current_by_coverage[coverage] = sample
            else:
                baseline_by_coverage[coverage] = sample

    failures: list[str] = []
    for coverage in COVERAGES:
        current = current_by_coverage[coverage]
        baseline = baseline_by_coverage[coverage]
        limit = baseline.fast_median_ms * (1.0 + args.max_regression)
        if current.fast_median_ms > limit:
            failures.append(
                f"{coverage}: current={current.fast_median_ms:.1f}ms, "
                f"main={baseline.fast_median_ms:.1f}ms, limit={limit:.1f}ms"
            )

    args.output.mkdir(parents=True, exist_ok=True)
    csv_path = args.output / "posix-10m-main-regression.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=Sample.__dataclass_fields__.keys())
        writer.writeheader()
        for sample in samples:
            writer.writerow(sample.__dict__)

    if failures:
        raise RuntimeError(
            "10M spatial regression exceeded the allowed threshold:\n" +
            "\n".join(failures)
        )
    print(f"POSIX 10M main regression gate passed: {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
