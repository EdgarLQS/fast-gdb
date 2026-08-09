import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tools.learning.course_core import (  # noqa: E402
    ManifestError,
    load_course_data,
    load_manifest,
)


class CourseManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.manifest_path = REPO_ROOT / "docs/gdb/learning/course.json"

    def test_manifest_defines_eighteen_acyclic_modules(self) -> None:
        manifest = load_manifest(self.manifest_path)

        self.assertEqual(18, len(manifest.modules))
        self.assertEqual([f"M{i:02d}" for i in range(1, 19)], list(manifest.modules))
        self.assertEqual({"current"}, {item.delivery_state for item in manifest.modules.values()})
        self.assertEqual(("M14",), manifest.modules["M15"].prerequisites)
        self.assertEqual(("M14",), manifest.modules["M16"].prerequisites)
        self.assertEqual(("M14",), manifest.modules["M17"].prerequisites)
        self.assertEqual(
            ("M15", "M16", "M17"), manifest.modules["M18"].prerequisites
        )

    def test_all_modules_define_complete_learning_loop(self) -> None:
        manifest = load_manifest(self.manifest_path)

        for module_id in manifest.modules:
            module = manifest.modules[module_id]
            self.assertGreaterEqual(len(module.diagnostic_questions), 5)
            self.assertGreaterEqual(len(module.micro_units), 3)
            self.assertGreaterEqual(len(module.standard_views), 1)
            question_ids = {
                question["id"]
                for question in (*module.diagnostic_questions, *module.quiz_questions)
            }
            self.assertEqual(question_ids, set(module.question_standard_views))
            self.assertTrue(
                set(module.question_standard_views.values())
                <= {view["id"] for view in module.standard_views}
            )
            for view in module.standard_views:
                self.assertTrue(view["id"])
                self.assertTrue(view["title"])
                self.assertTrue(view["summary"])
                self.assertTrue(view["standard_answer"])
                self.assertTrue(view["evidence"])
                self.assertIn("not_proven", view)
                self.assertIn("common_mistakes", view)
            self.assertTrue(module.labs)
            self.assertTrue(module.evidence_task)
            self.assertTrue(module.evidence_task["required_levels"])
            self.assertTrue(module.oral_review)
            self.assertEqual((2, 7, 21), module.review_days)
            self.assertEqual(3, len(module.review_tasks))

    def test_manifest_rejects_unknown_prerequisite(self) -> None:
        data = self.manifest_path.read_text(encoding="utf-8")
        broken = data.replace('"prerequisites": ["M01"]', '"prerequisites": ["M99"]', 1)
        with tempfile.TemporaryDirectory() as temporary_dir:
            temporary_root = Path(temporary_dir)
            shutil.copytree(self.manifest_path.parent / "modules", temporary_root / "modules")
            temporary = temporary_root / "course.json"
            temporary.write_text(broken, encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "未知先修模块"):
                load_manifest(temporary)

    def test_module_files_are_merged_in_declared_order(self) -> None:
        data = load_course_data(self.manifest_path)

        self.assertEqual(18, len(data["modules"]))
        self.assertEqual([f"M{i:02d}" for i in range(1, 19)], [m["id"] for m in data["modules"]])

    def test_all_answer_keys_match_manifest_question_and_lab_ids(self) -> None:
        manifest = load_manifest(self.manifest_path)
        answer_path = REPO_ROOT / "tools/learning/answer_key.json"
        answer_key = json.loads(answer_path.read_text(encoding="utf-8"))["modules"]

        for module_id in manifest.modules:
            module = manifest.modules[module_id]
            self.assertEqual(
                {item["id"] for item in module.diagnostic_questions},
                set(answer_key[module_id]["diagnostic"]),
            )
            self.assertEqual(
                {item["id"] for item in module.quiz_questions},
                set(answer_key[module_id]["quiz"]),
            )
            self.assertEqual(
                {item["id"] for item in module.labs},
                set(answer_key[module_id]["labs"]),
            )
            quiz_options = {
                question["id"]: {option["id"] for option in question["options"]}
                for question in module.quiz_questions
            }
            for question_id, answer in answer_key[module_id]["quiz"].items():
                self.assertIn(answer, quiz_options[question_id])
            for task in module.review_tasks:
                expected = answer_key[module_id]["reviews"][str(task["day"])]
                self.assertEqual(set(task["question_ids"]), set(expected))
                for question_id, answer in expected.items():
                    self.assertEqual(answer_key[module_id]["quiz"][question_id], answer)


if __name__ == "__main__":
    unittest.main()
