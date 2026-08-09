"""仅监听本机回环地址的 FileGDB 学习服务。"""

from __future__ import annotations

import json
import mimetypes
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Callable
from urllib.parse import unquote, urlparse

from tools.learning.course_core import CourseEngine, CourseError


MAX_REQUEST_BYTES = 64 * 1024


class CourseRequestHandler(BaseHTTPRequestHandler):
    """只暴露课程静态资源和白名单学习动作。"""

    engine: CourseEngine
    server_version = "FastGdbLearning/1"

    def log_message(self, format_string: str, *arguments) -> None:
        return

    def _security_headers(self) -> None:
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self'; "
            "connect-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'",
        )

    def _send_bytes(self, status: int, payload: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self._security_headers()
        self.end_headers()
        self.wfile.write(payload)

    def _send_json(self, status: int, payload: dict) -> None:
        body = (json.dumps(payload, ensure_ascii=False) + "\n").encode("utf-8")
        self._send_bytes(status, body, "application/json; charset=utf-8")

    def _same_origin(self) -> bool:
        origin = self.headers.get("Origin")
        host, port = self.server.server_address
        return not origin or origin == f"http://{host}:{port}"

    def _require_api_access(self) -> bool:
        if self._same_origin():
            return True
        self._send_json(403, {"error": "请求来源不是本地学习台"})
        return False

    def _static_path(self, request_path: str) -> Path:
        relative = "index.html" if request_path == "/" else request_path.lstrip("/")
        candidate = (self.engine.learning_root / unquote(relative)).resolve()
        root = self.engine.learning_root.resolve()
        if os.path.commonpath((str(root), str(candidate))) != str(root):
            raise CourseError("静态资源路径越界")
        return candidate

    def _serve_static(self, request_path: str) -> None:
        try:
            path = self._static_path(request_path)
        except CourseError as error:
            self._send_json(403, {"error": str(error)})
            return
        if not path.is_file():
            self._send_json(404, {"error": "资源不存在"})
            return
        content_type = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        textual = content_type.startswith("text/") or content_type in (
            "application/javascript",
            "application/json",
        )
        self._send_bytes(
            200,
            path.read_bytes(),
            content_type + ("; charset=utf-8" if textual else ""),
        )

    def _course_payload(self) -> dict:
        return self.engine.course_payload()

    def _progress_payload(self) -> dict:
        statuses = {
            module_id: self.engine.module_status(module_id)
            for module_id in self.engine.manifest.modules
        }
        return {"schema_version": 1, "statuses": statuses}

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if not path.startswith("/api/"):
            self._serve_static(path)
            return
        if not self._require_api_access():
            return
        if path == "/api/course":
            self._send_json(200, self._course_payload())
        elif path == "/api/progress":
            self._send_json(200, self._progress_payload())
        else:
            self._send_json(404, {"error": "API 不存在"})

    def _read_json(self) -> dict:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as error:
            raise CourseError("Content-Length 无效") from error
        if length <= 0 or length > MAX_REQUEST_BYTES:
            raise CourseError("请求正文为空或过大")
        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise CourseError("请求 JSON 无效") from error
        if not isinstance(payload, dict):
            raise CourseError("请求 JSON 必须是对象")
        return payload

    def _apply_action(self, path: str, body: dict) -> dict:
        module_id = body.get("module_id", "")
        if path == "/api/answers":
            result = self.engine.submit_answers(
                module_id, body.get("kind", ""), body.get("answers", {})
            )
            return {"result": result, "status": self.engine.module_status(module_id)}
        if path == "/api/standard-view":
            result = self.engine.record_standard_view(
                module_id, body.get("view_id", "")
            )
            return {
                **result,
                "status": self.engine.module_status(module_id),
            }
        if path == "/api/evidence":
            self.engine.submit_evidence(module_id, body.get("response", ""))
            return {"saved": True, "status": self.engine.module_status(module_id)}
        if path == "/api/review":
            result = self.engine.submit_review(
                module_id, int(body.get("day", 0)), body.get("answers", {})
            )
            return {"result": result, "status": self.engine.module_status(module_id)}
        raise CourseError("API 不存在")

    def do_POST(self) -> None:
        path = urlparse(self.path).path
        if not self._require_api_access():
            return
        allowed = {"/api/answers", "/api/standard-view", "/api/evidence", "/api/review"}
        if path not in allowed:
            self._send_json(404, {"error": "API 不存在"})
            return
        try:
            response = self._apply_action(path, self._read_json())
        except (CourseError, KeyError, TypeError, ValueError) as error:
            self._send_json(400, {"error": str(error)})
            return
        self._send_json(200, response)


def _handler_factory(engine: CourseEngine) -> Callable:
    return type(
        "BoundCourseRequestHandler",
        (CourseRequestHandler,),
        {"engine": engine},
    )


def create_server(engine: CourseEngine, port: int) -> ThreadingHTTPServer:
    """创建只绑定 127.0.0.1 的课程 HTTP 服务。"""

    server = ThreadingHTTPServer(("127.0.0.1", port), _handler_factory(engine))
    server.daemon_threads = True
    return server
