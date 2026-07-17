#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "run_spatial_regression.py"
SPEC = importlib.util.spec_from_file_location("run_spatial_regression", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class RunSpatialRegressionTest(unittest.TestCase):
    def test_parse_dataset(self) -> None:
        dataset = MODULE.parse_dataset("Point=/tmp/point.gdb")
        self.assertEqual(dataset.label, "Point")
        self.assertEqual(dataset.path, Path("/tmp/point.gdb"))

    def test_parse_dataset_trims_whitespace(self) -> None:
        dataset = MODULE.parse_dataset(" Point = /tmp/point.gdb ")
        self.assertEqual(dataset.label, "Point")
        self.assertEqual(dataset.path, Path("/tmp/point.gdb"))

    def test_parse_dataset_rejects_missing_label(self) -> None:
        with self.assertRaises(Exception):
            MODULE.parse_dataset("=/tmp/point.gdb")

    def test_parse_dataset_rejects_missing_path(self) -> None:
        with self.assertRaises(Exception):
            MODULE.parse_dataset("Point=")

    def test_parse_dataset_rejects_missing_separator(self) -> None:
        with self.assertRaises(Exception):
            MODULE.parse_dataset("/tmp/point.gdb")

    def test_safe_name(self) -> None:
        self.assertEqual(MODULE.safe_name("Point / 10M fresh-open"), "Point-10M-fresh-open")
        self.assertEqual(MODULE.safe_name("***"), "dataset")

    def test_compare_samples_reports_regression_and_correctness_drift(self) -> None:
        current = MODULE.Sample(
            dataset="Point",
            reference="current",
            mode="fresh-open",
            coverage="coverage_10pct",
            fast_median_ms=110.0,
            gdal_median_ms=50.0,
            fast_p95_ms=120.0,
            gdal_p95_ms=60.0,
            fast_gdal_ratio=2.2,
            invalid_geometries=0,
            result_count=99,
            fid_signature="current-hash",
            log="current.log",
        )
        baseline = MODULE.Sample(
            dataset="Point",
            reference="main",
            mode="fresh-open",
            coverage="coverage_10pct",
            fast_median_ms=100.0,
            gdal_median_ms=50.0,
            fast_p95_ms=110.0,
            gdal_p95_ms=60.0,
            fast_gdal_ratio=2.0,
            invalid_geometries=0,
            result_count=100,
            fid_signature="main-hash",
            log="main.log",
        )
        failures = MODULE.compare_samples(current, baseline, 0.05)
        self.assertEqual(len(failures), 3)
        self.assertTrue(any("limit=105.0ms" in failure for failure in failures))
        self.assertTrue(any("result count differs" in failure for failure in failures))
        self.assertTrue(any("FID signature differs" in failure for failure in failures))

    def test_compare_samples_accepts_missing_optional_fid_signature(self) -> None:
        sample = MODULE.Sample(
            dataset="Point",
            reference="current",
            mode="steady-state",
            coverage="coverage_01pct",
            fast_median_ms=100.0,
            gdal_median_ms=50.0,
            fast_p95_ms=110.0,
            gdal_p95_ms=60.0,
            fast_gdal_ratio=2.0,
            invalid_geometries=0,
            result_count=100,
            fid_signature="unavailable",
            log="sample.log",
        )
        baseline = MODULE.Sample(**{**sample.__dict__, "reference": "main"})
        self.assertEqual(MODULE.compare_samples(sample, baseline, 0.05), [])

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
