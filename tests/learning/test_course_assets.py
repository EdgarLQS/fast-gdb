import re
import sys
import unittest
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlparse


REPO_ROOT = Path(__file__).resolve().parents[2]
LEARNING_ROOT = REPO_ROOT / "docs/gdb/learning"
sys.path.insert(0, str(REPO_ROOT))

from tools.learning.course_core import load_course_data, load_manifest  # noqa: E402


class LinkCollector(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.links = []
        self.ids = set()

    def handle_starttag(self, tag: str, attributes) -> None:
        values = dict(attributes)
        if values.get("id"):
            self.ids.add(values["id"])
        for name in ("href", "src"):
            if values.get(name):
                self.links.append(values[name])


def parse_html(path: Path) -> LinkCollector:
    parser = LinkCollector()
    parser.feed(path.read_text(encoding="utf-8"))
    return parser


class CourseAssetsTest(unittest.TestCase):
    def test_learning_markdown_local_links_resolve(self) -> None:
        missing = []
        markdown_files = list(LEARNING_ROOT.rglob("*.md"))
        markdown_files.append(REPO_ROOT / "docs/plan/04_FileGDB数据结构完整学习路线.md")
        for markdown_path in markdown_files:
            text = markdown_path.read_text(encoding="utf-8")
            for link in re.findall(r"\[[^\]]+\]\(([^)]+)\)", text):
                target_text = link.split("#", 1)[0]
                parsed = urlparse(target_text)
                if not target_text or parsed.scheme:
                    continue
                target = (markdown_path.parent / unquote(target_text)).resolve()
                if not target.exists():
                    missing.append(f"{markdown_path.relative_to(REPO_ROOT)} -> {link}")
        self.assertEqual([], missing)

    def test_all_learning_html_local_links_resolve(self) -> None:
        missing = []
        for html_path in LEARNING_ROOT.rglob("*.html"):
            parser = parse_html(html_path)
            for link in parser.links:
                parsed = urlparse(link)
                if parsed.scheme or link.startswith("#"):
                    continue
                target = (html_path.parent / unquote(parsed.path)).resolve()
                if not target.exists():
                    missing.append(f"{html_path.relative_to(REPO_ROOT)} -> {link}")
        self.assertEqual([], missing)

    def test_all_micro_units_have_real_lesson_anchors(self) -> None:
        manifest = load_manifest(LEARNING_ROOT / "course.json")
        for module_id in manifest.modules:
            module = manifest.modules[module_id]
            parser = parse_html(LEARNING_ROOT / module.lesson)
            for unit in module.micro_units:
                self.assertIn(unit["id"], parser.ids)

    def test_public_course_manifest_contains_no_manual_activity_keys(self) -> None:
        data = load_course_data(LEARNING_ROOT / "course.json")

        def keys(value):
            if isinstance(value, dict):
                for key, child in value.items():
                    yield key
                    yield from keys(child)
            elif isinstance(value, list):
                for child in value:
                    yield from keys(child)

        self.assertNotIn("interactives", set(keys(data)))

    def test_standard_views_have_renderable_answer_payloads(self) -> None:
        data = load_course_data(LEARNING_ROOT / "course.json")
        for module in data["modules"]:
            self.assertTrue(module["standard_views"])
            question_ids = {
                question["id"]
                for question in (
                    *module["diagnostic_questions"],
                    *module["quiz_questions"],
                )
            }
            self.assertEqual(question_ids, set(module["question_standard_views"]))
            for view in module["standard_views"]:
                self.assertIn(view["kind"], {"graph", "bytes", "file-family", "table"})
                self.assertTrue(view["standard_answer"])
                self.assertTrue(view["sources"])

    def test_local_standard_answer_sources_resolve(self) -> None:
        data = load_course_data(LEARNING_ROOT / "course.json")
        missing = []
        for module in data["modules"]:
            for view in module["standard_views"]:
                for source in view["sources"]:
                    parsed = urlparse(source["href"])
                    if parsed.scheme:
                        continue
                    target = (LEARNING_ROOT / unquote(parsed.path)).resolve()
                    if not target.exists():
                        missing.append(f'{module["id"]}/{view["id"]} -> {source["href"]}')
        self.assertEqual([], missing)

    def test_browser_client_requires_no_session_token(self) -> None:
        script = (LEARNING_ROOT / "assets/course-app.js").read_text(encoding="utf-8")
        self.assertNotIn("X-Course-Token", script)
        self.assertNotIn("fast-gdb-course-token", script)
        self.assertNotIn("readToken", script)


if __name__ == "__main__":
    unittest.main()
