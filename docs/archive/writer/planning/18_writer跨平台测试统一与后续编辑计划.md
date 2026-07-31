> **历史归档**：本文记录已废弃的 Writer 方案或阶段性证据，不代表当前产品能力。fast-gdb 当前仅提供 Reader；现行边界见 [ADR-007](../../../governance/adr/ADR-007-reader-only-gdal-edit-boundary.md)，归档说明见 [Writer 历史索引](../README.md)。

# 18 — Writer 跨平台测试统一与后续编辑计划

- **更新日期**：2026-07-17
- **文档状态**：M18 能力与验收边界
- **当前实现基线**：`main@42d8f76620a8c39eeb8523a0f84fcde0eb719f01`
- **当前执行平台**：macOS
- **暂缓平台**：Linux、Windows；不计入当前里程碑和完成率

> M18 的 Append、Update、Delete、Transaction 和 Recovery 已由 squash 提交 `9dd7edf73763b56d84677c8a246bc85f80a1a0c1` 合入 `main`。本文不再把这些能力列为待实现事项。相关验收记录已移除。

## 1. 当前基线与支持边界

计划 17 已完成 macOS 上的安全空 schema 批量写、字段/几何矩阵、原子发布、4 GiB 边界、索引重建、Reader → Writer → 索引 → GDAL 闭环、安装消费、1K–10M 基准、磁盘写满和 1800 秒长稳。Reader 已完成 macOS 10M 正确性、8 线程独立 QueryEngine 和 1800 秒长稳，但 Point、MultiPoint、Polyline 的 10M fresh-open 仍有已记录的性能缺口。

当前 Writer 实现范围为：

- 空 schema one-shot `WriterSession`；
- GDAL-only 非空顺序追加 `WriterAppendSession`；
- GDAL-only `WriterUpdateSession`；
- GDAL-only Delete；
- 统一 `WriterTransaction`；
- 显式 `writer_recovery` 检查与安全恢复动作；
- 稳定 `WriterErrorCode` 与稳定/legacy 安装面。

边界保持不变：单 Writer、单源 GDB、单图层事务；不支持嵌套事务、savepoint、跨 GDB 事务、并发 Writer 保证、原生曲线写入或 MultiPatch 写入。Linux、Windows、50M 和跨平台绝对性能比较仍 Deferred。

## 2. 统一测试契约

Google Test 名称保持稳定，通过 manifest 建立业务场景编号：

| 编号域 | 范围 |
|---|---|
| `W-SCHEMA-*` | schema 与安全打开 |
| `W-FIELD-*` | 字段编码和严格类型 |
| `W-GEOM-*` | 几何编码、维度、拓扑和非法输入 |
| `W-FAIL-*` | 状态机、I/O、flush/close、发布和边界失败 |
| `W-INDEX-*` | 空间/属性索引闭环 |
| `W-PKG-*` | 安装消费和稳定安装面 |
| `W-APPEND-*` | 非空追加、FID、范围、索引和回滚 |
| `W-UPDATE-*` | FID Update、字段/几何、索引和回滚 |
| `W-DELETE-*` | Delete、范围、索引和空表 |
| `W-TXN-*` | 事务、发布、回滚和恢复 |
| `R-CONC-*` | Reader 并发 |
| `R-STABLE-*` | Reader 稳定性 |

结果只允许 `PASS`、`FAIL`、`SKIP`；`SKIP` 必须包含结构化原因，且不得计入通过数。证据使用 schema v2，至少包含提交、环境、命令、manifest、数据规模、缓存状态、分段耗时、正确性和最终状态。

## 3. M18.1 — macOS 测试契约冻结

### 实现状态

代码、schema-v2 runner、合同和 workflow 已实现并进入 `main`。

### 正式验收出口

- GDAL ON/OFF Release 和完整 CTest；
- 基础 Writer required 场景连续执行 3 次；
- 无无原因 `SKIP`；
- manifest、JSON、CSV 和环境元数据完整；
- 当前 SHA 对应的 macOS workflow steps、logs 和 artifact。

当前未取得上述完整正式证据，因此状态为 Blocked，而不是未实现。

## 4. M18.2 — Writer API 与安装面收口

### 实现状态

`WriterSession`、`writer_recovery`、结构化错误、稳定/legacy 安装面和 package consumer 已实现并进入 `main`。

### 正式验收出口

- 无 GDAL Writer consumer 编译并运行；
- legacy consumer 编译并运行；
- GDAL consumer 覆盖 Index、Append 和 Transaction；
- no-GDAL 不暴露 GDAL-only API；
- 稳定 target 不泄露物理布局头；
- 错误包含稳定代码、阶段、图层、路径和系统原因。

## 5. M18.3 — macOS 性能专项

### 实现状态

current/main/GDAL 比较器、5% 回退门禁、Writer-only 分段和 macOS `sample(1)` 采集基础设施已实现。

### 正式验收出口

- 性能 workflow 使用 `samples=3`、`profile_duration_seconds=5`、`run_reader_10m=false`；
- 正确性失败时性能结果无效；
- current 相对 main 默认不得回退超过 5%；
- artifact 保存 current/main/GDAL 摘要、环境和 raw profile；
- 未达到 GDAL 的场景保留真实结论。

当前尚无绑定 `main@42d8f76` 的 raw profile artifact，不据此声称性能正式通过，也不在本分支进行 Reader 优化。

## 6. M18.4 — 高级编辑

### 6.1 非空追加

状态：实现完成，已进入 `main`；正式合同 artifact 待补。

- 完整 sibling staging；
- 原记录/FID 不变；
- 新 FID 单调且不复用空洞；
- 严格字段/几何验证；
- 逐行回读、索引查询、backup/publish/rollback。

### 6.2 Update

状态：实现完成，先前 self-review 的 commit-time 逐 FID 重开验证、Binary byte-for-byte 和公共头测试依赖问题已在 squash 前清零；正式合同 artifact 待补。

- 全 GDB staging；
- FID 和总记录数保持；
- 未指定字段/几何保持原值；
- 不存在 FID、类型错误和非法几何发布前失败；
- commit-time 重开逐 FID 验证。

### 6.3 Delete

状态：实现完成，已进入 `main`；正式合同 artifact 待补。

- staging 删除，不直接维护 freelist；
- survivor FID 不变，被删除 FID 不立即复用；
- 范围收缩、索引无残留和全删空表；
- 不存在 FID、重复 FID 和空选择语义由 ADR 冻结。

### 6.4 事务与崩溃恢复

状态：实现完成，已进入 `main`；正式合同与故障注入 artifact 待补。

- 单 Writer、单源 GDB、单图层，第一版无嵌套/savepoint；
- Append/Update/Delete 共享一个 working GDB，一次发布；
- commit 只在全量重开验证通过后发布；
- Recovery 对旧命名、损坏源/backup、伪造或歧义候选采取显式安全处理；
- 默认不自动覆盖健康源数据。

## 7. Public API Freeze

API Freeze 审查见 [API_FREEZE_REVIEW](../reviews/API_FREEZE_REVIEW.md)。当前稳定安装面为：

- 无 GDAL：`writer_session.h`、`writer_recovery.h`；
- GDAL 额外：`writer_index.h`、`writer_append.h`、`writer_update.h`、`writer_delete.h`、`writer_transaction.h`；
- legacy 物理布局 API 仅安装到 `writer_legacy`；
- `row_buffer`、`tablx_writer`、`gdb_table_writer` 等内部头不得泄露到稳定目录。

## 8. 当前阻塞

Issue #12 记录 GitHub Actions 在 checkout 前失败、没有 steps/logs 的外部阻塞。对 `main@42d8f76` 的查询仍未获得当前 workflow runs 和 artifacts。因此：

- 代码和本地 Writer 专项结论可以记录；
- 正式判定保持 **Code accepted / Formal acceptance blocked**；
- PR #11 的历史 Draft/Request Changes 结论不得被当成当前实现未完成；
- 历史结果、静态检查、推算和 `SKIP` 不得替代当前验收；
- Issue #12 只有在 Actions 可以实际执行并生成正式 artifact 后才能关闭。

## 9. 暂缓事项

- Linux Writer 测试与安装消费；
- Windows UTF-8、锁、排他发布与自动化；
- 跨平台绝对性能比较；
- 50M、35GB、5 亿级生产阶梯；
- 原生曲线和 MultiPatch 写入；
- 并发 Writer、跨 GDB、嵌套事务和 savepoint。

## 10. 当前收口顺序

1. 运行 GDAL ON/OFF Release 和完整 CTest；
2. 五套功能合同各连续三次，另行运行性能合同；
3. 运行 no-GDAL、legacy、GDAL 三类 package consumer；
4. 核实六个 macOS workflow 的 checkout、steps、logs 和绑定当前 SHA 的 artifacts；
5. 完成 publish/rollback/cleanup 与 Recovery 异常矩阵；
6. 证据齐全后才能把正式判定改为 `Accepted`；
7. M18 收口独立完成后，Reader 10M fresh-open 性能进入后续分支。

## 11. 关联入口

- [M18 Writer 执行进度](18_writer执行进度.md)
- M18 Writer main 验收记录已移除。
- [M18 正式收口与 Reader 性能优化计划](19_M18正式收口与Reader性能优化计划.md)
- [Writer Roadmap](../roadmap/writer-roadmap.md)
- [Writer 生命周期](../architecture/writer-lifecycle.md)
- [Writer Design Principles](../architecture/writer-design-principles.md)
- [Writer Known Limitations](../architecture/writer-known-limitations.md)
- [当前 ADR 索引](../../../governance/adr/README.md)
- [Writer Transaction Design](../design/writer-transaction.md)
- [Writer Public API Freeze Review](../reviews/API_FREEZE_REVIEW.md)
