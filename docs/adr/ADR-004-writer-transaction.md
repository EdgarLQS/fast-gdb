# ADR-004 — Writer Transaction Model

- 状态：Proposed
- 日期：2026-07-16
- 决策范围：Append、Update、Delete 的统一 staging、commit、rollback 和崩溃恢复边界

## Context

Append 和 Update 当前各自复制源 GDB、修改 staging、验证并发布。Delete 将采用相同模式。如果继续为每个操作独立扩展发布逻辑，将造成状态机、冲突检测、错误语义和恢复策略重复，且多操作组合会多次切换源目录。

## Decision

第一版事务采用以下约束：

1. 单 Writer、单源 GDB、单事务对象。
2. 一个事务只拥有一个完整 sibling staging 和一个唯一 backup 路径。
3. 不支持嵌套事务、savepoint、跨 GDB 或分布式事务。
4. Append、Update、Delete 只修改事务 staging，不自行发布。
5. `commit()` 仅在全量 close、reopen、FID、属性、几何、范围、索引和源冲突验证通过后执行一次发布。
6. `rollback()`/`abort()` 在发布前删除 staging 并保留源 GDB。
7. 首个失败锁定事务，之后只允许 rollback/abort、状态查询和诊断。
8. 单操作 Session 可以保留为便捷包装，但内部语义等价于单操作事务。

## Proposed Public Shape

最终 C++ 布局尚不冻结，语义建议为：

```cpp
WriterTransaction tx;
tx.open(source_gdb);
tx.append(layer, rows);
tx.update(layer, fid, patch);
tx.erase(layer, fids);
tx.commit();
// or tx.rollback();
```

为避免过早扩大 API，第一版可以只支持一个 layer；多层事务需要新的 ADR。

## Commit Phases

```text
Active
  → PrepareClose
  → ReopenValidate
  → ConflictCheck
  → BackupSource
  → PublishStaging
  → CleanupBackup
  → Committed
```

任一 Prepare/Reopen/Conflict 失败均保持源不变并进入 Locked。Backup 后 Publish 失败必须尝试恢复 backup。Rollback 也失败时进入 RecoveryRequired，禁止自动删除 staging/backup。

## Recovery Records

事务在 staging 根目录写入最小 manifest，至少包含：

- schema version；
- transaction id；
- source/staging/backup canonical path；
- source fingerprint；
- created timestamp；
- current phase；
- affected layers；
- operation summaries；
- expected validation assertions。

manifest 不包含业务字段值，只用于识别和恢复。

## Consequences

### Positive

- Append/Update/Delete 共享同一发布和错误模型；
- 多操作只发布一次；
- 崩溃恢复具有可识别状态；
- 减少复制的发布代码和不一致风险。

### Negative

- staging 需要完整 GDB 额外磁盘空间；
- 事务持续期间源数据不能被其他 Writer 修改；
- 发布窗口仍不承诺并发 Reader 连续性；
- 第一版没有 savepoint，任一操作失败会锁定整个事务。

## Rejected Alternatives

- **原地事务日志**：需要直接维护 FileGDB 物理布局、freelist 和恢复日志，当前风险过高。
- **每个操作独立发布**：无法提供真正的多操作原子性。
- **自动覆盖式崩溃恢复**：恢复信息不足时可能损坏健康源数据。
- **嵌套事务/savepoint**：显著扩大状态空间，不进入第一版。

## Acceptance Conditions

ADR 转为 Accepted 前必须具备：

1. transaction manifest schema；
2. Append+Update+Delete 混合合同；
3. prepare、publish、rollback 和 cleanup 故障注入；
4. crash phase 恢复矩阵；
5. source unchanged / published complete 的二态验收；
6. 当前 macOS required contract artifact。
