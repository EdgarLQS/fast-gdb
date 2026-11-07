#!/usr/bin/env python3
"""Measure repeated independent test-runner processes against one read-only GDB."""

import argparse
import json
import math
import os
import subprocess
import time
from pathlib import Path


def percentile(samples, fraction):
    ordered = sorted(samples)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--dataset", required=True, type=Path)
    parser.add_argument("--gtest-filter", required=True)
    parser.add_argument("--rounds", type=int, default=25)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")

    command = [
        str(args.runner),
        f"--gtest_filter={args.gtest_filter}",
        "--gtest_brief=1",
    ]
    env = os.environ.copy()
    env["FAST_GDB_CURVE_DATASET"] = str(args.dataset)
    samples_ms = []

    for iteration in range(args.rounds):
        start = time.perf_counter()
        result = subprocess.run(command, env=env, capture_output=True, text=True)
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        output = result.stdout + result.stderr
        if result.returncode != 0 or "[  PASSED  ] 1 test." not in output:
            raise RuntimeError(
                f"round {iteration + 1} did not pass\n"
                f"return code: {result.returncode}\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )
        samples_ms.append(round(elapsed_ms, 3))

    report = {
        "runner": str(args.runner),
        "dataset": str(args.dataset),
        "gtest_filter": args.gtest_filter,
        "rounds": args.rounds,
        "all_passed": True,
        "median_ms": percentile(samples_ms, 0.50),
        "p95_ms": percentile(samples_ms, 0.95),
        "p99_ms": percentile(samples_ms, 0.99),
        "min_ms": min(samples_ms),
        "max_ms": max(samples_ms),
        "samples_ms": samples_ms,
        "filesystem_cache": "not flushed; cache state uncontrolled",
    }
    serialized = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(serialized, encoding="utf-8")
    else:
        print(serialized, end="")


if __name__ == "__main__":
    main()
