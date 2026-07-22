# ADR-007 — VersionedGdbStore 不可变 generation 发布

- **状态**：Implemented / Awaiting Evidence
- **日期**：2026-07-21
- **更新**：2026-07-22
- **公共实现**：`include/fast_gdb/writer/versioned_gdb_store.h`、`versioned_gdb_validator.h`
- **适用范围**：同一进程多个独立 Reader 与单个 Writer 并发访问本地 FileGDB
- **取代**：ADR-001～ADR-005 的公共 Writer API 和直接 source 发布决策

## 1. 背景

直接修改或替换业务 GDB 目录会产生两个不可接受的问题：

1. 发布窗口中新 Reader 可能找不到完整 source；
2. Writer 可能替换 Reader 正在 mmap 的文件。

因此公共 Writer 模型不再暴露原地编辑、`source → backup → source`、legacy target 或独立 Append/Update/Delete/Transaction。提交单元改为一个完整、不可变的 FileGDB generation。

## 2. 决策

`VersionedGdbStore` 是唯一公共 Writer API。安装包只导出：

- `fast_gdb::writer`；
- `<versioned_gdb_store.h>`；
- `<versioned_gdb_validator.h>`。

旧 Writer 头、legacy target 和兼容别名全部删除，不设置迁移期。

仓库布局：

```text
<store-root>/
├── CURRENT
├── generations/
│   ├── gen-<id>.gdb/
│   └── gen-<id>.gdb/
└── work/
    └── work-gen-<id>.gdb/
```

`CURRENT` 是严格单行 UTF-8 文本，只包含一个 `gen-*.gdb` 目录名和结尾换行。多行、超长、缺少换行、路径穿越或目标缺失均 fail closed。

调用方可以使用业务内部编辑器、GDAL 或其他工具修改 `working_path()`，但公共库不提供字段级编辑兼容接口。

## 3. Reader 快照租约

- `acquire_reader()` 在状态锁下绑定 CURRENT generation 并增加租约；
- snapshot 必须长于其派生的 `GdbCatalog`、`QueryEngine`、cursor、fd 和 mmap；
- 已有 Reader 不自动切换；
- 空闲 Reader 关闭全部派生资源后可 `refresh()`；
- 旧 generation 仅在不是 CURRENT、租约为零且不存在持久性不确定状态时删除。

结果：旧 Reader 连续读取旧版，新 Reader 获取新版，Writer 不触碰任何已发布文件。

## 4. 单 Writer 门禁

- 同一规范化 store root 的实例共享进程内状态；
- `weakly_canonical` 合并符号链接路径别名；
- Windows registry key 进行大小写折叠；
- 任意时刻最多一个 `GdbWriteTransaction`；
- Writer 只能修改 `working_path()`；
- 未发布事务析构或 `abort()` 删除 work 并释放门禁；
- 不提供跨进程锁或选主。

## 5. Working generation

从 CURRENT 创建私有 working GDB：

1. macOS 优先逐文件 `clonefile`；
2. Linux 优先逐文件 `ioctl(FICLONE)`；
3. Windows 或不支持 CoW 时完整复制；
4. 任一文件回退后事务报告 `FullCopy`；
5. working 文件增加 owner read/write 权限；
6. managed GDB 禁止符号链接和特殊文件；
7. working 不得位于 source generation 内部；
8. clone、复制或 ENOSPC 失败不得影响 CURRENT。

CoW 只是性能优化，完整复制是规范路径。

## 6. 发布前验证

`publish()` 强制要求 validator。标准 QueryEngine validator 创建全新 Reader 对象并按配置执行：

- `GdbCatalog::scan()` 和目录 magic；
- `CatalogResolver` 系统目录重开；
- 图层 `QueryEngine` 重开；
- 活动记录数；
- 可选全表扫描；
- 抽样 FID；
- 可选抽样 WKB-first 几何；
- `.spx` 结构解析；
- `.gdbindexes` 元数据与 `.atx` B+ 树解析。

校验失败时 CURRENT 不变，working 可修复后重试 validator 或 abort。

## 7. 持久化与切换顺序

1. 调用方关闭全部 working Writer、fd 和 mmap；
2. validator 重开并通过；
3. 刷新候选常规文件；
4. 同步候选目录树；
5. working 重命名为 immutable generation；
6. 同步 `generations/`；
7. 写入并刷新 `CURRENT.tmp-*`；
8. 原子替换 CURRENT：POSIX `rename`，Windows `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`；
9. 同步 store root；
10. 更新进程内 current generation；
11. 安全回收无租约旧 generation。

第 8 步前失败时 CURRENT 不变。第 8 步成功后不能通过删除新 generation 假装回滚。

## 8. 发布结果

`GdbWriteTransaction` 提供：

- `NotPublished`：CURRENT 未切换；
- `PublishedDurable`：切换与最终持久化成功；
- `PublishedDurabilityUncertain`：CURRENT 已切换，但最终 store root 同步失败。

第三种状态下 `publish()` 返回 `false`，但 `published()==true`。事务已终结，不允许重试或 abort。

## 9. 故障与恢复

### 切换前失败

- CURRENT 仍指向旧版；
- Reader 不受影响；
- working 可修复或 abort；
- 不得删除当前 generation；
- 清理失败必须可诊断。

### CURRENT 已切换但持久性不确定

- 当前进程采用新版；
- 新旧 generation 全部保留；
- Reader 释放不得触发旧版回收；
- 阻止新 Writer；
- 无活动 Reader/Writer 后必须 `recover()`；
- recover 重读 CURRENT，完成目录持久化屏障，再恢复垃圾回收。

### 崩溃恢复

`open()` / `recover()`：

- 创建必要目录；
- 删除 stale work 和 `CURRENT.tmp-*`；
- 严格解析 CURRENT 并确认目标存在；
- 完成目录持久化屏障；
- 清除不确定状态；
- 在无活动租约时删除非 CURRENT generation；
- CURRENT 非法或目标缺失时 fail closed，不猜测最新目录。

## 10. 必要条件

1. 所有访问经 VersionedGdbStore；
2. 同一仓库只允许一个进程执行写入；
3. Reader snapshot 覆盖全部派生资源；
4. 本地文件系统提供可靠刷新、rename 和原子替换；
5. publish 前关闭 working 的全部句柄；
6. validator 覆盖业务关键图层；
7. 容量允许 CoW 增长或完整复制；
8. 不确定状态期间禁止清理旧 generation 和开始新 Writer。

## 11. 非目标

- 旧 Writer API 或兼容层；
- 字段级 Append/Update/Delete 公共接口；
- schema migration；
- 原生曲线或 MultiPatch 写入；
- FID 空洞复用；
- savepoint、嵌套、跨 GDB 或分布式事务；
- 跨进程 Reader 租约或 Writer 锁；
- 多 Writer 合并；
- S3、对象存储或网络对象清单；
- 跨主机一致性；
- 已发布 generation 原地修补。

## 12. 后果

正面：Reader 发布窗口连续可见；mmap 文件不被修改；提交边界明确；CoW 可降低副本成本；公共 API 单一。

代价：调用方必须一次性切换新 API；字段级编辑器由调用方提供；长 Reader 租约保留旧代次；Windows通常完整复制；全树持久化增加延迟。

## 13. 验收

### 公共面

- 安装目录只有两个 Writer 头；
- 导出目标不存在 `writer_legacy`；
- package consumer 只能使用 VersionedGdbStore；
- 文档无旧 API 示例。

### 并发与恢复

- 多 Reader 发布期间保持旧版；
- 新 Reader 获取新版；
- refresh、最后租约回收和路径别名门禁正确；
- 校验、复制、空间和切换前失败保持旧 CURRENT；
- 不确定发布保留新旧 generation并要求 recover；
- 损坏 CURRENT fail closed。

### 三平台

- macOS/APFS `clonefile` 与 full-copy 回退；
- Linux reflink 与非 reflink；
- Windows full-copy、大小写门禁和 `MoveFileExW`；
- ENOSPC 和 crash-phase 故障注入；
- 真实 FileGDB validator；
- 完整 CMake/CTest 和 package consumer artifacts。

S3 和对象存储继续排除。
