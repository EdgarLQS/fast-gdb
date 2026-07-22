# Writer Known Limitations

- **更新日期**：2026-07-22
- **当前开发基线**：`agent/versioned-gdb-store`
- **状态**：Implemented / Formal acceptance blocked

本文集中记录当前明确不支持、只在特定入口支持或尚未正式验收的能力。这里的限制优先于示例代码、历史计划和 self-review 中的推断。

## 1. 两类发布保证不能混用

Writer 目前存在两种发布模型：

1. Append/Update/Delete/WriterTransaction 的既有直接 source 替换；
2. ADR-007 的 `VersionedGdbStore` generation 发布。

只有第二种模型在满足全部接入条件时，才保证同一进程中的旧 Reader 连续读取旧版、新 Reader 获取新版。直接 `source → backup → source` 发布仍不承诺 Reader 连续可见。

## 2. 平台与正式验收

- Reader 和基础库支持 Windows、Linux、macOS；
- VersionedGdbStore 已实现 macOS `clonefile`、Linux `FICLONE` 和完整复制回退代码；
- 当前只完成 Linux 本地严格编译检查、smoke、并发检查和 sanitizer；
- macOS/APFS、Linux reflink/非 reflink、Windows `MoveFileExW` 仍需真实运行证据；
- ENOSPC、目录同步、原子切换和崩溃阶段故障注入尚未形成正式 artifact；
- 50M、35GB 和 5 亿级生产数据不计入当前里程碑；
- 不承诺跨平台绝对性能一致。

## 3. 数据模型和编辑能力

- `WriterSession` 只处理预先创建的 pristine empty schema；
- 高级 Append/Update/Delete/Transaction 继续受各自 ADR 约束；
- 不提供通用 schema creation/migration；
- 不支持原生曲线写入；
- 不支持 MultiPatch 写入；
- Append、Delete 不复用 FID/ObjectID 空洞；
- Update 不允许修改 FID/ObjectID；
- `VersionedGdbStore` 不改变上述编辑边界。

## 4. 并发边界

### 已支持

- 同一进程、同一规范化 store root；
- 多个独立 Reader 快照；
- 最多一个 Writer；
- 旧 Reader 固定旧 generation；
- 新 Reader 获取 CURRENT；
- 空闲 Reader 显式 refresh。

### 不支持

- 跨进程 Reader 租约；
- 跨进程 Writer 锁或选主；
- 多 Writer 排队、合并或 last-writer-wins；
- 绕过 `VersionedGdbStore` 直接打开或修改 generation 后仍获得安全保证；
- Reader 在活动 QueryEngine、cursor、fd 或 mmap 未关闭时 refresh。

路径别名门禁只解决同一进程内规范化路径、符号链接和 Windows 大小写别名问题，不构成恶意进程或外部工具写保护。

## 5. 文件系统边界

VersionedGdbStore 假设本地文件系统提供可靠的：

- 常规文件刷新；
- 目录刷新；
- 同文件系统目录重命名；
- `CURRENT` 原子替换；
- 崩溃后的持久化语义。

明确不承诺：

- S3 或其他对象存储；
- 网络对象清单；
- 不可靠 NFS/SMB 语义；
- 跨卷或跨文件系统原子移动；
- 跨主机一致性。

CoW 只是优化。`clonefile`/`FICLONE` 不支持时会完整复制，因此容量规划必须允许完整 generation 副本。当前不提供空间预留、配额、自动腾挪或容量预测。

## 6. 不可变 generation 约束

- 已发布 generation 不得再被任何 Writer 修改；
- managed GDB 中的符号链接和特殊文件会被拒绝；
- Writer 只能修改 `GdbWriteTransaction::working_path()`；
- publish 前必须关闭全部 Writer、fd 和 mmap；
- 所有业务 Reader 必须从 `GdbReaderSnapshot::path()` 打开；
- 外部程序直接修改 `generations/` 或 `CURRENT` 不在保护范围内。

## 7. 校验边界

标准 validator 可以检查目录 magic、系统目录、记录数、全表扫描、抽样 FID、抽样几何、`.spx` 和 `.atx` 结构。

限制：

- validator 只验证调用方配置的 layer rules；
- 未列出的图层不自动获得业务级等价保证；
- 抽样 FID/几何不是全量语义证明；
- 索引结构可解析不等同于所有业务查询组合均已验收；
- 真实 FileGDB 记录/FID/几何/索引发布矩阵仍待正式证据。

## 8. 发布结果和恢复

`publish()==false` 可能有两种完全不同的状态：

- `NotPublished`：CURRENT 未切换，可修复候选或 abort；
- `PublishedDurabilityUncertain`：CURRENT 已切换，但最终 store root 持久化屏障失败。

后一状态下：

- 事务已经终结；
- 不允许 abort 或重试；
- 新旧 generation 都必须保留；
- 阻止新 Writer；
- 释放全部 Reader 后调用 `recover()`。

`recover()` 不根据目录时间戳猜测最新版。CURRENT 非法、缺失引用或候选歧义时 fail closed。

## 9. 事务边界

- 不支持嵌套事务；
- 不支持 savepoint；
- 不支持跨 GDB、跨进程、跨主机或分布式事务；
- `WriterTransaction` 的业务编辑语义与 VersionedGdbStore 的发布语义是两层对象；
- 需要连续 Reader 时，WriterTransaction 只能操作版本事务的 working path，不能自行替换 CURRENT generation。

## 10. 安装面

无 GDAL 稳定安装面包括：

- `writer_session.h`；
- `writer_recovery.h`；
- `versioned_gdb_store.h`；
- `versioned_gdb_validator.h`。

GDAL 构建额外暴露索引、Append、Update、Delete 和 WriterTransaction API。内部 `versioned_gdb_store_internal.h`、物理布局头和实现拆分头不得安装到稳定或 legacy 公共目录。

## 11. 当前正式判定

VersionedGdbStore 已完成三轮代码自检和本地验证，但以下未闭环：

- 完整 CMake/CTest；
- 三平台真实运行；
- CoW/完整复制矩阵；
- ENOSPC 和崩溃阶段故障注入；
- 真实 FileGDB validator 验收；
- 可审计 GitHub Actions logs/artifacts。

因此当前结论保持 **Implemented / Formal acceptance blocked**。不得把本地自检等同于生产级跨平台发布验收。
