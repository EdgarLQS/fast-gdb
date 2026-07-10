# 05 — fast-gdb v1 合并检查单（历史归档）

**文档状态**：📚 已归档  
**适用范围**：原 `feature/fast-gdb-plan` 合并门禁  
**当前发布检查单**：[08_fast-gdb只读发布收口.md](08_fast-gdb只读发布收口.md)

## 1. 归档说明

原 v1 分支已经合并 `main`。本文件中的构建、同步和 `git status` 项目属于当时的分支操作，不再作为当前未完成任务，也不应继续显示为当前 release blocker。

## 2. 原合并时已完成的代码收敛

- [x] 删除历史重复 `peek_geometry_blob` 实现。
- [x] 删除 `EXPLORGDB_RENAME_LEGACY_PEEK` 兼容层。
- [x] 固定宽度字段使用统一物理布局。
- [x] 公开 peek 路径使用统一字段跳过规则。
- [x] DateTimeWithOffset 在主要读取路径消费 10 字节。
- [x] QueryEngine 覆盖 open、FID、scan 和 bbox 主路径。
- [x] 缺少 `.atx` 时有明确状态和 reason。
- [x] 临时 GitHub Actions 工作流未进入最终合并。

## 3. 原合并门禁

以下项目已随原分支合并结束，不再逐项追踪：

- 本地 CMake 配置和编译。
- v1 新增专项测试。
- 当时的 smoke 和系统表测试。
- 同步当时的 `main`。
- 检查原分支改动范围。

历史检查单没有保存完整本地输出，因此不能用它证明当前分支测试已通过。

## 4. 当前需要重新执行的门禁

当前 `chore/fast-gdb-release-hardening` 分支必须使用新的发布清单：

- [ ] General Curve flag/header 修正和专项测试。
- [ ] MultiPatch capability 定级修正或语义补齐。
- [ ] 普通真实 FileGDB 回归。
- [ ] 真实曲线 FileGDB 边界回归。
- [ ] 当前分支专项测试。
- [ ] 当前分支全量测试。
- [ ] 最终文档一致性检查。

详细命令见 [08_fast-gdb只读发布收口.md](08_fast-gdb只读发布收口.md)。

## 5. 当前仍然有效的范围原则

- 不引入默认 GDAL 运行时 fallback。
- reader 发布不等于 writer 生产化完成。
- 不把 WKT 语法可输出等同于完整几何语义支持。
- 未运行的本地或真实数据测试不能标记为已通过。
