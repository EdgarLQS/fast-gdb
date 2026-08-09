import subprocess
import sys
import tempfile
import unittest
import json
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
COURSE_CLI = REPO_ROOT / "tools/learning/course.py"


class CourseCliTest(unittest.TestCase):
    def setUp(self) -> None:
        test_root = REPO_ROOT / "build/learning/test-runs"
        test_root.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=test_root)
        self.addCleanup(self.temporary.cleanup)
        self.state_dir = Path(self.temporary.name) / "state"

    def _run(self, *arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                str(COURSE_CLI),
                "--state-dir",
                str(self.state_dir),
                *arguments,
            ],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_status_reports_locked_modules(self) -> None:
        result = self._run("status")

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("M01  NotStarted", result.stdout)
        self.assertIn("M02  Locked", result.stdout)
        self.assertIn("M04  Locked", result.stdout)

    def test_review_export_creates_codex_prompt_inside_state_directory(self) -> None:
        result = self._run("review-export", "M01")

        self.assertEqual(0, result.returncode, result.stderr)
        prompt = self.state_dir / "reviews/M01-prompt.md"
        self.assertTrue(prompt.is_file())
        content = prompt.read_text(encoding="utf-8")
        self.assertIn("一次只问一个问题", content)
        self.assertIn('"module_id": "M01"', content)
        self.assertIn("structure", content)
        session = json.loads(
            (self.state_dir / "reviews/M01-session.json").read_text(encoding="utf-8")
        )
        self.assertIn(session["review_session"], content)

    def test_review_import_rejects_file_outside_review_directory(self) -> None:
        outside = self.state_dir.parent / "outside.json"
        outside.write_text("{}", encoding="utf-8")
        self.addCleanup(outside.unlink, missing_ok=True)

        result = self._run("review-import", "M01", str(outside))

        self.assertNotEqual(0, result.returncode)
        self.assertIn("必须位于", result.stderr)

    def test_review_import_requires_exported_session(self) -> None:
        review = self.state_dir / "reviews/M01-result.json"
        review.parent.mkdir(parents=True, exist_ok=True)
        review.write_text(
            json.dumps(
                {
                    "module_id": "M01",
                    "review_session": "invented",
                    "scores": {
                        "structure": 3,
                        "accuracy": 3,
                        "evidence": 2,
                        "boundaries": 2,
                    },
                    "questions": ["q1", "q2", "q3"],
                    "gaps": [],
                    "status": "pass",
                    "reviewed_at": "2026-08-09T09:00:00+00:00",
                }
            ),
            encoding="utf-8",
        )

        result = self._run("review-import", "M01", str(review))

        self.assertNotEqual(0, result.returncode)
        self.assertIn("口试会话", result.stderr)

    def test_state_directory_cannot_escape_build_learning(self) -> None:
        result = subprocess.run(
            [
                sys.executable,
                str(COURSE_CLI),
                "--state-dir",
                str(Path(self.temporary.name).parent.parent.parent / "outside"),
                "status",
            ],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
        )

        self.assertNotEqual(0, result.returncode)
        self.assertIn("build/learning", result.stderr)

    def test_lab_uses_single_stable_activity_identifier(self) -> None:
        result = self._run("lab", "M01-L01")

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertTrue((self.state_dir / "artifacts/M01/M01-L01.txt").is_file())

    def test_export_record_refuses_unmastered_module(self) -> None:
        result = self._run("export-record", "M01")

        self.assertNotEqual(0, result.returncode)
        self.assertIn("尚未达到 Mastered", result.stderr)


if __name__ == "__main__":
    unittest.main()
