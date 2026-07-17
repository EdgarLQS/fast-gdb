# M18 Writer 最终自审

> **历史快照**：本文记录 2026-07-16 原功能分支自审结论。M18 随后由 squash 提交 `9dd7edf73763b56d84677c8a246bc85f80a1a0c1` 进入 `main`，PR #11 已作为 superseded 关闭。当前正式状态以 `docs/evidence/M18-writer-main-acceptance-2026-07-17.md` 为准。

- 日期：2026-07-16
- 历史分支：`codex/m18-1-macos-test-contract-ci`
- 历史 PR：#11（现已关闭）
- 当时结论：**代码与合同实现完成；正式验收仍被 GitHub Actions 外部故障阻塞。**

## 1. 已完成范围

- M18.1：schema-v2 测试合同、统一 runner、macOS workflow。
- M18.2：稳定 `WriterSession`、最小安装面、legacy 隔离。
- M18.3：current/main/GDAL 性能比较、5% 回退门禁、macOS profile 链路。
- M18.4.1：非空顺序 Append。
- M18.4.2：staged Update，包括 Binary 字节级回读和 commit-time 逐 FID 重开验证。
- M18.4.3：staged Delete，包括全删、范围和索引查询合同。
- M18.4.4：统一 `WriterTransaction`，在私有工作 GDB 中组合 Append/Update/Delete，对真实源只发布一次。
- 崩溃恢复：显式检查 source/working/backup 状态；只执行调用方明确选择的安全恢复动作。
- 错误模型：稳定 `WriterErrorCode`、字符串名称和兼容旧 Session 的 `effective_code()`。

## 2. 本轮发现并修复

1. Update 公共头泄露测试依赖。
2. Update Binary 只验证类型、未验证字节内容。
3. Update commit 只验证行数、未逐个确认目标 FID。
4. Update 包装器与原 `.cpp` 重复编译风险。
5. Update 校验状态使用进程静态映射，析构路径可能残留。
6. Delete manifest 与 Google Test fixture 名称不一致。
7. Delete 索引验收只看文件存在不足以排除陈旧命中。
8. 事务源发布前缺少 source fingerprint 冲突检查。
9. 恢复候选多于一个时可能误自动选择。

上述问题均已有代码或合同修正。

## 3. 状态机与安全结论

- 所有编辑都先发生在 sibling staging/working GDB。
- 首次失败锁定对应 Session/Transaction；只允许诊断及 abort。
- 真实 source 在验证前不修改。
- publish 第二步失败时尝试 backup rollback。
- Recovery 在 Ambiguous 状态下拒绝自动处理。
- 不支持嵌套事务、savepoint、跨 GDB 事务、并发 Writer。
- 发布窗口不承诺并发 Reader 连续性。

## 4. API Freeze 结论

稳定安装面：

- 无 GDAL：`writer_session.h`、`writer_recovery.h`。
- GDAL：额外安装 `writer_index.h`、`writer_append.h`、`writer_update.h`、`writer_delete.h`、`writer_transaction.h`。
- 物理布局、RowBuffer、TablxWriter 和原始 table Writer 不进入稳定目录。

兼容性说明：早期 Session 未逐分支填写 `WriterError.code` 时，`effective_code()` 返回 `Unknown`，调用方无需解析 message。新 Transaction/Recovery 路径使用明确错误码。

## 5. 验证证据边界

历史本地 Unix Makefiles 记录了以下等价验证结果：

- macOS GDAL ON/OFF Release 编译和链接通过；
- required contract 连续三次 PASS；
- 无 GDAL、GDAL 和 legacy package consumer 编译、链接和运行通过。

以下不是代码缺项，而是当时及当前仍未闭环的正式 GitHub Actions 验收证据：

- Ninja workflow 和 GitHub runner artifact；
- current/main/GDAL 真实性能 artifact；
- raw `sample(1)` profile；
- 故障注入下的 publish/rollback/cleanup artifact。

GitHub Actions 当时在首个 step 前失败，详见 Issue #12。该历史结论解释了原 PR 的 Draft / Request Changes 状态，但不再要求已关闭的 PR #11 保持 Draft，也不改变当前 `Code accepted / Formal acceptance blocked` 判定。

## 6. 历史合并建议与当前处置

当时建议：**Request Changes（仅因缺少当前 CI 证据）**。

当前处置：M18 已由 squash 提交进入 `main`，原 PR #11 已关闭；Actions 恢复后应在当前 main/收口分支重新运行 Writer contract、Append、Update、Delete、Transaction/Recovery、性能 profile，复核 artifacts 后再决定是否把正式判定改为 `Accepted`。