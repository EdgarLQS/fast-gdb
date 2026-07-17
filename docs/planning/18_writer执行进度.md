# M18 Writer 执行进度

- 更新日期：2026-07-17
- 收口分支：`codex/m18-main-closeout`
- 实现基线：`main@9dd7edf73763b56d84677c8a246bc85f80a1a0c1`
- 当前 main：`42d8f76620a8c39eeb8523a0f84fcde0eb719f01`
- 原功能 PR：#11（已关闭；由 squash 提交替代）
- 外部阻塞：Issue #12（Open）
- 当前结论：**M18 计划内实现已 squash 合入 main；正式验收仍因缺少当前 main 的 GitHub Actions steps、logs 与 artifacts 而阻塞。**

## 阶段状态

| 阶段 | 实现状态 | 自审状态 | 当前验收状态 | 说明 |
|---|---|---|---|---|
| M18.1 macOS 测试契约 | 已实现并合入 main | 已完成 | Formal acceptance blocked | schema-v2 runner 与合同已进入 main，缺正式 artifact |
| M18.2 稳定 API 与安装面 | 已实现并合入 main | 已完成 | Formal acceptance blocked | `WriterSession`、Recovery、稳定/legacy 安装面已进入 main |
| M18.3 macOS 性能专项 | 基础设施已实现并合入 main | 已完成 | Formal acceptance blocked | current/main/GDAL 与 profile 链路缺当前 SHA artifact |
| M18.4.1 Append | 已实现并合入 main | 已完成 | Formal acceptance blocked | staging Append、回读、索引和回滚合同已实现 |
| M18.4.2 Update | 已实现并合入 main | 已完成 | Formal acceptance blocked | Binary 校验与 commit-time 逐 FID 重开验证已实现 |
| M18.4.3 Delete | 已实现并合入 main | 已完成 | Formal acceptance blocked | FID、全删、范围、索引查询和回滚合同已实现 |
| M18.4.4 Transaction/Recovery | 已实现并合入 main | 已完成 | Formal acceptance blocked | 单工作 GDB、单次发布与显式恢复动作已实现 |

## 已交付实现

- 稳定公共头：`writer_session.h`、`writer_recovery.h`。
- GDAL 公共头：`writer_index.h`、`writer_append.h`、`writer_update.h`、`writer_delete.h`、`writer_transaction.h`。
- 错误模型：`WriterErrorCode`、稳定名称及兼容 `effective_code()`。
- 合同：基础 Writer、Append、Update、Delete、Transaction/Recovery schema-v2 manifests。
- macOS workflows：基础合同、Append、Update、Delete、Transaction/Recovery、性能。
- 自审入口：`docs/reviews/M18-final-writer-self-review.md`。

## 安全边界与 Deferred

- 单 Writer、单源 GDB、单图层事务；不支持嵌套事务、savepoint、跨 GDB 或分布式事务。
- 编辑先作用于 sibling staging/working GDB；真实 source 仅在验证和冲突检查后发布。
- Recovery 在候选不唯一或组合不明确时返回 Ambiguous，不自动覆盖健康 source。
- 发布窗口不承诺并发 Reader 连续性。
- Linux、Windows、50M、35GB/5 亿、原生曲线与 MultiPatch 继续 Deferred。
- Reader 10M fresh-open 性能属于后续独立分支，不属于本分支。

## 2026-07-17 GitHub 状态核实

- PR #11：已关闭，未标记 merged；正文已更新为“由 `main` 上的 squash 提交 `9dd7edf` 替代”，并明确区分实现状态和正式验收状态。
- 原远端分支：按分支名搜索未返回。PR 仍保留历史 head SHA `6b8d3d5565246be5cf306b4d95a4759455bdcbbe`，不再需要把该分支作为当前交付入口。
- Issue #12：Open，继续跟踪 Actions checkout 前失败/无 steps、logs、artifacts 的阻塞。
- `main@42d8f76`：未查询到关联 workflow runs。
- 当前没有可证明 checkout、steps、logs 或六个 macOS artifact 的正式证据。

## 正式验收待补证据

1. GDAL ON/OFF Release 构建与完整 CTest。
2. 五套功能合同各连续三次；性能合同按固定参数运行。
3. 无 GDAL、legacy、GDAL 三类 package consumer。
4. 六个 macOS workflows 的 checkout、steps、logs 与绑定当前 SHA 的 artifacts。
5. publish/rollback/cleanup 故障注入、Recovery 边界与 raw profile。
6. Issue #12 仅在 Actions 恢复并产出上述正式证据后关闭。

## 判定

`Code accepted / Formal acceptance blocked`

不得在上述证据齐全前标记 `Accepted`。