import hashlib
import json
import sys
import tempfile
import threading
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import patch


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tools.learning.course_core import CourseEngine, CourseError  # noqa: E402


class CourseProgressTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.now = datetime(2026, 8, 9, 9, 0, tzinfo=timezone.utc)
        self.engine = CourseEngine(
            REPO_ROOT,
            Path(self.temporary.name),
            clock=lambda: self.now,
        )
        answer_path = REPO_ROOT / "tools/learning/answer_key.json"
        self.answers = json.loads(answer_path.read_text(encoding="utf-8"))

    def _complete_gate(self, module_id: str) -> None:
        module_answers = self.answers["modules"][module_id]
        module = self.engine.manifest.modules[module_id]
        lab_id = module.labs[0]["id"]
        self.engine.submit_answers(module_id, "quiz", module_answers["quiz"])
        levels = "、".join(module.evidence_task["required_levels"])
        self.engine.submit_evidence(
            module_id,
            f"{levels} 支撑当前模块结论；这些证据不能证明所有 FileGDB 版本和 ArcGIS 行为。",
        )
        self.engine.run_lab(module_id, lab_id)
        session = self.engine.create_oral_review_session(module_id)
        self.engine.import_oral_review(
            module_id,
            {
                "module_id": module_id,
                "review_session": session["review_session"],
                "scores": {
                    "structure": 3,
                    "accuracy": 3,
                    "evidence": 2,
                    "boundaries": 2,
                },
                "questions": ["问题一", "问题二", "问题三"],
                "gaps": [],
                "status": "pass",
                "reviewed_at": self.now.isoformat(),
            },
        )

    def _complete_m01_gate(self) -> None:
        self._complete_gate("M01")

    def test_initial_state_respects_prerequisites_and_delivery_state(self) -> None:
        self.assertEqual("NotStarted", self.engine.module_status("M01")["state"])
        self.assertEqual("Locked", self.engine.module_status("M02")["state"])
        self.assertEqual("Locked", self.engine.module_status("M04")["state"])

    def test_module_becomes_provisional_only_after_all_gate_evidence(self) -> None:
        self._complete_m01_gate()

        self.assertEqual("Provisional", self.engine.module_status("M01")["state"])
        self.assertEqual("NotStarted", self.engine.module_status("M02")["state"])
        self.assertTrue((Path(self.temporary.name) / "progress.json").is_file())
        artifact = Path(self.temporary.name) / "artifacts/M01/M01-L01.txt"
        self.assertTrue(artifact.is_file())
        evidence = Path(self.temporary.name) / "artifacts/M01/M01-E01.md"
        self.assertTrue(evidence.is_file())

    def test_failed_critical_question_blocks_provisional(self) -> None:
        answers = dict(self.answers["modules"]["M01"]["quiz"])
        answers["M01-Q01"] = "D"
        self.engine.submit_answers("M01", "quiz", answers)

        status = self.engine.module_status("M01")
        self.assertEqual("Learning", status["state"])
        self.assertFalse(status["gate"]["quiz_passed"])

    def test_standard_view_is_recorded_without_being_a_mastery_gate(self) -> None:
        view_id = self.engine.manifest.modules["M01"].standard_views[0]["id"]

        result = self.engine.record_standard_view("M01", view_id)

        self.assertTrue(result["viewed"])
        status = self.engine.module_status("M01")
        self.assertIn(view_id, status["standard_views"])
        self.assertNotIn("interactives_passed", status["gate"])

    def test_spaced_reviews_are_required_for_mastery(self) -> None:
        self._complete_m01_gate()
        review_answers = self.answers["modules"]["M01"]["reviews"]

        with self.assertRaises(CourseError):
            self.engine.submit_review("M01", 2, review_answers["2"])

        for day in (2, 7, 21):
            self.now = datetime(2026, 8, 9, 9, 0, tzinfo=timezone.utc) + timedelta(
                days=day
            )
            self.engine.submit_review("M01", day, review_answers[str(day)])

        self.assertEqual("Mastered", self.engine.module_status("M01")["state"])

    def test_artifact_paths_cannot_escape_state_directory(self) -> None:
        with self.assertRaises(CourseError):
            self.engine.artifact_path("../../outside.txt")

    def test_evidence_lab_inputs_cannot_escape_whitelisted_roots(self) -> None:
        fixture = self.engine._fixture_copy()
        with self.assertRaises(CourseError):
            self.engine._resolve_lab_input(
                fixture,
                {"scope": "repo", "path": "../../etc/passwd", "mode": "text"},
            )

    def test_evidence_requires_declared_levels_and_boundary(self) -> None:
        with self.assertRaises(CourseError):
            self.engine.submit_evidence(
                "M01", "这是一段足够长但没有证据等级和边界的填充文本。"
            )

    def test_review_cannot_bypass_locked_prerequisite(self) -> None:
        self.engine.progress["modules"]["M02"] = {
            "review_due": {"2": self.now.isoformat()}
        }
        with self.assertRaisesRegex(CourseError, "先修模块"):
            self.engine.submit_review("M02", 2, {})

    def test_tampered_evidence_artifact_closes_gate(self) -> None:
        self._complete_m01_gate()
        artifact = Path(self.temporary.name) / "artifacts/M01/M01-E01.md"
        artifact.write_text("tampered", encoding="utf-8")

        status = self.engine.module_status("M01")

        self.assertEqual("Learning", status["state"])
        self.assertFalse(status["gate"]["evidence_submitted"])

    def test_core_fixture_matches_versioned_provenance_manifest(self) -> None:
        fixture = self.engine.fixture_status()

        self.assertTrue(fixture["verified"], fixture)
        self.assertEqual(111, fixture["file_count"])
        self.assertLess(fixture["size_bytes"], 2 * 1024 * 1024)

    def test_parallel_cli_and_server_progress_updates_are_merged(self) -> None:
        second_engine = CourseEngine(
            REPO_ROOT,
            Path(self.temporary.name),
            clock=lambda: self.now,
        )
        answers = self.answers["modules"]["M01"]["quiz"]

        self.engine.submit_answers("M01", "quiz", answers)
        second_engine.run_lab("M01", "M01-L01")

        status = self.engine.module_status("M01")
        self.assertTrue(status["gate"]["quiz_passed"])
        self.assertTrue(status["gate"]["labs_passed"])

    def test_same_engine_mutations_wait_for_transaction_lock(self) -> None:
        finished = threading.Event()

        def submit() -> None:
            self.engine.submit_answers(
                "M01", "quiz", self.answers["modules"]["M01"]["quiz"]
            )
            finished.set()

        with self.engine._transaction_lock:
            worker = threading.Thread(target=submit)
            worker.start()
            self.assertFalse(finished.wait(0.05))
        worker.join(timeout=2)

        self.assertTrue(finished.is_set())

    def test_status_reads_wait_for_transaction_lock(self) -> None:
        finished = threading.Event()

        def read_status() -> None:
            self.engine.module_status("M01")
            finished.set()

        with self.engine._transaction_lock:
            worker = threading.Thread(target=read_status)
            worker.start()
            self.assertFalse(finished.wait(0.05))
        worker.join(timeout=2)

        self.assertTrue(finished.is_set())

    def test_stale_working_fixture_is_replaced_before_lab(self) -> None:
        self.engine.run_lab("M01", "M01-L01")
        copied = Path(self.temporary.name) / "fixtures/acceptance_metadata.gdb"
        table = copied / "a00000001.gdbtable"
        table.write_bytes(b"corrupt")

        self.engine.run_lab("M01", "M01-L01")

        source = REPO_ROOT / "test_data/gdb/acceptance_metadata.gdb/a00000001.gdbtable"
        self.assertEqual(source.read_bytes(), table.read_bytes())

    def test_progress_write_keeps_previous_version_in_backups(self) -> None:
        answers = self.answers["modules"]["M01"]["diagnostic"]
        self.engine.submit_answers("M01", "diagnostic", answers)
        self.engine.submit_evidence(
            "M01",
            "EsriConfirmed 和 FastGdbConfirmed 只覆盖当前语义；Unknown 不能证明全部 ArcGIS 版本。",
        )

        backups = list((Path(self.temporary.name) / "backups").glob("progress-*.json"))
        self.assertTrue(backups)

    def test_preexisting_unlocked_lock_file_does_not_block_progress(self) -> None:
        lock = Path(self.temporary.name) / ".progress.lock"
        lock.parent.mkdir(parents=True, exist_ok=True)
        lock.write_text("stale", encoding="utf-8")

        self.engine.submit_answers(
            "M01", "diagnostic", self.answers["modules"]["M01"]["diagnostic"]
        )

        self.assertEqual("Learning", self.engine.module_status("M01")["state"])

    def test_lab_result_is_checked_against_versioned_answer_key(self) -> None:
        expected = self.engine.answer_key["modules"]["M02"]["labs"]["M02-L01"]
        original = expected["prefix_sha256"]
        expected["prefix_sha256"] = hashlib.sha256(b"wrong").hexdigest()
        self._complete_gate("M01")

        with self.assertRaises(CourseError):
            self.engine.run_lab("M02", "M02-L01")

        expected["prefix_sha256"] = original

    def test_lab_rejects_empty_runner_output(self) -> None:
        self._complete_gate("M01")

        with patch.object(self.engine, "_run_hex_lab", return_value=""):
            with self.assertRaises(CourseError):
                self.engine.run_lab("M02", "M02-L01")

    def test_all_eighteen_modules_complete_without_manual_paths(self) -> None:
        for module_id in self.engine.manifest.modules:
            self._complete_gate(module_id)
            self.assertEqual(
                "Provisional", self.engine.module_status(module_id)["state"]
            )

        hex_artifact = Path(self.temporary.name) / "artifacts/M02/M02-L01.txt"
        inventory_artifact = Path(self.temporary.name) / "artifacts/M03/M03-L01.txt"
        self.assertIn("00000000", hex_artifact.read_text(encoding="utf-8"))
        self.assertIn(
            "a00000001.gdbtable", inventory_artifact.read_text(encoding="utf-8")
        )


if __name__ == "__main__":
    unittest.main()
