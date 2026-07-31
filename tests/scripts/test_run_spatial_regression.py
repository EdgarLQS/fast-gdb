#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = (
    Path(__file__).resolve().parents[2]
    / "tools"
    / "ci"
    / "run_spatial_regression.py"
)
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

    def test_parse_sha_accepts_commit_and_rejects_weak_prefix(self) -> None:
        self.assertEqual(MODULE.parse_sha(" A1B2C3D "), "a1b2c3d")
        with self.assertRaises(Exception):
            MODULE.parse_sha("abc123")

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
            fid_verification="full-vector-gdal-equality",
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
            fid_verification="full-vector-gdal-equality",
            log="main.log",
        )
        failures = MODULE.compare_samples(current, baseline, 0.05)
        self.assertEqual(len(failures), 3)
        self.assertTrue(any("limit=105.0ms" in failure for failure in failures))
        self.assertTrue(any("result count differs" in failure for failure in failures))
        self.assertTrue(any("FID signature differs" in failure for failure in failures))

    def test_compare_samples_accepts_full_vector_verification_without_signature(self) -> None:
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
            fid_signature="",
            fid_verification="full-vector-gdal-equality",
            log="sample.log",
        )
        baseline = MODULE.Sample(**{**sample.__dict__, "reference": "main"})
        self.assertEqual(MODULE.compare_samples(sample, baseline, 0.05), [])

    def test_compare_samples_rejects_missing_fid_verification(self) -> None:
        sample = MODULE.Sample(
            dataset="Point", reference="current", mode="fresh-open",
            coverage="coverage_01pct", fast_median_ms=10.0,
            gdal_median_ms=20.0, fast_p95_ms=11.0, gdal_p95_ms=21.0,
            fast_gdal_ratio=0.5, invalid_geometries=0, result_count=10,
            fid_signature="", fid_verification="", log="sample.log",
        )
        baseline = MODULE.Sample(**{
            **sample.__dict__, "reference": "main",
            "fid_verification": "full-vector-gdal-equality",
        })
        self.assertTrue(
            any(
                "current FID verification is missing" in failure
                for failure in MODULE.compare_samples(sample, baseline, 0.05)
            )
        )

    def test_validate_builds_rejects_same_runner(self) -> None:
        current = self._provenance("/tmp/runner", "a" * 40)
        baseline = self._provenance("/tmp/runner", "b" * 40)
        with self.assertRaisesRegex(ValueError, "same runner"):
            MODULE.validate_builds(current, baseline, None, None)

    def test_validate_builds_rejects_same_sha(self) -> None:
        current = self._provenance("/tmp/current", "a" * 40)
        baseline = self._provenance("/tmp/main", "a" * 40)
        with self.assertRaisesRegex(ValueError, "SHA must differ"):
            MODULE.validate_builds(current, baseline, None, None)

    def test_validate_builds_accepts_distinct_clean_expected_shas(self) -> None:
        current = self._provenance("/tmp/current", "a" * 40)
        baseline = self._provenance("/tmp/main", "b" * 40)
        MODULE.validate_builds(current, baseline, "a" * 7, "b" * 7)

    def test_validate_builds_rejects_expected_sha_mismatch(self) -> None:
        current = self._provenance("/tmp/current", "a" * 40)
        baseline = self._provenance("/tmp/main", "b" * 40)
        with self.assertRaisesRegex(ValueError, "current source SHA mismatch"):
            MODULE.validate_builds(current, baseline, "c0ffee", None)

    def test_validate_builds_rejects_dirty_source(self) -> None:
        current = MODULE.BuildProvenance(**{
            **self._provenance("/tmp/current", "a" * 40).__dict__,
            "source_dirty": True,
        })
        baseline = self._provenance("/tmp/main", "b" * 40)
        with self.assertRaisesRegex(ValueError, "must be clean"):
            MODULE.validate_builds(current, baseline, None, None)

    def test_validate_dataset_labels_rejects_sanitized_collision(self) -> None:
        datasets = [
            MODULE.Dataset("A/B", Path("/tmp/a")),
            MODULE.Dataset("A-B", Path("/tmp/b")),
        ]
        with self.assertRaisesRegex(ValueError, "filename sanitization"):
            MODULE.validate_dataset_labels(datasets)

    def test_validate_dataset_labels_rejects_case_insensitive_collision(self) -> None:
        datasets = [
            MODULE.Dataset("Point", Path("/tmp/a")),
            MODULE.Dataset("point", Path("/tmp/b")),
        ]
        with self.assertRaisesRegex(ValueError, "filename sanitization"):
            MODULE.validate_dataset_labels(datasets)

    @staticmethod
    def _provenance(runner: str, sha: str):
        return MODULE.BuildProvenance(
            build="/tmp/build", runner=runner, source="/tmp/source",
            source_sha=sha, source_dirty=False, compiler="clang", gdal="3.13",
        )

    def test_inspect_build_records_cache_and_git_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            runner = build / "gdb_tutorial_test_runner"
            runner.touch()
            (build / "CMakeCache.txt").write_text(
                "CMAKE_HOME_DIRECTORY:INTERNAL=/tmp/source\n"
                "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++\n"
                "FIND_PACKAGE_MESSAGE_DETAILS_GDAL:INTERNAL=[v3.13.0()]\n",
                encoding="utf-8",
            )
            with mock.patch.object(
                MODULE, "git_value", side_effect=("a" * 40, " M file")
            ) as git_value:
                result = MODULE.inspect_build(build, runner)
        self.assertEqual(result.source_sha, "a" * 40)
        self.assertTrue(result.source_dirty)
        self.assertEqual(result.compiler, "/usr/bin/clang++")
        self.assertEqual(
            git_value.call_args_list[-1].args[1:],
            ("status", "--porcelain", "--untracked-files=normal"),
        )

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
