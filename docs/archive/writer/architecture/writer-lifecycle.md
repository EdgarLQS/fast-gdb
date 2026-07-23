> **历史归档**：本文记录已废弃的 Writer 方案或阶段性证据，不代表当前产品能力。fast-gdb 当前仅提供 Reader；现行边界见 [ADR-007](../../../adr/ADR-007-reader-only-gdal-edit-boundary.md)，归档说明见 [Writer 历史索引](../README.md)。

# Writer 生命周期与发布模型

## 1. 共同原则

所有稳定写能力遵守同一状态机：

```text
constructed
  └─ open
      ├─ active
      │   ├─ mutate rows/features
      │   ├─ validate
      │   ├─ commit → validate/reopen → publish → committed
      │   └─ abort → cleanup → aborted
      └─ failure → locked → abort/destructor only
```

首个失败锁定会话。锁定后不得继续写入或提交，避免后续成功覆盖最初错误。`abort()` 和析构负责清理未发布 staging；成功发布后的会话不可回滚。

## 2. 空 schema WriterSession

用途：向已创建且没有写入/删除历史的空 schema 写入新数据。

```text
empty schema template
  → open staging
  → begin_row / set_* / geometry / end_row
  → close + header/tablx validation
  → reopen validation
  → no-overwrite publish
```

源目标在 commit 前不存在，发布使用排他无覆盖重命名。该入口不具备 schema 创建、非空追加、Update 或 Delete 权限。

## 3. 非空追加 WriterAppendSession

用途：在现有非空图层末尾顺序追加记录。

```text
source GDB
  → complete sibling staging copy
  → append in staging
  → immediate read-back per new FID
  → reopen count/FID/attribute/geometry/index validation
  → source fingerprint check
  → source→backup
  → staging→source
  → remove backup
```

约束：

- 原记录和原 FID 不变；
- 新 FID 严格大于原最大 FID并单调增长；
- 不复用删除空洞；
- 只支持单 Writer；
- 发布窗口不承诺并发 Reader 连续性。

## 4. Update WriterUpdateSession

用途：按现有 FID 更新属性和/或几何。

```text
source GDB
  → complete sibling staging copy
  → locate exact FID
  → apply only explicitly supplied fields/geometry
  → SetFeature in staging
  → immediate read-back
  → commit-time reopen and per-FID validation
  → source fingerprint check
  → backup/publish/cleanup
```

约束：

- FID/ObjectID 和总记录数保持不变；
- 未指定字段和几何保持原值；
- 不存在的 FID、类型错误、Null 约束、非有限数值和非法几何在发布前失败；
- 当前实现仍需完成 Binary 字节级验证和 commit 重开逐 FID 验证。

## 5. Delete 规划模型

第一版 Delete 不原地维护 freelist，采用全 GDB staging 重写：

```text
source GDB
  → complete sibling staging copy or rebuilt staging
  → delete selected FIDs in staging
  → rebuild extent/indexes
  → verify survivors and deleted-FID absence
  → backup/publish/cleanup
```

约束：

- 未删除记录保持 FID；
- 被删除 FID 不立即复用；
- 删除范围极值记录后必须验证范围收缩；
- 空表是合法结果，但 schema 和必要 metadata 必须保留；
- 空选择、重复 FID 和不存在 FID 的语义必须在 ADR 中冻结。

## 6. 发布协议

高级编辑共享以下发布阶段：

1. **Prepare**：完成 staging 写入、flush、close、索引和重开验证。
2. **Conflict check**：确认源 GDB 自 open 后未发生外部变化。
3. **Backup**：将源目录重命名为唯一 backup。
4. **Publish**：将 staging 重命名为源路径。
5. **Cleanup**：删除 backup 并写入证据。

第二次重命名失败时必须尝试 backup→source 回滚。回滚也失败属于不可自动掩盖的严重状态，错误必须同时记录 publish 和 rollback 原因。

## 7. 事务接入点

未来事务不会让 Append/Update/Delete 各自发布。事务对象统一拥有 staging，并将多个编辑动作排入同一 staging：

```text
transaction.open(source)
  → append/update/delete operations
  → validate complete staged dataset
  → transaction.commit() publishes once
```

第一版不支持嵌套事务或 savepoint。单次操作 API 可以作为单操作事务的便捷封装，但其发布语义必须与事务一致。
