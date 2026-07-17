#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "run_spatial_regression.py"
SPEC = importlib.util.spec_from_file_location("run_spatial_regression", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class RunSpatialRegressionTest(unittest.TestCase):
    def test_parse_dataset(self) -> None:
        dataset = MODULE.parse_dataset("Point=/tmp/point.gdb")
        self.assertEqual(dataset.label, "Point")
        self.assertEqual(dataset.path, Path("/tmp/point.gdb"))

    def test_parse_dataset_rejects_missing_label(self) -> None:
        with self.assertRaises(Exception):
            MODULE.parse_dataset("=/tmp/point.gdb")

    def test_parse_dataset_rejects_missing_separator(self) -> None:
        with self.assertRaises(Exception):
            MODULE.parse_dataset("/tmp/point.gdb")

    def test_safe_name(self) -> None:
        self.assertEqual(MODULE.safe_name("Point / 10M fresh-open"), "Point-10M-fresh-open")
        self.assertEqual(MODULE.safe_name("***"), "dataset")

    def test_coverages_are_stable(self) -> None:
        self.assertEqual(
            MODULE.COVERAGES,
            (
                "coverage_01pct",
                "coverage_10pct",
                "coverage_30pct",
                "coverage_80pct",
                "coverage_full",
            ),
        )


if __name__ == "__main__":
    unittest.main()
