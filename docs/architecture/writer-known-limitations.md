# VersionedGdbStore Known Limitations

- **更新日期**：2026-07-22
- **唯一公共入口**：`VersionedGdbStore`
- **状态**：Implemented / Formal acceptance blocked

本文记录当前 Writer Store 明确不支持或尚未完成正式验收的能力。旧 Writer、legacy target、直接 source 替换接口和兼容头已经从公共安装面删除，不再构成支持范围。

## 1. 公共 API 边界

安装包只提供：

- `versioned_gdb_store.h`；
- `versioned_gdb_validator.h`；
- `fast_gdb::writer`。

不提供：

- 旧 WriterSession；
- 独立 Append/Update/Delete 公共接口；
- 旧 WriterTransaction；
- `fast_gdb::writer_legacy`；
- 旧 recovery/index 公共头；
- 任何兼容别名或过渡 target。

源码树中可能保留供内部实现或历史测试使用的私有代码，但它不属于 API、ABI 或安装承诺。

## 2. 编辑能力

VersionedGdbStore 负责完整 GDB 的版本管理和发布，不提供字段级编辑 DSL。调用方必须使用业务内部编辑器、GDAL 或其他工具生成正确的完整 working GDB。

当前公共 API 不提供：

- schema creation/migration；
- 字段级 Append、Update、Delete；
- FID/ObjectID 分配或空洞复用；
- 原生曲线写入；
- MultiPatch 写入；
- 数据合并和冲突解决。

## 3. 并发边界

已支持：

- 同一进程；
- 同一规范化 store root；
- 多个独立 Reader snapshot；
- 最多一个 Writer；
- 旧 Reader 跨发布继续读取旧 generation；
- 新 Reader 获取 CURRENT；
- 空闲 Reader 显式 refresh。

不支持：

- 跨进程 Reader 租约；
- 跨进程 Writer 锁或选主；
- 多 Writer 排队、合并或 last-writer-wins；
- 跨主机一致性；
- 绕过托管入口后的安全保证。

路径规范化只能防止同一进程中的符号链接、相对路径和 Windows 大小写别名绕过门禁，不构成外部进程写保护。

## 4. Reader 生命周期约束

- snapshot 必须比其派生的 `GdbCatalog`、`QueryEngine`、cursor、fd 和 mmap 活得更久；
- 活动 Reader 不得调用 `refresh()`；
- publish 不会自动切换已有 Reader；
- 调用方直接保存 generation 路径并绕过 snapshot 时，旧版回收可能使该路径失效。

## 5. Writer 生命周期约束

- Writer 只能修改 `working_path()`；
- publish 前必须关闭全部指向 working GDB 的 Writer、fd 和 mmap；
- 已发布 generation 绝不能再次修改；
- `publish()==false` 时必须检查 `published()` 或 `publish_state()`；
- `PublishedDurabilityUncertain` 状态下不得 abort、重试或开始新 Writer；
- recover 前必须释放全部 Reader 和 Writer。

## 6. 文件系统边界

VersionedGdbStore 假设可靠本地文件系统提供：

- 常规文件刷新；
- 目录刷新；
- 同文件系统 rename；
- CURRENT 原子替换；
- 崩溃后的持久化语义。

不承诺：

- S3 或其他对象存储；
- NFS/SMB 等不可靠网络文件系统；
- 跨卷或跨文件系统原子移动；
- 网络对象清单；
- 跨主机协调。

## 7. 容量边界

- `clonefile`/`FICLONE` 只是优化；
- 不支持 CoW 时完整复制整个 GDB；
- 当前没有空间预留、配额、容量预测或自动删除当前版本；
- ENOSPC 必须只影响 working generation，不得破坏 CURRENT；
- 长时间 Reader 租约会延迟旧 generation 回收并增加磁盘占用。

## 8. 校验边界

标准 validator 可以验证：

- 目录 magic；
- 系统目录和图层；
- 活动记录数；
- 全表扫描数；
- 抽样 FID；
- 抽样 WKB-first 几何；
- `.spx`；
- `.gdbindexes` 与 `.atx`。

限制：

- 只验证调用方配置的 layer rules；
- 抽样 FID/几何不是全量业务语义证明；
- 索引结构可解析不等于所有业务查询均已验收；
- 自定义 validator 的正确性由调用方负责。

## 9. 事务边界

不支持：

- savepoint；
- 嵌套事务；
- 跨 GDB 事务；
- 分布式事务；
- 多 generation 合并；
- WAL 或业务级增量日志。

一个 `GdbWriteTransaction` 的提交单元始终是一个完整 FileGDB generation。

## 10. 恢复边界

- 恢复只信任严格有效的 CURRENT；
- 不根据时间戳、目录名或最大 generation id 猜测最新版；
- CURRENT 非法或指向缺失目录时 fail closed；
- 不确定发布状态下保留新旧 generation；
- 清理失败必须返回错误；
- 当前不提供自动从损坏 CURRENT 中选择候选 generation 的修复工具。

## 11. 平台与验收状态

已完成本地：

- C++17 严格语法检查；
- Reader/Writer smoke；
- 多线程旧/新 generation 可见性；
- 路径别名门禁；
- ASan/UBSan；
- package consumer 更新。

仍待正式证据：

- 完整 CMake/CTest；
- macOS/APFS `clonefile`；
- Linux reflink/非 reflink 矩阵；
- Windows 编译、完整复制和 `MoveFileExW`；
- ENOSPC 和持久化阶段故障注入；
- 真实 FileGDB validator 验收；
- 可审计 GitHub Actions logs/artifacts。

因此当前结论保持 **Implemented / Formal acceptance blocked**。
