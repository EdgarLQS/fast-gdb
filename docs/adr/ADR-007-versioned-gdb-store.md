# ADR-007 — VersionedGdbStore 不可变代次发布

- **状态**：Proposed
- **日期**：2026-07-21
- **关联实现**：`versioned_gdb_store*.{h,cpp}`、`versioned_gdb_validator.*`
- **适用范围**：同一进程内多个独立 Reader 与单个 Writer 并发访问本地 FileGDB

## 1. 背景

现有 Writer 采用 `source → backup → source` 目录替换。该方式存在两个不可接受的窗口：发布期间新 Reader 可能找不到源目录；Writer 也可能修改或替换仍被 Reader mmap 的文件。

因此 Reader 和 Writer 不再共享可变 GDB 目录。发布单元改为“不可变 generation + Reader 快照租约 + 原子 CURRENT 清单”。

## 2. 决策

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

`CURRENT` 必须是严格单行 UTF-8 文本，只包含一个 `gen-*.gdb` 目录名和一个结尾换行。多行、超长、缺少换行、路径穿越或目标 generation 缺失均 fail closed。

### 2.1 Reader 快照租约

- `acquire_reader()` 在状态锁下绑定 CURRENT generation 并增加租约计数；
- 快照必须长于其上创建的 `GdbCatalog`、`QueryEngine`、cursor、文件句柄和 mmap；
- 已有 Reader 不自动切换；
- 空闲 Reader 关闭活动资源后可显式 `refresh()`；
- 旧 generation 只有在不再是 CURRENT、租约为零且不存在持久性不确定状态时才可清理。

因此旧 Reader 连续读取旧版，新 Reader 获取新版，两者不会共享被修改的文件。

### 2.2 单 Writer 门禁

- 同一规范化 store root 的实例共享一个进程内状态和 Writer 门禁；
- 使用 `weakly_canonical` 合并符号链接路径别名；
- Windows 注册键进行大小写折叠，避免 `C:\Data\Store` 与 `c:\data\store` 绕过门禁；
- 任意时刻最多一个 `GdbWriteTransaction`；
- Writer 只能修改 `working_path()`；
- 未发布事务析构或 `abort()` 删除 working GDB 并释放门禁；
- 本 ADR 不提供跨进程锁，多进程写入必须由上层禁止。

### 2.3 Working GDB 创建

Writer 从 CURRENT generation 创建私有 working GDB：

1. macOS 优先逐文件调用 `clonefile`；
2. Linux 优先逐文件调用 `ioctl(FICLONE)`；
3. Windows 或不支持 CoW 时回退完整复制；
4. 任一文件回退后，事务策略报告 `FullCopy`；
5. working 文件显式增加 owner read/write 权限，避免只读发布文件无法编辑；
6. managed GDB 中禁止符号链接和其他特殊文件，避免 generation 通过外部可变目标破坏不可变性；
7. working 目录不得位于 source GDB 内部；
8. clone、复制或空间不足失败时，CURRENT 和现有 generation 保持不变。

CoW 只是性能优化，完整复制是规范回退路径。

### 2.4 发布校验

`publish()` 强制要求重开 validator。标准 QueryEngine validator 使用全新的对象重新打开候选 generation，并按配置执行：

- `GdbCatalog::scan()` 和可选目录 magic 校验；
- `CatalogResolver` 重开系统目录；
- 每个图层创建新的 `QueryEngine`；
- 比较活动记录数；
- 可选全表扫描并核对扫描数；
- 抽样 FID 重开并核对 `record.fid`；
- 仅在要求时通过 WKB-first 路径解码抽样几何；
- 要求的 `.spx` 必须存在并通过 `GdbSpatialIndexParser::parse()`；
- 要求的属性索引必须同时存在于元数据中，并通过 `GdbAttributeIndexParser::parse()` 完整解析 `.atx` B+ 树。

校验失败时 working 保留，事务可修复后重试或 abort，CURRENT 不变。

### 2.5 持久化与切换顺序

发布顺序固定为：

1. 调用方关闭所有指向 working GDB 的 Writer 和句柄；
2. validator 重开并通过；
3. 刷新候选的所有常规文件；
4. 同步候选目录树；
5. 将 working 重命名为不可变 generation；
6. 同步 `generations` 目录；
7. 写入并刷新 `CURRENT.tmp-*`；
8. 原子替换 CURRENT：POSIX `rename`，Windows `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`；
9. 同步 store root 目录；
10. 更新进程内 `current_generation`；
11. 在安全条件成立时清理无租约旧 generation。

第 8 步之前失败时 CURRENT 不变。第 8 步成功后，新版本已对当前进程可见，不能尝试删除新版进行“回滚”。

### 2.6 发布结果状态

`GdbWriteTransaction` 公开：

- `NotPublished`：CURRENT 未切换；
- `PublishedDurable`：CURRENT 切换及最终持久化屏障成功；
- `PublishedDurabilityUncertain`：CURRENT 已切换，但最终根目录同步失败。

`publish()` 在第三种状态返回 `false`，但 `published()==true`。事务已经终结，不允许重试。

## 3. 故障与恢复

### 3.1 切换前失败

校验、clone/copy、空间不足、候选同步、generation 提升或 CURRENT 原子替换失败时：

- CURRENT 仍指向旧版；
- 已有 Reader 不受影响；
- working 可修复、abort 或清理；
- 不得先删除当前或旧 generation；
- 清理失败必须进入诊断，不能静默忽略。

### 3.2 CURRENT 已替换但根目录同步失败

这是“提交结果已切换但持久性不确定”，处理规则为：

- 当前进程的新 Reader 获取新版；
- 新旧 generation 全部保留，Reader 释放不得触发旧代次回收；
- 记录 `PublishedDurabilityUncertain`；
- 阻止新的 Writer；
- 要求在无活动 Reader/Writer 时显式执行 `recover()`；
- `recover()` 重新读取 CURRENT，执行目录持久化屏障，再恢复旧 generation 回收。

这样即使崩溃后文件系统恢复旧 CURRENT，也不会出现旧清单指向已删除 generation 的悬空状态。

### 3.3 崩溃恢复

`open()` 或显式 `recover()`：

- 创建并同步必要目录；
- 删除 stale work 和 `CURRENT.tmp-*`，删除失败即失败；
- 严格解析 CURRENT 并确认目标 generation 存在；
- 完成 `generations`、`work` 和 store root 的目录同步屏障；
- 清除持久性不确定标志；
- 在无活动租约边界删除非 CURRENT generation；
- CURRENT 非法或目标缺失时 fail closed，不猜测“最新”目录。

跨进程 Reader 租约不在本 ADR 范围内。

## 4. 必要条件

使用方必须满足：

1. 所有访问经 `VersionedGdbStore` 托管入口；
2. 同一仓库只允许一个进程执行写入；
3. Reader 快照长于其所有文件资源和 mmap；
4. 本地文件系统提供可靠的文件刷新、目录重命名和清单替换语义；
5. 发布前关闭 working GDB 的所有句柄；
6. validator 覆盖业务所需记录、FID、几何和索引；
7. 容量规划允许 CoW 增长或完整复制；
8. 持久性不确定期间禁止清理旧 generation 和开始新 Writer。

## 5. 非目标

- S3 和对象存储；
- 跨主机一致性；
- 跨进程 Reader 租约；
- 跨进程 Writer 选主；
- 多 Writer 合并；
- 对已发布 generation 原地修补；
- 将 FileGDB 拆分成逐文件对象提交。

对象存储需要条件写、对象版本清单和不同的垃圾回收模型，不能复用本地目录重命名假设。

## 6. 后果

正面结果：Reader 发布窗口连续可见，mmap 文件不被原地修改，发布校验和 CURRENT 切换形成明确事务边界，CoW 可降低 working 副本成本。

代价：所有调用方必须迁移到托管入口；长 Reader 租约和不确定发布状态会保留旧 generation；Windows 通常走完整复制；全树持久化会增加发布延迟。

## 7. 验收

### 7.1 并发可见性

- 多个旧 Reader 持续读取时发布新版；
- 旧 Reader 的路径和内容保持旧版；
- 新 Reader 只获取新版；
- idle Reader 显式 refresh 后切换；
- 无 Reader 观察到目录缺失或半发布状态。

### 7.2 Writer、回滚与恢复

- 同一 store 的第二 Writer 立即失败；
- 真实路径、符号链接别名及 Windows 大小写别名共享门禁；
- 校验、复制、空间和切换前失败保持旧 CURRENT；
- abort/析构清理 working 并释放门禁；
- 不确定发布保留新旧 generation、阻止新 Writer并要求 recover；
- 清理错误可诊断；
- 损坏或多行 CURRENT fail closed。

### 7.3 三平台回退

- macOS/APFS：验证 `clonefile` 和强制完整复制回退；
- Linux：验证支持 reflink 与不支持 reflink 的文件系统；
- Windows：验证完整复制、可写 working 文件、大小写门禁和 `MoveFileExW`；
- 三平台均运行并发、回滚、空间不足和崩溃边界故障注入。

S3 和对象存储继续明确排除。
