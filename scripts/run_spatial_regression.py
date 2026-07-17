#!/usr/bin/env python3
"""Compare current and baseline spatial benchmarks across one or more datasets."""

from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import re
import subprocess
import sys
from dataclasses import asdict, dataclass
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
FID_RE = re.compile(r"^\s+fids(?:_sha256)?:\s*(.+)$", re.MULTILINE)


@dataclass(frozen=True)
class Dataset:
    label: str
    path: Path


@dataclass(frozen=True)
class Sample:
    dataset: str
    reference: str
    mode: str
    coverage: str
    fast_median_ms: float
    gdal_median_ms: float
    fast_p95_ms: float
    gdal_p95_ms: float
    fast_gdal_ratio: float
    invalid_geometries: int
    result_count: int
    fid_signature: str
    log: str


def parse_dataset(value: str) -> Dataset:
    if "=" not in value:
        raise argparse.ArgumentTypeError("--dataset must use LABEL=PATH")
    label, raw_path = value.split("=", 1)
    label = label.strip()
    raw_path = raw_path.strip()
    if not label:
        raise argparse.ArgumentTypeError("dataset label must not be empty")
    if not raw_path:
        raise argparse.ArgumentTypeError("dataset path must not be empty")
    path = Path(raw_path).expanduser()
    return Dataset(label=label, path=path)


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


def safe_name(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "-", value).strip("-") or "dataset"


def compare_samples(
    current: Sample,
    baseline: Sample,
    max_regression: float,
) -> list[str]:
    """Return deterministic gate failures for one dataset/coverage pair."""
    failures: list[str] = []
    prefix = f"{current.dataset}/{current.coverage}"
    limit = baseline.fast_median_ms * (1.0 + max_regression)
    if current.fast_median_ms > limit:
        failures.append(
            f"{prefix}: current={current.fast_median_ms:.1f}ms, "
            f"main={baseline.fast_median_ms:.1f}ms, limit={limit:.1f}ms"
        )
    if current.result_count != baseline.result_count:
        failures.append(
            f"{prefix}: result count differs "
            f"current={current.result_count}, main={baseline.result_count}"
        )
    if (
        current.fid_signature != "unavailable"
        and baseline.fid_signature != "unavailable"
        and current.fid_signature != baseline.fid_signature
    ):
        failures.append(f"{prefix}: FID signature differs between current and main")
    return failures


def run_case(
    runner: Path,
    dataset: Dataset,
    coverage: str,
    reference: str,
    mode: str,
    trials: int,
    output_dir: Path,
) -> Sample:
    env = os.environ.copy()
    env.update(
        {
            "FAST_GDB_RUN_SPATIAL_BENCHMARKS": "1",
            "FAST_GDB_BENCHMARK_PATH": str(dataset.path.resolve()),
            "FAST_GDB_BENCHMARK_LABEL": (
                f"{reference} / {dataset.label} / 10m / {mode} / {coverage}"
            ),
            "FAST_GDB_BENCHMARK_CASE": coverage,
            "FAST_GDB_BENCHMARK_TRIALS": str(trials),
            "FAST_GDB_BENCHMARK_MODE": mode,
        }
    )
    # This script supports fresh-open but does not claim strict-cold execution.
    for name in (
        "FAST_GDB_BENCHMARK_STRICT_COLD",
        "FAST_GDB_BENCHMARK_CACHE_CLEAR_COMMAND",
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
    log_path = output_dir / (
        f"{safe_name(dataset.label)}-{reference}-10m-{mode}-{coverage}.log"
    )
    log_path.write_text(completed.stdout, encoding="utf-8")
    sys.stdout.write(completed.stdout)
    if completed.returncode != 0:
        raise RuntimeError(
            f"{dataset.label}/{reference}/{coverage} failed with exit code "
            f"{completed.returncode}; see {log_path}"
        )

    matches = list(SUMMARY_RE.finditer(completed.stdout))
    if len(matches) != 1:
        raise RuntimeError(
            f"expected one summary row for {dataset.label}/{reference}/{coverage}, "
            f"found {len(matches)}"
        )
    summary = matches[0]
    if summary.group(1) != coverage:
        raise RuntimeError(
            f"expected {coverage}, parsed {summary.group(1)} for "
            f"{dataset.label}/{reference}"
        )

    funnel_matches = list(FUNNEL_RE.finditer(completed.stdout))
    if len(funnel_matches) != 1:
        raise RuntimeError(
            f"expected one funnel row for {dataset.label}/{reference}/{coverage}, "
            f"found {len(funnel_matches)}"
        )
    invalid = int(funnel_matches[0].group(1))
    result_count = int(funnel_matches[0].group(2))
    if invalid != 0:
        raise RuntimeError(
            f"invalid geometries observed for {dataset.label}/{reference}/{coverage}: "
            f"{invalid}"
        )

    fid_matches = list(FID_RE.finditer(completed.stdout))
    fid_signature = fid_matches[-1].group(1).strip() if fid_matches else "unavailable"

    return Sample(
        dataset=dataset.label,
        reference=reference,
        mode=mode,
        coverage=coverage,
        fast_median_ms=float(summary.group(2)),
        gdal_median_ms=float(summary.group(3)),
        fast_p95_ms=float(summary.group(4)),
        gdal_p95_ms=float(summary.group(5)),
        fast_gdal_ratio=float(summary.group(6)),
        invalid_geometries=invalid,
        result_count=result_count,
        fid_signature=fid_signature,
        log=str(log_path),
    )


def write_environment(output: Path, args: argparse.Namespace, datasets: list[Dataset]) -> None:
    environment = {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": sys.version,
        "mode": args.mode,
        "strict_cold": False,
        "trials": args.trials,
        "max_regression": args.max_regression,
        "current_build": str(args.current_build.resolve()),
        "baseline_build": str(args.baseline_build.resolve()),
        "datasets": [
            {"label": dataset.label, "path": str(dataset.path.resolve())}
            for dataset in datasets
        ],
    }
    (output / "environment.json").write_text(
        json.dumps(environment, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--current-build", required=True, type=Path)
    parser.add_argument("--baseline-build", required=True, type=Path)
    parser.add_argument(
        "--dataset",
        action="append",
        type=parse_dataset,
        help="repeatable LABEL=PATH dataset specification",
    )
    # Backward-compatible single-dataset argument.
    parser.add_argument("--gdb", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--mode", choices=("steady-state", "fresh-open"), default="steady-state")
    parser.add_argument("--trials", type=int, default=20)
    parser.add_argument("--max-regression", type=float, default=0.05)
    args = parser.parse_args()

    if not 1 <= args.trials <= 100:
        parser.error("--trials must be between 1 and 100")
    if args.max_regression < 0:
        parser.error("--max-regression must be non-negative")
    if args.dataset and args.gdb:
        parser.error("use either --dataset or --gdb, not both")
    datasets = list(args.dataset or [])
    if args.gdb:
        datasets.append(Dataset(label="default", path=args.gdb))
    if not datasets:
        parser.error("at least one --dataset LABEL=PATH or --gdb is required")
    labels = [dataset.label for dataset in datasets]
    if len(labels) != len(set(labels)):
        parser.error("dataset labels must be unique")
    for dataset in datasets:
        if not dataset.path.is_dir():
            parser.error(f"GDB directory does not exist: {dataset.path}")

    current_runner = resolve_runner(args.current_build)
    baseline_runner = resolve_runner(args.baseline_build)
    samples: list[Sample] = []
    failures: list[str] = []
    args.output.mkdir(parents=True, exist_ok=True)
    write_environment(args.output, args, datasets)

    for dataset_index, dataset in enumerate(datasets):
        current_by_coverage: dict[str, Sample] = {}
        baseline_by_coverage: dict[str, Sample] = {}
        for coverage_index, coverage in enumerate(COVERAGES):
            # Alternate both within and across datasets to reduce one-way bias.
            current_first = (dataset_index + coverage_index) % 2 == 0
            order = (
                (("current", current_runner), ("main", baseline_runner))
                if current_first
                else (("main", baseline_runner), ("current", current_runner))
            )
            for reference, runner in order:
                sample = run_case(
                    runner,
                    dataset,
                    coverage,
                    reference,
                    args.mode,
                    args.trials,
                    args.output,
                )
                samples.append(sample)
                if reference == "current":
                    current_by_coverage[coverage] = sample
                else:
                    baseline_by_coverage[coverage] = sample

        for coverage in COVERAGES:
            failures.extend(
                compare_samples(
                    current_by_coverage[coverage],
                    baseline_by_coverage[coverage],
                    args.max_regression,
                )
            )

    csv_path = args.output / f"posix-10m-{args.mode}-main-regression.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=Sample.__dataclass_fields__.keys())
        writer.writeheader()
        for sample in samples:
            writer.writerow(asdict(sample))

    json_path = args.output / f"posix-10m-{args.mode}-main-regression.json"
    json_path.write_text(
        json.dumps(
            {
                "status": "fail" if failures else "pass",
                "mode": args.mode,
                "strict_cold": False,
                "samples": [asdict(sample) for sample in samples],
                "failures": failures,
            },
            indent=2,
            ensure_ascii=False,
        )
        + "\n",
        encoding="utf-8",
    )

    if failures:
        raise RuntimeError(
            "10M spatial regression gate failed:\n" + "\n".join(failures)
        )
    print(f"POSIX 10M {args.mode} main regression gate passed: {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
