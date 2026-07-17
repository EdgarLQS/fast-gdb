# M18 Writer 执行进度

- 更新日期：2026-07-16
- 执行分支：`codex/m18-1-macos-test-contract-ci`
- 关联 PR：#11
- 基线：`main@373bb265212a04fe20e44b8b8d8bb0b64e2e8faf`
- 当前结论：**Writer 专项本地门禁已通过；完整仓库门禁和正式验收仍需补充证据。**

## 阶段状态

| 阶段 | 实现状态 | 自审状态 | 当前验收状态 | 说明 |
|---|---|---|---|---|
| M18.1 macOS 测试契约 | 已实现 | 已完成 | Blocked | schema-v2 runner、合同及手工门禁已建立 |
| M18.2 稳定 API 与安装面 | 已实现 | 已完成 | Blocked | `WriterSession`、错误模型、稳定/legacy 安装面已冻结 |
| M18.3 macOS 性能专项 | 测量基础设施已实现 | 已完成 | Blocked | current/main/GDAL、5% 门禁和 `sample(1)` 链路已建立 |
| M18.4.1 非空追加 | 已实现 | 已完成 | Blocked | staging Append、FID 单调、回读、索引和回滚合同已实现 |
| M18.4.2 Update | 已实现 | 已完成 | Blocked | Binary 字节校验、commit-time 逐 FID 重开验证和包装编译隔离已完成 |
| M18.4.3 Delete | 已实现 | 已完成 | Blocked | FID 保留、全删、范围、索引查询和回滚合同已实现 |
| M18.4.4 事务/崩溃恢复 | 已实现 | 已完成 | Blocked | 统一工作 GDB、单次源发布、显式恢复状态与动作已实现 |

## 最终交付

- 稳定公共头：`writer_session.h`、`writer_recovery.h`。
- GDAL 公共头：`writer_index.h`、`writer_append.h`、`writer_update.h`、`writer_delete.h`、`writer_transaction.h`。
- 错误模型：`WriterErrorCode`、稳定名称及旧 Session 的 `effective_code()`。
- 高级编辑合同：Append、Update、Delete、Transaction/Recovery schema-v2 manifest。
- 独立 macOS workflow：合同、性能、Append、Update、Delete、Transaction/Recovery。
- 自审入口：`docs/reviews/M18-final-writer-self-review.md`。

## 安全边界

- 单 Writer、单源 GDB；不支持嵌套事务、savepoint、跨 GDB 或分布式事务。
- 所有编辑先作用于完整 sibling staging/working GDB。
- 真实 source 仅在验证和冲突检查完成后发布。
- 发布失败尝试 backup rollback。
- Recovery 在候选不唯一或组合不明确时返回 Ambiguous，不自动覆盖健康 source。
- 发布窗口不承诺并发 Reader 连续性。
- Linux、Windows、50M、原生曲线和 MultiPatch 不计入当前完成率。

## 当前外部阻塞

GitHub Actions 在 checkout 前失败，多个 workflow 返回：

- `status=completed`
- `conclusion=failure`
- `steps=null`
- `logs_url=null`

问题记录于 Issue #12。因此：

- 本地 Unix Makefiles 的 GDAL Release 构建、Writer 专项和高级合同已通过；
- GitHub Actions 的 Ninja workflow、runner 环境和正式 artifact 仍未覆盖；
- 安装消费、性能门禁或 raw profile 的正式 artifact 仍待 CI 生成；
- PR #11 保持 Draft，审查结论保持 Request Changes。

## CI 恢复后的唯一剩余工作

1. 运行基础 Writer 合同三次并生成 artifact。
2. 运行 Append、Update、Delete、Transaction/Recovery 合同三次。
3. 运行无 GDAL/GDAL package consumer。
4. 生成 current/main/GDAL 性能和 raw profile artifact。
5. 执行 publish/rollback/cleanup 故障注入。
6. 汇总 evidence，复核后将 PR 转为 Ready for Review。

除上述 CI 运行证据外，M18 当前计划内实现任务已完成。
