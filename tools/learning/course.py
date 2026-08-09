#!/usr/bin/env python3
"""FileGDB 本地学习实验室命令行入口。"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import List, Optional


REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.learning.course_core import CourseEngine, CourseError, ManifestError  # noqa: E402
from tools.learning.course_server import create_server  # noqa: E402


def _default_state_dir() -> Path:
    return REPO_ROOT / "build/learning"


def _confined_state_dir(value: str) -> Path:
    base = _default_state_dir().resolve()
    candidate = Path(value).resolve()
    if os.path.commonpath((str(base), str(candidate))) != str(base):
        raise argparse.ArgumentTypeError(
            f"学习状态目录必须位于 {base}，不能越出 build/learning"
        )
    return candidate


def _engine(arguments: argparse.Namespace) -> CourseEngine:
    return CourseEngine(REPO_ROOT, arguments.state_dir)


def _command_doctor(arguments: argparse.Namespace) -> int:
    engine = _engine(arguments)
    checks = [
        ("课程清单", len(engine.manifest.modules) == 18, "18 个模块"),
        (
            "核心数据",
            engine.fixture_status().get("verified", False),
            "acceptance_metadata.gdb + SHA-256",
        ),
        (
            "观察工具",
            (REPO_ROOT / "build/dev/bin/explorgdb_cli").is_file(),
            "build/dev/bin/explorgdb_cli",
        ),
        (
            "Python",
            sys.version_info >= (3, 9),
            f"{sys.version_info.major}.{sys.version_info.minor}",
        ),
    ]
    arguments.state_dir.mkdir(parents=True, exist_ok=True)
    checks.append(
        ("进度目录", os.access(arguments.state_dir, os.W_OK), str(arguments.state_dir))
    )
    for name, passed, detail in checks:
        print(f"{'READY' if passed else 'BLOCKED':7} {name}: {detail}")
    print("说明：explorgdb_cli 的 Magic file 警告不参与课程评分。")
    return 0 if all(item[1] for item in checks) else 1


def _command_status(arguments: argparse.Namespace) -> int:
    engine = _engine(arguments)
    print("FileGDB 学习进度")
    for module_id, module in engine.manifest.modules.items():
        state = engine.module_status(module_id)["state"]
        print(f"{module_id}  {state:10}  {module.title}")
    return 0


def _command_lab(arguments: argparse.Namespace) -> int:
    module_id = arguments.lab_id.split("-", 1)[0]
    artifact = _engine(arguments).run_lab(module_id, arguments.lab_id)
    print(f"实验通过，证据已保存：{artifact}")
    return 0


def _command_serve(arguments: argparse.Namespace) -> int:
    engine = _engine(arguments)
    server = create_server(engine, port=arguments.port)
    host, port = server.server_address
    print(f"学习台：http://{host}:{port}/")
    print("按 Ctrl-C 停止；服务只监听 127.0.0.1。")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n学习台已停止。")
    finally:
        server.server_close()
    return 0


def _review_prompt(engine: CourseEngine, module_id: str, session: dict) -> str:
    module = engine.delivered_module(module_id)
    focus = "、".join(module.oral_review.get("focus", []))
    return f"""# {module_id} · {module.title} AI 口试包

你是 FileGDB 学习口试官。一次只问一个问题，等待学习者回答后再追问；不要提前给出标准答案。

## 考查重点

{focus}

必须追问学习者：结论属于 EsriConfirmed、GdalConfirmed、FastGdbConfirmed、DataObserved、Inferred 还是 Unknown，以及该证据不能证明什么。

完成 3–5 个问题后，按以下四项各给 0–3 分：structure、accuracy、evidence、boundaries。通过要求总分不低于 10/12，且每项不低于 2。

请将最终结果保存为 `build/learning/reviews/{module_id}-result.json`，严格使用以下结构：

```json
{{
  "module_id": "{module_id}",
  "review_session": "{session["review_session"]}",
  "scores": {{"structure": 0, "accuracy": 0, "evidence": 0, "boundaries": 0}},
  "questions": [],
  "gaps": [],
  "status": "pass 或 retry",
  "reviewed_at": "ISO-8601 时间"
}}
```
"""


def _command_review_export(arguments: argparse.Namespace) -> int:
    engine = _engine(arguments)
    session = engine.create_oral_review_session(arguments.module_id)
    output = arguments.state_dir / f"reviews/{arguments.module_id}-prompt.md"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        _review_prompt(engine, arguments.module_id, session), encoding="utf-8"
    )
    session_path = arguments.state_dir / f"reviews/{arguments.module_id}-session.json"
    session_path.write_text(
        json.dumps(session, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"AI 口试提示词已生成：{output}")
    return 0


def _confined_review_path(state_dir: Path, value: str) -> Path:
    base = (state_dir / "reviews").resolve()
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = (REPO_ROOT / candidate).resolve()
    else:
        candidate = candidate.resolve()
    if os.path.commonpath((str(base), str(candidate))) != str(base):
        raise CourseError(f"AI 口试结果必须位于 {base}")
    return candidate


def _command_review_import(arguments: argparse.Namespace) -> int:
    path = _confined_review_path(arguments.state_dir, arguments.review_file)
    try:
        review = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CourseError(f"AI 口试结果无法读取：{error}") from error
    engine = _engine(arguments)
    engine.import_oral_review(arguments.module_id, review)
    print(f"AI 口试结果已导入：{arguments.module_id}")
    return 0


def _command_export_record(arguments: argparse.Namespace) -> int:
    engine = _engine(arguments)
    status = engine.module_status(arguments.module_id)
    if status["state"] != "Mastered":
        raise CourseError(
            f"{arguments.module_id} 尚未达到 Mastered，不能导出学习记录候选"
        )
    module = engine.manifest.modules[arguments.module_id]
    output = arguments.state_dir / f"record-candidates/{arguments.module_id}.md"
    output.parent.mkdir(parents=True, exist_ok=True)
    text = f"# {module.title}\n\n待学习者确认：说明已掌握的非显然结论，以及它如何改变后续学习。\n"
    output.write_text(text, encoding="utf-8")
    print(f"学习记录候选已生成：{output}")
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="FileGDB 本地学习实验室")
    parser.add_argument(
        "--state-dir",
        type=_confined_state_dir,
        default=_default_state_dir(),
        help=argparse.SUPPRESS,
    )
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("doctor").set_defaults(handler=_command_doctor)
    commands.add_parser("status").set_defaults(handler=_command_status)
    serve = commands.add_parser("serve")
    serve.add_argument("--port", type=int, default=8766)
    serve.set_defaults(handler=_command_serve)
    lab = commands.add_parser("lab")
    lab.add_argument("lab_id")
    lab.set_defaults(handler=_command_lab)
    review_export = commands.add_parser("review-export")
    review_export.add_argument("module_id")
    review_export.set_defaults(handler=_command_review_export)
    review_import = commands.add_parser("review-import")
    review_import.add_argument("module_id")
    review_import.add_argument("review_file")
    review_import.set_defaults(handler=_command_review_import)
    record = commands.add_parser("export-record")
    record.add_argument("module_id")
    record.set_defaults(handler=_command_export_record)
    return parser


def main(arguments: Optional[List[str]] = None) -> int:
    parsed = _parser().parse_args(arguments)
    try:
        return parsed.handler(parsed)
    except (CourseError, ManifestError) as error:
        print(f"错误：{error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
