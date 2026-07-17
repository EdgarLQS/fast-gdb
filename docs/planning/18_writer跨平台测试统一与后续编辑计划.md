# 18 — Writer 跨平台测试统一与后续编辑计划

- **更新日期**：2026-07-16
- **计划状态**：当前执行入口
- **当前执行平台**：macOS
- **暂缓平台**：Linux、Windows；不计入当前里程碑和完成率

> 当前执行状态、阻塞和下一步以 [M18 Writer 执行进度](18_writer执行进度.md) 为准；长期阶段视图见 [Writer Roadmap](../roadmap/writer-roadmap.md)。本计划保留里程碑目标和验收出口，不把静态实现误记为当前运行验收通过。

## 1. 当前基线与支持边界

计划 17 已完成 macOS 上的安全空 schema 批量写、字段/几何矩阵、原子发布、4 GiB 边界、索引重建、Reader → Writer → 索引 → GDAL 闭环、安装消费、1K–10M 基准、磁盘写满和 1800 秒长稳。Reader 已完成 macOS 10M 正确性、8 线程独立 QueryEngine 和 1800 秒长稳，但 Point、MultiPoint、Polyline 的 10M fresh-open 仍有已记录的性能缺口。

当前实现范围已经扩展为：

- 空 schema one-shot `WriterSession`；
- GDAL-only 非空顺序追加 `WriterAppendSession`；
- GDAL-only `WriterUpdateSession`；
- schema 创建、Delete、统一事务、显式崩溃恢复已实现；原生曲线和 MultiPatch 尚未授权；
- Linux、Windows、50M 和跨平台绝对性能比较仍 Deferred。

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
| `R-CONC-*` | Reader 并发 |
| `R-STABLE-*` | Reader 稳定性 |

结果只允许 `PASS`、`FAIL`、`SKIP`；`SKIP` 必须包含结构化原因，且不得计入通过数。证据继续使用 schema v2，至少包含提交、环境、命令、manifest、数据规模、缓存状态、分段耗时、正确性和最终状态。

## 3. M18.1 — macOS 测试契约冻结

### 状态

代码和合同基础设施已实现，等待 GitHub Actions 恢复后生成当前 macOS artifact。

### 验收出口

- Release 构建、Writer/Index/package consumer 全部通过；
- 小型功能矩阵连续执行 3 次结果一致；
- 无无原因 `SKIP`；
- manifest 和 JSON/CSV 可用于后续回归。

## 4. M18.2 — Writer API 与安装面收口

### 状态

`WriterSession`、结构化错误、稳定/legacy 安装面和 package consumer 已实现，等待运行证据。

### 验收出口

- 无 GDAL Writer consumer 可编译运行；
- GDAL 构建只额外暴露批准的 helper/session 头；
- 错误包含阶段、图层、路径和系统原因；
- 旧 API 仍可迁移，新 API 完成空 schema 全流程和失败回滚。

## 5. M18.3 — macOS 性能专项

### 状态

current/main/GDAL 比较器、5% 回退门禁、Writer-only 分段和 macOS `sample(1)` 采集已实现；尚未取得真实 profile artifact，因此没有提交基于猜测的算法优化。

### 验收出口

- 每项优化有 profile、独立提交和 current/main/GDAL 对比；
- 正确性失败时性能结果无效；
- current 相对 main 默认不得回退超过 5%；
- 未达到 GDAL 的场景保留真实结论。

## 6. M18.4 — 高级编辑契约与顺序

能力按“非空追加 → Update → Delete → 事务/崩溃恢复”推进。实现工作可以在 CI 外部阻塞期间继续，但任一能力只有在合同、故障注入、索引一致性和重开验收产生当前证据后才能正式开放。

### 6.1 非空追加

状态：已实现并完成静态自审，待运行证据。

- 完整 sibling staging；
- 原记录/FID 不变；
- 新 FID 单调且不复用空洞；
- 严格字段/几何验证；
- 逐行回读、索引查询、backup/publish/rollback。

### 6.2 Update

状态：已实现，待 CI artifact 验收。

- 全 GDB staging；
- FID 和总记录数保持；
- 未指定字段/几何保持原值；
- 不存在 FID、类型错误和非法几何发布前失败；
- 当前仍需清零 commit 重开逐 FID、Binary byte-for-byte 和公共头测试依赖 3 个 Major。

### 6.3 Delete

状态：已实现，待 CI artifact 验收：

- staging 删除，不直接维护 freelist；
- survivor FID 不变，被删除 FID 不立即复用；
- 范围收缩、索引无残留和全删空表；
- 不存在 FID、重复 FID 和空选择语义由 ADR 冻结。

### 6.4 事务与崩溃恢复

状态：ADR-004 Implemented，待 CI artifact 验收。

- 单 Writer、单源 GDB、第一版无嵌套/savepoint；
- Append/Update/Delete 共享一个 staging，一次发布；
- commit 只在全量重开验证通过后发布；
- 崩溃状态通过 transaction manifest 分类；
- 默认不自动覆盖健康源数据。

## 7. Public API Freeze

API Freeze 审查见 [API_FREEZE_REVIEW](../reviews/API_FREEZE_REVIEW.md)。

冻结候选：

- `WriterSession`
- `writer_index.h`
- `WriterAppendSession`

暂不冻结：

- `WriterUpdateSession`（3 个 Major 未清零）
- Delete/Transaction 最终 C++ 类型布局

Delete 实现前必须统一稳定 `WriterErrorCode`，调用方不得解析 message 文本判断错误类型。

## 8. 当前阻塞

GitHub Actions 当前在 checkout 前失败，多个新旧 workflow 返回 `steps=null` 和 `logs_url=null`。Issue #12 跟踪该仓库/账户/runner 侧问题。

影响：

- PR #11 保持 Draft；
- 审查结论保持 Request Changes；
- 历史结果、静态检查、推算和 `SKIP` 不得替代当前验收；
- 静态实现、设计和文档工作可继续。

## 9. 暂缓事项

- Linux Writer 测试与安装消费；
- Windows UTF-8、锁、排他发布与自动化；
- 跨平台绝对性能比较；
- 50M、35GB、5 亿级生产阶梯；
- 原生曲线和 MultiPatch 写入。

## 10. 下一执行顺序

1. 清零 Update 的 3 个 Major并重新自审；
2. 冻结统一 `WriterErrorCode`；
3. 根据 ADR-004 确认 Delete API；
4. 实现 Delete 合同和 staging 重写；
5. 设计/实现 transaction manifest 和恢复诊断；
6. CI 恢复后依序补齐 M18.1、M18.2、M18.3、Append、Update 当前证据。

## 11. 关联入口

- [M18 Writer 执行进度](18_writer执行进度.md)
- [Writer Roadmap](../roadmap/writer-roadmap.md)
- [Writer 生命周期](../architecture/writer-lifecycle.md)
- [Writer Design Principles](../architecture/writer-design-principles.md)
- [Writer Known Limitations](../architecture/writer-known-limitations.md)
- [Writer ADR 索引](../adr/README.md)
- [ADR-004 Writer Transaction](../adr/ADR-004-writer-transaction.md)
- [Writer Transaction Design](../design/writer-transaction.md)
- [Writer Public API Freeze Review](../reviews/API_FREEZE_REVIEW.md)
