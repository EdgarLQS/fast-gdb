# ADR-007 — VersionedGdbStore 不可变代次发布

- **状态**：Proposed
- **日期**：2026-07-21
- **关联实现**：`versioned_gdb_store.*`、`versioned_gdb_validator.*`
- **适用范围**：同一进程内多个独立 Reader 与单个 Writer 并发访问本地 FileGDB

## 1. 背景

当前 `fast-gdb` 已允许多个独立 `QueryEngine` 并发读取，但 Writer 发布仍以 `source → backup → source` 目录替换为核心。该模式存在两个不可接受的窗口：

1. 发布过程中 `source` 可能暂时不存在，新 Reader 无法连续获得可见版本；
2. Writer 可能修改或替换仍被 Reader mmap 的文件，Windows 上会直接受文件锁限制，POSIX 上也会产生路径与 inode 语义分离、恢复边界不清等问题。

因此，Reader 与 Writer 不能再共享一个可变 GDB 目录。发布单元必须从“原地目录”改为“不可变 generation + 原子清单切换”。

## 2. 决策

新增 `explorgdb::writer::VersionedGdbStore`，作为同一仓库所有 Reader/Writer 的托管入口。

仓库布局固定为：

```text
<store-root>/
├── CURRENT
├── generations/
│   ├── gen-<id>.gdb/
│   └── gen-<id>.gdb/
└── work/
    └── work-gen-<id>.gdb/
```

`CURRENT` 只包含一个 generation 目录名和换行。generation 一经发布即不可修改。

### 2.1 Reader 快照租约

- `acquire_reader()` 在进程内状态锁下读取当前 generation，并增加该 generation 的租约计数；
- `GdbReaderSnapshot::path()` 是 Reader、`GdbCatalog`、`QueryEngine` 和 mmap 的唯一合法打开路径；
- 快照必须长于其上创建的所有 Reader 对象和映射；
- 已有 Reader 永不自动切换 generation；
- 空闲 Reader 可在关闭活动 cursor、文件描述符和 mmap 后显式调用 `refresh()`；
- 旧 generation 只有在不再是 CURRENT 且租约计数归零后才可清理。

结果是：发布前已打开的 Reader 连续读取旧版，发布后新 Reader 获取新版，两者互不修改对方文件。

### 2.2 单 Writer 门禁

- 同一规范化 store root 的所有 `VersionedGdbStore` 实例共享进程内状态；
- 任意时刻最多一个 `GdbWriteTransaction`；
- Writer 只能打开 `working_path()`，不得打开 CURRENT generation 进行修改；
- 未发布事务析构或 `abort()` 会删除 working GDB 并释放门禁；
- 本 ADR 不提供跨进程 Writer 锁。多进程写入必须由更高层独占机制禁止。

### 2.3 Working GDB 创建

Writer 从 CURRENT generation 创建私有 working GDB：

1. macOS：逐文件优先调用 `clonefile`；
2. Linux：逐文件优先调用 `ioctl(FICLONE)`；
3. Windows 或文件系统不支持 CoW：回退 `copy_file` 完整复制；
4. 任一文件回退后，该事务的 `clone_strategy()` 报告 `FullCopy`；
5. 复制、clone 或空间不足失败时，删除不完整 working 目录，CURRENT 与现有 generation 不变。

不依赖 CoW 正确性；CoW 只是性能优化，完整复制是规范回退路径。

### 2.4 发布校验

`publish()` 强制要求 validator，禁止无校验发布。

标准实现 `make_query_engine_generation_validator()` 必须使用全新的对象重开候选 generation，不得复用 Writer 的缓存、句柄或 mmap。按规则执行：

- `GdbCatalog::scan()`；
- 可选目录 magic 校验；
- `CatalogResolver` 重开系统目录；
- 每个图层创建新的 `QueryEngine`；
- 比较活动记录数；
- 可选全表顺序扫描，要求扫描数等于活动记录数；
- 抽样 FID 重开并核对 `record.fid`；
- 抽样几何通过 WKB-first 路径解码，要求 `GeometryValue::valid()`；
- 要求的 `.spx` 非空；
- 要求的属性索引同时存在于 `.gdbindexes` 元数据与非空 `.atx` 文件中。

校验失败时 working 保留，Writer 可修复后重试或 abort；CURRENT 不变。

### 2.5 持久化与原子切换顺序

发布顺序固定为：

1. Writer 已关闭所有指向 working GDB 的句柄；
2. validator 重开并通过；
3. 对候选 generation 中的常规文件执行 `fsync` / `FlushFileBuffers`；
4. 同步候选目录；
5. 将 working 目录重命名为 `generations/gen-<id>.gdb`；
6. 同步 `generations` 目录；
7. 写 `CURRENT.tmp-<id>`，刷新并关闭；
8. 刷新临时清单；
9. 原子替换 `CURRENT`：POSIX `rename`，Windows `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`；
10. POSIX 同步 store root 目录；
11. 在进程内状态锁下更新 `current_generation`，释放 Writer 门禁；
12. 清理无租约旧 generation。

只有第 9 步成功后新版本才对新 Reader 生效。此前所有失败均不得改变 CURRENT。

## 3. 故障与恢复

### 3.1 校验、空间或 I/O 失败

- 当前 generation 不变；
- 已有 Reader 不受影响；
- working 可重试或删除；
- 空间不足不得先删除旧 generation，也不得覆盖 CURRENT。

### 3.2 generation 已提升但 CURRENT 切换失败

- CURRENT 仍指向旧版；
- 新 generation 被视为未发布候选并删除；
- Writer 门禁释放；
- 调用方获得明确失败。

### 3.3 进程崩溃恢复

`open()` / `recover()`：

- 删除 `work/` 中残留事务；
- 删除 `CURRENT.tmp-*`；
- 解析 CURRENT 并确认目标 generation 存在；
- 在无活跃租约的恢复边界删除非 CURRENT generation；
- CURRENT 缺失表示未初始化 store；
- CURRENT 内容非法或目标目录缺失时 fail closed，不猜测最新目录。

该策略假设进程退出后不存在仍有效的本进程 mmap。跨进程 Reader 不在本 ADR 的租约模型内。

## 4. 必要条件

使用方必须同时满足：

1. 所有访问经 `VersionedGdbStore` 托管入口；
2. 同一仓库仅允许一个 Writer；
3. Reader 快照长于其 Reader、cursor 和 mmap；
4. 本地文件系统对文件刷新、目录重命名和清单原子替换提供可靠语义；
5. Writer 发布前关闭 working GDB 的所有句柄；
6. validator 覆盖业务要求的记录数、FID、几何和索引；
7. 容量规划至少允许 CoW 元数据增长或完整复制；
8. 删除旧 generation 只能发生在租约归零后。

## 5. 非目标

- S3、对象存储、网络对象清单；
- 跨主机一致性；
- 跨进程 Reader 租约；
- 跨进程 Writer 选主；
- 多 Writer 合并；
- 对已发布 generation 的原地修补；
- 将 FileGDB 拆成逐文件对象并独立提交。

对象存储需要不同的版本清单、条件写、租约/垃圾回收和一致性模型，不能复用本地目录重命名假设。

## 6. 后果

### 正面

- 发布窗口内 Reader 始终有可见版本；
- mmap 文件永不被 Writer 原地修改；
- Reader 新旧版本切换由快照租约明确表达；
- 校验和 CURRENT 切换形成清晰事务边界；
- CoW 可降低大 GDB working 副本成本；
- 空间不足、校验失败和崩溃不会破坏当前版本。

### 代价

- 所有调用方必须迁移到托管入口；
- 旧 generation 会在 Reader 长租约期间占用空间；
- Windows 默认走完整复制，发布准备时间和容量成本更高；
- `fsync` 全树会增加发布延迟；
- 本阶段仍需上层保证单进程访问边界。

## 7. 验收

### 7.1 并发可见性

- 多个旧 Reader 持续扫描时发布新版；
- 旧 Reader 结果、路径和 mmap 保持旧版；
- 新 Reader 只获取新版；
- idle Reader 显式 refresh 后切换新版；
- 无 Reader 观察到目录不存在或半发布状态。

### 7.2 Writer 门禁与回滚

- 同一 store 的第二 Writer 必须立即失败；
- validator 失败、clone/copy 失败、空间不足、generation rename 失败和 CURRENT 切换失败均保持旧 CURRENT；
- abort/析构删除 working 并释放门禁；
- 发布成功后旧 generation 仅在最后租约释放后删除。

### 7.3 崩溃恢复

分别在以下边界注入终止并重启：

- working 创建中；
- validator 前后；
- generation rename 后；
- CURRENT 临时文件写入后；
- CURRENT 原子替换后；
- 旧 generation 清理前。

恢复后 CURRENT 必须指向完整、已验证 generation，或明确 fail closed；不得指向 working、临时清单或缺失目录。

### 7.4 三平台回退

- macOS/APFS：确认 `CopyOnWrite`，同时强制 clone 失败验证完整复制；
- Linux：在支持 reflink 的文件系统确认 `FICLONE`，在 ext4/不支持环境验证完整复制；
- Windows：确认完整复制、文件句柄关闭约束及 `MoveFileExW` 清单切换；
- 三平台均运行并发、回滚、崩溃恢复和空间不足测试。
