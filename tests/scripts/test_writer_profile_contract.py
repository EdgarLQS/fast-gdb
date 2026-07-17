#!/usr/bin/env python3

from __future__ import annotations

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MINIMUM_XYZM_PROFILE_ROWS = 20_000_000


class WriterProfileContractTest(unittest.TestCase):
    def test_xyzm_profile_workload_is_long_enough_and_consistent(self) -> None:
        manifest = json.loads(
            (ROOT / "tests/contracts/writer-macos-performance-v1.json").read_text(
                encoding="utf-8"
            )
        )
        scenario = next(
            item
            for item in manifest["profile"]["scenarios"]
            if item["id"] == "P0-POINT-XYZM-PROFILE"
        )
        self.assertGreaterEqual(scenario["rows"], MINIMUM_XYZM_PROFILE_ROWS)

        expected = f"--rows {scenario['rows']}"
        workflow = (
            ROOT / ".github/workflows/writer-macos-performance.yml"
        ).read_text(encoding="utf-8")
        guide = (ROOT / "docs/usage/09_本地验收说明.md").read_text(
            encoding="utf-8"
        )
        self.assertIn(expected, workflow)
        self.assertIn(expected, guide)


if __name__ == "__main__":
    unittest.main()
