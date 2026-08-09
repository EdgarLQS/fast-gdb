import json
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from tools.learning.course_core import CourseEngine  # noqa: E402
from tools.learning.course_server import create_server  # noqa: E402


class CourseServerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        engine = CourseEngine(REPO_ROOT, Path(self.temporary.name) / "state")
        self.server = create_server(engine, port=0)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        host, port = self.server.server_address
        self.base_url = f"http://{host}:{port}"

    def _request(self, path: str, origin: str = ""):
        headers = {}
        if origin:
            headers["Origin"] = origin
        return urllib.request.urlopen(
            urllib.request.Request(self.base_url + path, headers=headers), timeout=3
        )

    def _post(self, path: str, payload: dict):
        request = urllib.request.Request(
            self.base_url + path,
            data=json.dumps(payload).encode("utf-8"),
            method="POST",
            headers={"Content-Type": "application/json"},
        )
        return urllib.request.urlopen(request, timeout=3)

    def test_static_entrypoint_and_tokenless_api_are_available(self) -> None:
        with self._request("/") as response:
            self.assertEqual(200, response.status)
            self.assertIn("FileGDB 全面学习实验室", response.read().decode("utf-8"))

        with self._request("/api/course") as response:
            payload = json.loads(response.read().decode("utf-8"))
            self.assertEqual(18, len(payload["modules"]))
            self.assertTrue(payload["modules"][0]["standard_views"])
            self.assertIn("standard_answer", json.dumps(payload))
            question = payload["modules"][0]["diagnostic_questions"][0]
            self.assertEqual("A", question["answer_explanation"]["correct_option"])
            self.assertIn("带空间字段", question["answer_explanation"]["correct_text"])

    def test_api_rejects_foreign_origin(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as foreign:
            self._request("/api/progress", origin="https://evil.example")
        self.assertEqual(403, foreign.exception.code)

    def test_server_rejects_path_traversal_and_arbitrary_command_api(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as traversal:
            self._request("/%2e%2e/%2e%2e/etc/passwd")
        self.assertIn(traversal.exception.code, (403, 404))

        body = json.dumps({"command": "whoami"}).encode("utf-8")
        request = urllib.request.Request(
            self.base_url + "/api/command",
            data=body,
            method="POST",
            headers={"Content-Type": "application/json"},
        )
        with self.assertRaises(urllib.error.HTTPError) as command:
            urllib.request.urlopen(request, timeout=3)
        self.assertEqual(404, command.exception.code)

    def test_server_rejects_non_object_json(self) -> None:
        request = urllib.request.Request(
            self.base_url + "/api/evidence",
            data=b"[]",
            method="POST",
            headers={"Content-Type": "application/json"},
        )

        with self.assertRaises(urllib.error.HTTPError) as invalid:
            urllib.request.urlopen(request, timeout=3)

        self.assertEqual(400, invalid.exception.code)

    def test_answer_submission_updates_progress_through_public_api(self) -> None:
        answers = {
            "M01-D01": "A",
            "M01-D02": "B",
            "M01-D03": "C",
            "M01-D04": "B",
            "M01-D05": "A",
        }
        with self._post(
            "/api/answers",
            {"module_id": "M01", "kind": "diagnostic", "answers": answers},
        ) as response:
            payload = json.loads(response.read().decode("utf-8"))
        self.assertEqual(1.0, payload["result"]["score"])

        with self._request("/api/progress") as response:
            progress = json.loads(response.read().decode("utf-8"))
        self.assertEqual("Learning", progress["statuses"]["M01"]["state"])

    def test_standard_answer_view_is_recorded_and_manual_endpoint_is_removed(
        self,
    ) -> None:
        with self._post(
            "/api/standard-view", {"module_id": "M01", "view_id": "M01-S01"}
        ) as response:
            payload = json.loads(response.read().decode("utf-8"))
        self.assertTrue(payload["viewed"])
        self.assertIn("M01-S01", payload["status"]["standard_views"])

        with self.assertRaises(urllib.error.HTTPError) as removed:
            self._post(
                "/api/interactive",
                {"module_id": "M01", "activity_id": "M01-I01", "payload": {}},
            )
        self.assertEqual(404, removed.exception.code)


if __name__ == "__main__":
    unittest.main()
