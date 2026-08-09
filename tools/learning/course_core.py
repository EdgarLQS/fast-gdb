"""课程清单和学习进度的领域模型。"""

from __future__ import annotations

import copy
import csv
import hashlib
import json
import os
import secrets
import shutil
import subprocess
import time
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from functools import wraps
from pathlib import Path
from threading import RLock
from typing import Callable, Dict, Iterator, Tuple

if os.name == "nt":
    import msvcrt
else:
    import fcntl


class ManifestError(ValueError):
    """课程清单不完整或内部关系无效。"""


class CourseError(RuntimeError):
    """课程操作无法安全或有效地完成。"""


DELIVERED_STATES = {"pilot", "current"}
ALLOWED_LAB_ROOTS = (
    "docs/gdb/learning",
    "src/edgar/explorgdb",
    "tests/edgar/explorgdb",
    "tests/createdata",
    "test_data/gdb/acceptance_metadata",
)


def _serialized(method: Callable) -> Callable:
    """让同一 CourseEngine 的完整读改写事务串行执行。"""

    @wraps(method)
    def wrapped(self, *arguments, **keywords):
        with self._transaction_lock:
            return method(self, *arguments, **keywords)

    return wrapped


def _try_advisory_lock(descriptor: int) -> bool:
    try:
        if os.name == "nt":
            if os.fstat(descriptor).st_size == 0:
                os.write(descriptor, b"0")
            os.lseek(descriptor, 0, os.SEEK_SET)
            msvcrt.locking(descriptor, msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


def _release_advisory_lock(descriptor: int) -> None:
    if os.name == "nt":
        os.lseek(descriptor, 0, os.SEEK_SET)
        msvcrt.locking(descriptor, msvcrt.LK_UNLCK, 1)
    else:
        fcntl.flock(descriptor, fcntl.LOCK_UN)


@dataclass(frozen=True)
class CourseModule:
    """一个可独立验收的 FileGDB 学习模块。"""

    module_id: str
    title: str
    delivery_state: str
    prerequisites: Tuple[str, ...]
    lesson: str
    micro_units: Tuple[dict, ...]
    diagnostic_questions: Tuple[dict, ...]
    quiz_questions: Tuple[dict, ...]
    question_standard_views: Dict[str, str]
    standard_views: Tuple[dict, ...]
    labs: Tuple[dict, ...]
    evidence_task: dict
    oral_review: dict
    review_days: Tuple[int, ...]
    review_tasks: Tuple[dict, ...]


@dataclass(frozen=True)
class CourseManifest:
    """经过关系校验的课程清单。"""

    schema_version: int
    title: str
    modules: Dict[str, CourseModule]


def _module_from_json(item: dict) -> CourseModule:
    module_id = item.get("id", "")
    if not module_id or not item.get("title"):
        raise ManifestError("每个模块必须包含 id 和 title")
    return CourseModule(
        module_id=module_id,
        title=item["title"],
        delivery_state=item.get("delivery_state", "planned"),
        prerequisites=tuple(item.get("prerequisites", [])),
        lesson=item.get("lesson", ""),
        micro_units=tuple(item.get("micro_units", [])),
        diagnostic_questions=tuple(item.get("diagnostic_questions", [])),
        quiz_questions=tuple(item.get("quiz_questions", [])),
        question_standard_views=dict(item.get("question_standard_views", {})),
        standard_views=tuple(item.get("standard_views", [])),
        labs=tuple(item.get("labs", [])),
        evidence_task=dict(item.get("evidence_task", {})),
        oral_review=dict(item.get("oral_review", {})),
        review_days=tuple(item.get("review_days", [])),
        review_tasks=tuple(item.get("review_tasks", [])),
    )


def _validate_prerequisites(modules: Dict[str, CourseModule]) -> None:
    for module in modules.values():
        unknown = set(module.prerequisites) - set(modules)
        if unknown:
            names = ", ".join(sorted(unknown))
            raise ManifestError(f"{module.module_id} 引用了未知先修模块：{names}")


def _validate_acyclic(modules: Dict[str, CourseModule]) -> None:
    visiting = set()
    visited = set()

    def visit(module_id: str) -> None:
        if module_id in visiting:
            raise ManifestError(f"先修关系存在环：{module_id}")
        if module_id in visited:
            return
        visiting.add(module_id)
        for prerequisite in modules[module_id].prerequisites:
            visit(prerequisite)
        visiting.remove(module_id)
        visited.add(module_id)

    for module_id in modules:
        visit(module_id)


def _validate_standard_views(modules: Dict[str, CourseModule]) -> None:
    supported_kinds = {"graph", "bytes", "file-family", "table"}
    for module in modules.values():
        delivered = module.delivery_state in DELIVERED_STATES
        if delivered and not module.standard_views:
            raise ManifestError(f"已交付模块缺少标准答案：{module.module_id}")
        view_ids = set()
        for view in module.standard_views:
            view_id = view.get("id")
            if not view_id or view_id in view_ids:
                raise ManifestError(f"标准答案 ID 重复或为空：{module.module_id}")
            if view.get("kind") not in supported_kinds:
                raise ManifestError(f"标准答案类型不支持：{module.module_id}/{view_id}")
            required = ("title", "summary", "standard_answer", "evidence", "sources")
            if any(not view.get(key) for key in required):
                raise ManifestError(f"标准答案字段不完整：{module.module_id}/{view_id}")
            view_ids.add(view_id)
        question_ids = {
            question["id"]
            for question in (*module.diagnostic_questions, *module.quiz_questions)
        }
        missing_questions = question_ids - set(module.question_standard_views)
        unknown_views = set(module.question_standard_views.values()) - view_ids
        if delivered and missing_questions:
            raise ManifestError(f"题目缺少标准答案映射：{module.module_id}")
        if unknown_views:
            raise ManifestError(f"题目引用未知标准答案：{module.module_id}")


def _read_json_object(path: Path, label: str) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ManifestError(f"无法读取{label}：{error}") from error
    if not isinstance(data, dict):
        raise ManifestError(f"{label}必须是 JSON 对象")
    return data


def load_course_data(path: Path) -> dict:
    """读取主清单，并按声明顺序合并独立模块文件。"""

    path = path.resolve()
    data = _read_json_object(path, "课程清单")
    modules = list(data.get("modules", []))
    root = path.parent.resolve()
    for relative in data.get("module_files", []):
        if not isinstance(relative, str):
            raise ManifestError("module_files 只能包含相对路径字符串")
        candidate = (root / relative).resolve()
        if os.path.commonpath((str(root), str(candidate))) != str(root):
            raise ManifestError(f"模块文件路径越界：{relative}")
        modules.append(_read_json_object(candidate, f"模块文件 {relative}"))
    return {**data, "modules": modules}


def load_manifest(path: Path) -> CourseManifest:
    """读取并验证课程清单。"""

    data = load_course_data(path)
    modules = {}
    for item in data.get("modules", []):
        module = _module_from_json(item)
        if module.module_id in modules:
            raise ManifestError(f"模块 ID 重复：{module.module_id}")
        modules[module.module_id] = module
    _validate_prerequisites(modules)
    _validate_acyclic(modules)
    _validate_standard_views(modules)
    return CourseManifest(data.get("schema_version", 0), data.get("title", ""), modules)


def _utc_now() -> datetime:
    return datetime.now(timezone.utc)


class CourseEngine:
    """课程清单驱动的进度、评分和实验入口。"""

    def __init__(
        self,
        repo_root: Path,
        state_dir: Path,
        clock: Callable[[], datetime] = _utc_now,
    ) -> None:
        self.repo_root = repo_root.resolve()
        self.learning_root = self.repo_root / "docs/gdb/learning"
        self.state_dir = state_dir.resolve()
        self.clock = clock
        self._transaction_lock = RLock()
        self.manifest = load_manifest(self.learning_root / "course.json")
        answer_path = self.repo_root / "tools/learning/answer_key.json"
        self.answer_key = json.loads(answer_path.read_text(encoding="utf-8"))
        self.progress_path = self.state_dir / "progress.json"
        self.progress = self._load_progress()

    def _load_progress(self) -> dict:
        if not self.progress_path.exists():
            return {"schema_version": 1, "modules": {}}
        try:
            return json.loads(self.progress_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise CourseError(f"学习进度无法读取：{error}") from error

    @staticmethod
    def _merge_progress(base: dict, overlay: dict) -> dict:
        merged = copy.deepcopy(base)
        for key, value in overlay.items():
            if isinstance(value, dict) and isinstance(merged.get(key), dict):
                merged[key] = CourseEngine._merge_progress(merged[key], value)
            else:
                merged[key] = copy.deepcopy(value)
        return merged

    @contextmanager
    def _locked(self) -> Iterator[None]:
        self.state_dir.mkdir(parents=True, exist_ok=True)
        lock_path = self.state_dir / ".progress.lock"
        descriptor = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o600)
        deadline = time.monotonic() + 2.0
        while not _try_advisory_lock(descriptor):
            if time.monotonic() >= deadline:
                os.close(descriptor)
                raise CourseError("学习进度正被另一个进程写入；请稍后重试")
            time.sleep(0.05)
        try:
            yield
        finally:
            _release_advisory_lock(descriptor)
            os.close(descriptor)

    def _save(self) -> None:
        with self._locked():
            backups = self.state_dir / "backups"
            if self.progress_path.exists():
                backups.mkdir(parents=True, exist_ok=True)
                stamp = self.clock().strftime("%Y%m%dT%H%M%S%fZ")
                shutil.copy2(self.progress_path, backups / f"progress-{stamp}.json")
                latest = self._load_progress()
                self.progress = self._merge_progress(latest, self.progress)
                for module_id in self.progress.get("modules", {}):
                    if self.manifest.modules[module_id].delivery_state in DELIVERED_STATES:
                        self._refresh_state(module_id)
            temporary = self.state_dir / ".progress.json.tmp"
            text = json.dumps(self.progress, ensure_ascii=False, indent=2) + "\n"
            temporary.write_text(text, encoding="utf-8")
            os.replace(temporary, self.progress_path)

    def _module_data(self, module_id: str) -> dict:
        self._require_module(module_id)
        return self.progress["modules"].setdefault(module_id, {})

    def _require_module(self, module_id: str) -> CourseModule:
        module = self.manifest.modules.get(module_id)
        if module is None:
            raise CourseError(f"未知课程模块：{module_id}")
        if module.delivery_state not in DELIVERED_STATES:
            raise CourseError(f"{module_id} 尚未进入课程交付范围")
        return module

    def delivered_module(self, module_id: str) -> CourseModule:
        """返回已经交付学习闭环的模块。"""

        return self._require_module(module_id)

    def course_payload(self) -> dict:
        """返回已合并的浏览器课程清单。"""

        data = load_course_data(self.learning_root / "course.json")
        for module in data["modules"]:
            answers = self.answer_key["modules"][module["id"]]
            for kind in ("diagnostic_questions", "quiz_questions"):
                answer_kind = kind.removesuffix("_questions")
                for question in module[kind]:
                    correct_id = answers[answer_kind][question["id"]]
                    option = next(
                        item for item in question["options"] if item["id"] == correct_id
                    )
                    view_id = module["question_standard_views"][question["id"]]
                    view = next(
                        item for item in module["standard_views"] if item["id"] == view_id
                    )
                    question["answer_explanation"] = {
                        "correct_option": correct_id,
                        "correct_text": option["text"],
                        "explanation": view["standard_answer"],
                    }
        return data

    def _is_unlocked(self, module: CourseModule) -> bool:
        for prerequisite in module.prerequisites:
            state = self.module_status(prerequisite)["state"]
            if state not in ("Provisional", "Mastered"):
                return False
        return True

    def _touch_learning(self, module_id: str) -> dict:
        self.progress = self._load_progress()
        module = self._require_module(module_id)
        if not self._is_unlocked(module):
            raise CourseError(f"{module_id} 的先修模块尚未达到 Provisional")
        data = self._module_data(module_id)
        data.setdefault("started_at", self.clock().isoformat())
        data["updated_at"] = self.clock().isoformat()
        return data

    @staticmethod
    def _score(expected: dict, answers: dict) -> dict:
        correct = {key: answers.get(key) == value for key, value in expected.items()}
        score = sum(correct.values()) / len(expected) if expected else 0.0
        return {"answers": answers, "correct": correct, "score": score}

    @_serialized
    def submit_answers(self, module_id: str, kind: str, answers: dict) -> dict:
        if kind not in ("diagnostic", "quiz"):
            raise CourseError(f"不支持的答题类型：{kind}")
        if not isinstance(answers, dict):
            raise CourseError("答题内容必须是 JSON 对象")
        data = self._touch_learning(module_id)
        expected = self.answer_key["modules"][module_id][kind]
        result = self._score(expected, answers)
        if kind == "quiz":
            module = self.manifest.modules[module_id]
            critical = {q["id"] for q in module.quiz_questions if q.get("critical")}
            result["critical_passed"] = all(result["correct"].get(q) for q in critical)
        data[kind] = result
        self._refresh_state(module_id)
        self._save()
        return result

    @_serialized
    def record_standard_view(self, module_id: str, view_id: str) -> dict:
        """记录学习者打开了标准答案，但不把查看行为当作掌握门槛。"""

        module = self._require_module(module_id)
        view_ids = {view.get("id") for view in module.standard_views}
        if view_id not in view_ids:
            raise CourseError(f"未知标准答案：{view_id}")
        data = self._touch_learning(module_id)
        viewed_at = (
            data.setdefault("standard_views", {}).get(view_id, {}).get("viewed_at")
        )
        if not viewed_at:
            viewed_at = self.clock().isoformat()
            data["standard_views"][view_id] = {"viewed_at": viewed_at}
            self._save()
        return {"viewed": True, "viewed_at": viewed_at}

    @_serialized
    def submit_evidence(self, module_id: str, response: str) -> None:
        if not isinstance(response, str):
            raise CourseError("证据说明必须是文本")
        text = response.strip()
        if len(text) < 20:
            raise CourseError("证据说明至少需要 20 个字符")
        module = self._require_module(module_id)
        required = module.evidence_task.get("required_levels", [])
        missing = [level for level in required if level not in text]
        if missing:
            raise CourseError(f"证据说明缺少等级：{', '.join(missing)}")
        if "不能证明" not in text:
            raise CourseError("证据说明必须明确写出“不能证明什么”")
        data = self._touch_learning(module_id)
        task_id = module.evidence_task["id"]
        artifact, digest = self._write_artifact(
            f"{module_id}/{task_id}.md",
            f"# {task_id} 证据说明\n\n{text}\n",
        )
        data["evidence"] = {
            "artifact": str(artifact.relative_to(self.state_dir / "artifacts")),
            "sha256": digest,
            "submitted_at": self.clock().isoformat(),
        }
        self._refresh_state(module_id)
        self._save()

    def artifact_path(self, relative_path: str) -> Path:
        base = (self.state_dir / "artifacts").resolve()
        candidate = (base / relative_path).resolve()
        if os.path.commonpath((str(base), str(candidate))) != str(base):
            raise CourseError("实验产物路径越出了 build/learning/artifacts")
        return candidate

    def _write_artifact(self, relative_path: str, content: str) -> Tuple[Path, str]:
        artifact = self.artifact_path(relative_path)
        artifact.parent.mkdir(parents=True, exist_ok=True)
        temporary = artifact.with_name(f".{artifact.name}.tmp")
        temporary.write_text(content, encoding="utf-8")
        os.replace(temporary, artifact)
        digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
        return artifact, digest

    def _artifact_matches(self, record: dict) -> bool:
        try:
            artifact = self.artifact_path(record.get("artifact", ""))
        except (CourseError, TypeError):
            return False
        if not artifact.is_file() or not record.get("sha256"):
            return False
        return hashlib.sha256(artifact.read_bytes()).hexdigest() == record["sha256"]

    @staticmethod
    def _tree_digest(directory: Path) -> dict:
        digest = hashlib.sha256()
        file_count = 0
        size_bytes = 0
        for path in sorted(item for item in directory.rglob("*") if item.is_file()):
            relative = path.relative_to(directory).as_posix()
            file_digest = hashlib.sha256(path.read_bytes()).hexdigest()
            digest.update(f"{relative}\n{file_digest}\n".encode("utf-8"))
            file_count += 1
            size_bytes += path.stat().st_size
        return {
            "tree_sha256": digest.hexdigest(),
            "file_count": file_count,
            "size_bytes": size_bytes,
        }

    def fixture_status(self) -> dict:
        source = self.repo_root / "test_data/gdb/acceptance_metadata.gdb"
        manifest_path = self.learning_root / "fixtures/core-fixture.json"
        if not source.is_dir() or not manifest_path.is_file():
            return {"verified": False, "reason": "核心数据或来源清单不存在"}
        expected = json.loads(manifest_path.read_text(encoding="utf-8"))
        observed = self._tree_digest(source)
        keys = ("tree_sha256", "file_count", "size_bytes")
        observed["verified"] = all(observed[key] == expected.get(key) for key in keys)
        observed["source"] = expected.get("source", "")
        return observed

    def _fixture_copy(self) -> Path:
        source = self.repo_root / "test_data/gdb/acceptance_metadata.gdb"
        status = self.fixture_status()
        if not status.get("verified"):
            raise CourseError("核心教学数据缺失或 SHA-256 与来源清单不一致")
        destination = self.state_dir / "fixtures/acceptance_metadata.gdb"
        source_digest = self._tree_digest(source)["tree_sha256"]
        destination_digest = (
            self._tree_digest(destination)["tree_sha256"]
            if destination.is_dir()
            else ""
        )
        if destination_digest != source_digest:
            if destination.exists():
                shutil.rmtree(destination)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(source, destination)
        return destination

    def _run_catalog_lab(self, fixture: Path) -> str:
        executable = self.repo_root / "build/dev/bin/explorgdb_cli"
        if not executable.is_file():
            raise CourseError("缺少 build/dev/bin/explorgdb_cli，请先完成项目构建")
        result = subprocess.run(
            [str(executable), "explore", str(fixture)],
            cwd=self.repo_root,
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            raise CourseError(f"explorgdb_cli 执行失败：{result.stderr.strip()}")
        return result.stdout

    @staticmethod
    def _run_hex_lab(fixture: Path) -> str:
        table = fixture / "a00000001.gdbtable"
        data = table.read_bytes()[:64]
        rows = [data[index : index + 16].hex(" ") for index in range(0, len(data), 16)]
        return (
            "\n".join(f"{index * 16:08x}  {row}" for index, row in enumerate(rows))
            + "\n"
        )

    @staticmethod
    def _run_inventory_lab(fixture: Path) -> str:
        inventory = []
        for path in sorted(item for item in fixture.iterdir() if item.is_file()):
            inventory.append(
                {"name": path.name, "suffix": path.suffix, "size": path.stat().st_size}
            )
        return json.dumps({"files": inventory}, ensure_ascii=False, indent=2) + "\n"

    def _resolve_lab_input(self, fixture: Path, item: dict) -> Path:
        relative = item.get("path")
        scope = item.get("scope")
        if not isinstance(relative, str) or not relative or Path(relative).is_absolute():
            raise CourseError("实验输入必须是非空相对路径")
        if scope == "fixture":
            root = fixture.resolve()
        elif scope == "repo":
            if not any(
                relative == prefix or relative.startswith(f"{prefix}/")
                for prefix in ALLOWED_LAB_ROOTS
            ):
                raise CourseError(f"实验输入不在课程白名单：{relative}")
            root = self.repo_root
        else:
            raise CourseError(f"实验输入 scope 不支持：{scope}")
        candidate = (root / relative).resolve()
        if os.path.commonpath((str(root), str(candidate))) != str(root):
            raise CourseError(f"实验输入路径越界：{relative}")
        if not candidate.exists():
            raise CourseError(f"实验输入不存在：{relative}")
        return candidate

    @staticmethod
    def _profile_csv(path: Path) -> dict:
        with path.open(encoding="utf-8", newline="") as stream:
            rows = list(csv.reader(stream))
        return {
            "headers": rows[0] if rows else [],
            "row_count": max(0, len(rows) - 1),
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        }

    @staticmethod
    def _profile_text(path: Path, item: dict) -> dict:
        text = path.read_text(encoding="utf-8")
        terms = item.get("terms", [])
        if not isinstance(terms, list) or not all(isinstance(term, str) for term in terms):
            raise CourseError("文本实验的 terms 必须是字符串数组")
        return {
            "line_count": len(text.splitlines()),
            "terms": {term: term in text for term in terms},
            "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
        }

    @staticmethod
    def _profile_binary(path: Path, item: dict) -> dict:
        prefix_bytes = item.get("prefix_bytes", 64)
        if not isinstance(prefix_bytes, int) or not 1 <= prefix_bytes <= 4096:
            raise CourseError("二进制实验 prefix_bytes 必须在 1–4096 之间")
        data = path.read_bytes()
        prefix = data[:prefix_bytes]
        return {
            "size_bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "prefix_bytes": len(prefix),
            "prefix_sha256": hashlib.sha256(prefix).hexdigest(),
            "prefix_hex": prefix.hex(" "),
        }

    def _profile_lab_input(self, fixture: Path, item: dict) -> dict:
        path = self._resolve_lab_input(fixture, item)
        mode = item.get("mode")
        if mode == "csv" and path.is_file():
            profile = self._profile_csv(path)
        elif mode == "text" and path.is_file():
            profile = self._profile_text(path, item)
        elif mode == "binary" and path.is_file():
            profile = self._profile_binary(path, item)
        elif mode == "directory" and path.is_dir():
            profile = self._tree_digest(path)
        else:
            raise CourseError(f"实验输入模式与路径不匹配：{item.get('path')}")
        terms = profile.get("terms", {})
        profile["valid"] = bool(profile.get("sha256") or profile.get("tree_sha256")) and all(
            terms.values()
        )
        return {"scope": item["scope"], "path": item["path"], "mode": mode, **profile}

    def _run_evidence_report(self, fixture: Path, lab: dict) -> str:
        inputs = lab.get("inputs")
        if not isinstance(inputs, list) or not inputs:
            raise CourseError("证据实验至少需要一个输入")
        report = {
            "lab_id": lab["id"],
            "title": lab["title"],
            "inputs": [self._profile_lab_input(fixture, item) for item in inputs],
        }
        return json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n"

    def _validate_lab(
        self, module_id: str, lab: dict, fixture: Path, output: str
    ) -> dict:
        expected = self.answer_key["modules"][module_id]["labs"][lab["id"]]
        if lab["kind"] == "catalog-overview":
            observed = self._tree_digest(fixture)["tree_sha256"]
            return {
                "tree_sha256": observed == expected["tree_sha256"],
                "observation_nonempty": bool(output.strip()),
            }
        if lab["kind"] == "hex-observation":
            prefix = (fixture / "a00000001.gdbtable").read_bytes()[:64]
            observed = hashlib.sha256(prefix).hexdigest()
            output_digest = hashlib.sha256(output.encode("utf-8")).hexdigest()
            return {
                "prefix_sha256": observed == expected["prefix_sha256"],
                "output_sha256": output_digest == expected["output_sha256"],
            }
        if lab["kind"] == "evidence-report":
            try:
                report = json.loads(output)
                inputs = report["inputs"]
            except (json.JSONDecodeError, KeyError, TypeError):
                inputs = []
            output_digest = hashlib.sha256(output.encode("utf-8")).hexdigest()
            return {
                "input_count": len(inputs) == expected["input_count"],
                "inputs_valid": bool(inputs)
                and all(isinstance(item, dict) and item.get("valid") for item in inputs),
                "output_sha256": output_digest == expected["output_sha256"],
            }
        try:
            output_files = json.loads(output)["files"]
        except (json.JSONDecodeError, KeyError, TypeError):
            output_files = []
        suffixes = {}
        files = [item for item in fixture.iterdir() if item.is_file()]
        for suffix in expected["suffix_counts"]:
            suffixes[suffix] = sum(item.suffix == suffix for item in files)
        return {
            "file_count": len(files) == expected["file_count"],
            "suffix_counts": suffixes == expected["suffix_counts"],
            "output_inventory": {
                (item.get("name"), item.get("suffix"), item.get("size"))
                for item in output_files
                if isinstance(item, dict)
            }
            == {(item.name, item.suffix, item.stat().st_size) for item in files},
        }

    @_serialized
    def run_lab(self, module_id: str, lab_id: str) -> Path:
        module = self._require_module(module_id)
        lab = next((item for item in module.labs if item["id"] == lab_id), None)
        if lab is None:
            raise CourseError(f"未知实验：{lab_id}")
        data = self._touch_learning(module_id)
        fixture = self._fixture_copy()
        runners = {
            "catalog-overview": self._run_catalog_lab,
            "hex-observation": self._run_hex_lab,
            "file-inventory": self._run_inventory_lab,
        }
        if lab["kind"] == "evidence-report":
            output = self._run_evidence_report(fixture, lab)
        else:
            runner = runners.get(lab["kind"])
            if runner is None:
                raise CourseError(f"不支持的实验类型：{lab['kind']}")
            output = runner(fixture)
        validation = self._validate_lab(module_id, lab, fixture, output)
        artifact, digest = self._write_artifact(f"{module_id}/{lab_id}.txt", output)
        passed = all(validation.values())
        data.setdefault("labs", {})[lab_id] = {
            "passed": passed,
            "artifact": str(artifact.relative_to(self.state_dir / "artifacts")),
            "sha256": digest,
            "validation": validation,
            "completed_at": self.clock().isoformat(),
        }
        self._refresh_state(module_id)
        self._save()
        if not passed:
            raise CourseError(f"{lab_id} 输出与版本化课程清单不一致")
        return artifact

    @_serialized
    def create_oral_review_session(self, module_id: str) -> dict:
        data = self._touch_learning(module_id)
        session = {
            "module_id": module_id,
            "review_session": secrets.token_urlsafe(18),
            "created_at": self.clock().isoformat(),
            "consumed": False,
        }
        data["oral_session"] = session
        data.pop("oral_review", None)
        self._refresh_state(module_id)
        self._save()
        return dict(session)

    @_serialized
    def import_oral_review(self, module_id: str, review: dict) -> None:
        if not isinstance(review, dict):
            raise CourseError("AI 口试结果必须是 JSON 对象")
        data = self._touch_learning(module_id)
        if review.get("module_id") != module_id or review.get("status") != "pass":
            raise CourseError("AI 口试结果的模块或状态无效")
        session = data.get("oral_session", {})
        if session.get("consumed") or review.get("review_session") != session.get(
            "review_session"
        ):
            raise CourseError("AI 口试会话不存在、已失效或已使用")
        questions = review.get("questions")
        if not isinstance(questions, list) or not 3 <= len(questions) <= 5:
            raise CourseError("AI 口试必须记录 3–5 个问题")
        if not all(
            isinstance(question, str) and question.strip() for question in questions
        ):
            raise CourseError("AI 口试问题记录无效")
        if not isinstance(review.get("gaps"), list):
            raise CourseError("AI 口试 gaps 必须是数组")
        try:
            reviewed_at = datetime.fromisoformat(review.get("reviewed_at", ""))
            created_at = datetime.fromisoformat(session["created_at"])
        except (TypeError, ValueError) as error:
            raise CourseError("AI 口试 reviewed_at 必须是 ISO-8601 时间") from error
        if reviewed_at.tzinfo is None or reviewed_at < created_at:
            raise CourseError("AI 口试时间早于会话创建时间或缺少时区")
        scores = review.get("scores", {})
        dimensions = ("structure", "accuracy", "evidence", "boundaries")
        values = [scores.get(name, -1) for name in dimensions]
        if any(
            not isinstance(value, int) or value < 2 or value > 3 for value in values
        ):
            raise CourseError("AI 口试每项必须为 2–3 分")
        if sum(values) < 10:
            raise CourseError("AI 口试总分必须不低于 10/12")
        data["oral_review"] = dict(review, passed=True)
        session["consumed"] = True
        session["consumed_at"] = self.clock().isoformat()
        self._refresh_state(module_id)
        self._save()

    def _gate_flags(self, module_id: str, data: dict) -> dict:
        module = self.manifest.modules[module_id]
        quiz = data.get("quiz", {})
        lab_ids = {item["id"] for item in module.labs}
        return {
            "quiz_passed": quiz.get("score", 0) >= 0.85
            and quiz.get("critical_passed", False),
            "labs_passed": all(
                data.get("labs", {}).get(i, {}).get("passed")
                and self._artifact_matches(data.get("labs", {}).get(i, {}))
                for i in lab_ids
            ),
            "evidence_submitted": self._artifact_matches(data.get("evidence", {})),
            "oral_review_passed": data.get("oral_review", {}).get("passed", False),
        }

    def _refresh_state(self, module_id: str) -> None:
        data = self._module_data(module_id)
        gate = self._gate_flags(module_id, data)
        if not all(gate.values()):
            data["state"] = "Learning"
            return
        if "provisional_at" not in data:
            provisional = self.clock()
            data["provisional_at"] = provisional.isoformat()
            data["review_due"] = {
                str(day): (provisional + timedelta(days=day)).isoformat()
                for day in self.manifest.modules[module_id].review_days
            }
        reviews = data.get("reviews", {})
        days = self.manifest.modules[module_id].review_days
        data["state"] = (
            "Mastered"
            if all(reviews.get(str(day), {}).get("passed") for day in days)
            else "Provisional"
        )

    @_serialized
    def module_status(self, module_id: str) -> dict:
        self.progress = self._load_progress()
        module = self.manifest.modules.get(module_id)
        if module is None:
            raise CourseError(f"未知课程模块：{module_id}")
        if module.delivery_state not in DELIVERED_STATES:
            return {"state": "Planned", "gate": {}}
        if not self._is_unlocked(module):
            return {"state": "Locked", "gate": {}}
        data = self.progress["modules"].get(module_id)
        if data is None:
            return {
                "state": "NotStarted",
                "gate": self._gate_flags(module_id, {}),
                "standard_views": {},
            }
        gate = self._gate_flags(module_id, data)
        state = data.get("state", "Learning")
        if state in ("Provisional", "Mastered") and not all(gate.values()):
            state = "Learning"
        return {
            "state": state,
            "gate": gate,
            "review_due": data.get("review_due", {}),
            "reviews": data.get("reviews", {}),
            "standard_views": {
                key: value.get("viewed_at", "")
                for key, value in data.get("standard_views", {}).items()
            },
            "artifacts": {
                "labs": {
                    key: value.get("artifact", "")
                    for key, value in data.get("labs", {}).items()
                },
                "evidence": data.get("evidence", {}).get("artifact", ""),
            },
        }

    @_serialized
    def submit_review(self, module_id: str, day: int, answers: dict) -> dict:
        self.progress = self._load_progress()
        module = self._require_module(module_id)
        if not self._is_unlocked(module):
            raise CourseError(f"{module_id} 的先修模块尚未达到 Provisional")
        data = self._module_data(module_id)
        due_text = data.get("review_due", {}).get(str(day))
        if not due_text:
            raise CourseError("模块尚未达到 Provisional，不能进行间隔复习")
        due = datetime.fromisoformat(due_text)
        if self.clock() < due:
            raise CourseError(f"D+{day} 复习尚未到期")
        expected = self.answer_key["modules"][module_id]["reviews"].get(str(day))
        if expected is None:
            raise CourseError(f"不存在 D+{day} 复习任务")
        result = self._score(expected, answers)
        result["passed"] = result["score"] >= 0.8
        result["completed_at"] = self.clock().isoformat()
        data.setdefault("reviews", {})[str(day)] = result
        self._refresh_state(module_id)
        self._save()
        return result
