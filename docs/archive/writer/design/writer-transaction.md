> **历史归档**：本文记录已废弃的 Writer 方案或阶段性证据，不代表当前产品能力。fast-gdb 当前仅提供 Reader；现行边界见 [ADR-007](../../../governance/adr/ADR-007-reader-only-gdal-edit-boundary.md)，归档说明见 [Writer 历史索引](../README.md)。

# Writer Transaction Design

本文细化 ADR-004，并记录 Delete API、统一 `WriterTransaction` 和显式崩溃恢复的运行时约束。相关能力已实现并完成 Writer 专项本地验证，正式发布验收仍需 GitHub Actions artifact。

## 1. 对象职责

`WriterTransaction` 负责：

- 创建并拥有唯一 staging/backup；
- 记录源 fingerprint 和事务 manifest；
- 向 Append/Update/Delete 操作提供同一 staging 数据集；
- 聚合 affected FID、layer、extent 和 index 验证要求；
- 执行一次 commit 或 rollback；
- 保存首个错误和恢复所需状态。

操作对象不得自行重命名源目录。

## 2. 状态机

```text
Constructed
  → Opening
  → Active
      ├─ operation failure → Locked
      ├─ rollback → RolledBack
      └─ commit
          → Preparing
          → Validating
          → Publishing
          → Committed

Publishing failure
  → rollback publish succeeded → Locked/RolledBack
  → rollback publish failed → RecoveryRequired
```

终态：Committed、RolledBack、RecoveryRequired。Committed 后不可 rollback；RecoveryRequired 下不得自动删除 staging 或 backup。

## 3. 操作语义

### Append

- 新 FID 大于事务打开时和事务内当前最大 FID；
- 多次 append 共享单调分配序列；
- 不复用 Delete 在同一事务中产生的空洞。

### Update

- 目标 FID 必须在事务当前可见状态中存在；
- 可以更新事务内刚 append 的记录；
- 不得更新已标记 Delete 的 FID；
- 未指定字段保持事务当前值。

### Delete

- 目标 FID 必须在事务当前可见状态中存在；
- 删除事务内刚 append 的记录可以优化为取消 append，但最终证据仍需说明该 FID 从未发布；
- survivor FID 不变；
- 被删除 FID 不复用。

## 4. 冲突规则

第一版只做乐观冲突检测：

- open 时记录源 fingerprint；
- commit 前重新计算；
- 不一致则 `SourceChanged`，禁止发布；
- 不提供 merge 或 last-writer-wins。

未来跨进程锁需要独立 ADR，不能隐式加入。

## 5. 验证计划

commit 前验证分为四层：

1. **Operation validation**：每次变更立即按 FID 回读。
2. **Dataset reopen**：关闭并重开完整 staging。
3. **Invariant validation**：数量、FID 集合、属性、几何、范围、schema。
4. **Index validation**：索引存在性及属性/空间查询结果。

任何一层失败均不得进入 BackupSource。

## 6. 事务 Manifest

建议文件：`.fast-gdb-transaction.json`。

最小 schema：

```json
{
  "schema_version": 1,
  "transaction_id": "uuid",
  "phase": "active",
  "source_path": "...",
  "staging_path": "...",
  "backup_path": "...",
  "source_fingerprint": "...",
  "affected_layers": ["layer"],
  "operations": {
    "append": 0,
    "update": 0,
    "delete": 0
  }
}
```

phase 每次迁移后需要 durable flush。manifest 自身写入失败锁定事务。

## 7. 崩溃分类

| 发现状态 | 默认动作 |
|---|---|
| source 存在，staging 存在，backup 不存在 | 未发布事务；允许人工确认后删除 staging |
| source 不存在，backup 存在，staging 存在 | 发布中断；优先恢复 backup 到 source |
| source 存在，backup 存在，staging 不存在 | 可能已发布且清理未完成；验证 source 后再决定删除 backup |
| source、backup、staging 均存在 | 不自动操作，标记 RecoveryRequired |
| 仅 staging 存在且 source 路径缺失 | 不自动发布，要求人工恢复 |

恢复工具必须先生成只读诊断报告，再执行显式动作。

## 8. API Freeze 前问题

以下问题必须在实现前回答：

- Delete 遇到不存在 FID 是整事务失败还是幂等忽略；
- 空操作 commit 是成功 no-op 还是错误；
- 一个事务是否第一版只允许单层；
- transaction id 和 manifest durable flush 的跨平台实现；
- backup cleanup 失败后 committed 状态如何表达；
- `abort()` 与 `rollback()` 是否保留一个公共名称或分别用于未发布/发布恢复。

## 9. 测试矩阵

至少覆盖：

- Append+Update；
- Update+Delete；
- Append 后 Delete；
- 多次 Update 同一 FID；
- 删除范围极值导致 extent 收缩；
- 属性和空间索引查询；
- source changed；
- prepare/close/reopen/index/publish/rollback/cleanup 故障注入；
- 每个 crash phase 的恢复分类；
- abort 和析构清理。
