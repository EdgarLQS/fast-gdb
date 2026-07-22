# Writer 生命周期与发布模型

- **最后更新**：2026-07-22
- **相关决策**：ADR-001、ADR-002、ADR-003、ADR-004、ADR-005、ADR-007

## 1. 架构分层

Writer 生命周期现在分为两个正交层次：

```text
编辑层
  WriterSession / Append / Update / Delete / WriterTransaction
  └─ 负责 working 数据集中的字段、几何、FID、索引和事务语义

发布层
  VersionedGdbStore / GdbWriteTransaction / GdbReaderSnapshot
  └─ 负责 Reader 快照、单 Writer 门禁、generation、验证和 CURRENT 切换
```

编辑层决定“候选 GDB 内容是否正确”；发布层决定“候选 GDB 如何对并发 Reader 生效”。
`VersionedGdbStore` 不替代 Append、Update、Delete 或业务事务，也不增加新的 FileGDB 写入类型。

## 2. 编辑会话共同原则

所有稳定编辑能力遵守首错锁定和未发布清理原则：

```text
constructed
  └─ open
      ├─ active
      │   ├─ mutate rows/features
      │   ├─ validate
      │   ├─ finish candidate
      │   └─ abort → cleanup → aborted
      └─ failure → locked → abort/destructor only
```

首个失败锁定会话。锁定后不得继续写入或提交，避免后续成功覆盖最初错误。编辑会话只应操作 staging/working GDB。

## 3. 空 schema WriterSession

用途：向已创建且没有写入/删除历史的空 schema 写入新数据。

```text
empty schema template
  → open staging
  → begin_row / set_* / geometry / end_row
  → close + header/tablx validation
  → reopen validation
  → no-overwrite publish to a new path
```

`WriterSession::commit(final)` 只发布到不存在的新目标。它适合新建或离线全量生成，不替换已有 source，也不提供并发版本仓库。

## 4. Append、Update、Delete 和 WriterTransaction

这些 API 负责已有 GDB 的内容编辑：

- Append：保留原 FID，新 FID 单调增长，不复用删除空洞；
- Update：保持 FID/ObjectID 和未指定字段；
- Delete：保留 survivor FID，不立即复用被删除 FID；
- WriterTransaction：将多个编辑动作组合到一个 working 数据集。

它们在候选发布前必须完成：

1. 操作级即时回读；
2. 完整 working GDB 重开；
3. 记录数、FID、属性、几何和范围验证；
4. 必要索引重建与查询验证；
5. 源变化冲突检查（使用直接 source 发布时）。

## 5. 两种发布协议

### 5.1 新目标无覆盖发布

用于 `WriterSession` 新建数据：

```text
staging → validate → rename to non-existing final path
```

目标已存在时失败，不覆盖任何已有目录。

### 5.2 直接 source 替换发布

Append/Update/Delete/WriterTransaction 的既有独立模式采用：

```text
source GDB
  → complete sibling working copy
  → edit + reopen validation
  → source fingerprint check
  → source → backup
  → working → source
  → remove backup
```

该模式适用于离线或能够暂停 Reader 的场景。它具备冲突检测、rollback 和显式 recovery，但**不承诺目录切换窗口内 Reader 连续可见，也不能让 Writer 修改 Reader 正在 mmap 的同一 source 文件**。

### 5.3 generation 版本发布

需要同一进程多 Reader + 单 Writer 时，推荐 ADR-007：

```text
CURRENT generation
  → begin_write
  → CoW/full-copy private working GDB
  → existing Writer APIs edit working_path()
  → close every Writer/fd/mmap
  → fresh Reader reopen validation
  → promote immutable generation
  → atomically replace CURRENT
```

Reader 语义：

- 已有 Reader 的 `GdbReaderSnapshot` 固定旧 generation；
- 发布后新 Reader 获取新 generation；
- 空闲 Reader 在关闭其 QueryEngine、cursor 和 mmap 后显式 `refresh()`；
- 旧 generation 仅在不是 CURRENT 且租约归零后回收。

## 6. VersionedGdbStore 状态机

```text
store.open()
  ├─ acquire_reader() → snapshot(old/current generation)
  └─ begin_write() → exclusive writer transaction
       ├─ edit working_path()
       ├─ validation failure → NotPublished → retry validation or abort
       ├─ publish durable → PublishedDurable
       └─ CURRENT switched, final barrier failed
            → PublishedDurabilityUncertain
            → preserve old + new generations
            → block new Writer
            → recover() with no active readers/writer
```

`publish()==false` 不一定表示 CURRENT 未切换。调用方必须同时检查 `published()` 或 `publish_state()`。

## 7. 持久化顺序

版本发布固定遵守：

1. 编辑层关闭所有指向 working GDB 的句柄；
2. validator 使用全新 Reader 重开候选；
3. 刷新候选文件和目录；
4. working 重命名为 immutable generation；
5. 同步 `generations/`；
6. 写入并刷新临时 `CURRENT`；
7. 原子替换 `CURRENT`；
8. 同步 store root；
9. 更新进程内 current generation；
10. 在安全条件下回收旧 generation。

在第 7 步前失败时，旧 CURRENT 保持权威；第 7 步成功、第 8 步失败时进入 `PublishedDurabilityUncertain`，不得删除新旧 generation 或继续写入。

## 8. 恢复模型

### 直接 source 发布

继续使用 `writer_recovery.h` 检查 source、working/staging 和 backup 组合。恢复默认不覆盖健康 source，候选歧义时拒绝猜测。

### generation 发布

`VersionedGdbStore::open()` / `recover()`：

- 清理 stale work 和临时清单；
- 严格解析 CURRENT；
- 确认 CURRENT generation 存在；
- 不根据时间戳猜测最新 generation；
- 不确定发布时先完成持久化屏障；
- 无活跃 Reader/Writer 后才允许恢复和旧代次回收。

## 9. 事务接入点

业务事务仍由 `WriterTransaction` 负责 Append/Update/Delete 的组合语义。需要连续 Reader 时，事务的真实目标应是 `GdbWriteTransaction::working_path()`，事务自身不得再替换版本仓库的 CURRENT generation。

```text
VersionedGdbStore.begin_write()
  → WriterTransaction / supported edit operations on working_path()
  → validate complete working dataset
  → close edit transaction
  → GdbWriteTransaction.publish() once
```

第一版仍不支持嵌套事务、savepoint、跨 GDB 或分布式事务。

## 10. 选择规则

| 场景 | 推荐发布模型 |
|---|---|
| 新建、不覆盖任何已有路径 | `WriterSession::commit(new_path)` |
| 离线维护，Reader 可暂停 | 既有直接 source 替换 + recovery |
| 同一进程 Reader 必须连续可见 | `VersionedGdbStore` generation 发布 |
| 跨进程或跨主机并发 | 当前不支持，需独立锁/租约 ADR |
| S3/对象存储 | 当前不支持，不能复用本地 rename 协议 |
