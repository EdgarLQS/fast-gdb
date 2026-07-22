# VersionedGdbStore Roadmap

- **更新日期**：2026-07-22
- **唯一公共 Writer API**：`VersionedGdbStore`
- **当前结论**：Implemented / Formal acceptance blocked

## 状态图例

- ✅ 已实现并取得正式证据
- ⚠️ 已实现，本地验证完成，正式证据未闭环
- 🚧 正在开发或存在未处理 Major
- 🧭 已设计，尚未实现
- 🧊 Deferred / Out of scope

## 当前路线图

| 能力 | 状态 | 公共入口/证据 | 下一出口 |
|---|:---:|---|---|
| 唯一 Writer 公共安装面 | ⚠️ | `fast_gdb::writer`、两个 versioned 头 | 安装后 package consumer 实际构建运行 |
| 不可变 generation | ⚠️ | `VersionedGdbStore`、ADR-007 | 完整 CMake/CTest 与真实 GDB |
| Reader snapshot lease | ⚠️ | `GdbReaderSnapshot` | 长扫描、mmap、refresh 并发矩阵 |
| 单 Writer 门禁 | ⚠️ | canonical root registry | Windows 大小写、junction、路径矩阵 |
| macOS CoW | ⚠️ | `clonefile` 实现 | APFS 实机 + 强制回退 |
| Linux CoW | ⚠️ | `FICLONE` 实现 | reflink/非 reflink 文件系统矩阵 |
| Windows 完整复制 | ⚠️ | full-copy + `MoveFileExW` 实现 | MSVC/MinGW 实机运行 |
| 强制重开 validator | ⚠️ | record/FID/geometry/spx/atx | 真实 FileGDB 业务规则矩阵 |
| CURRENT 原子切换 | ⚠️ | durable manifest protocol | 分阶段故障注入 |
| 不确定发布恢复 | ⚠️ | `PublishedDurabilityUncertain`、`recover()` | root fsync 失败注入与重启证明 |
| ENOSPC 安全 | 🧭 | 设计保证 CURRENT 不受影响 | 文件/目录/manifest 各阶段注入 |
| 大规模容量与性能 | 🧭 | clone strategy metrics | 1M/10M/50M 和大目录 publish 延迟 |
| 跨进程锁与租约 | 🧊 | 不在 ADR-007 | 独立 ADR |
| 对象存储 | 🧊 | 不在本地 rename 模型 | 独立架构 |
| 内建字段级编辑 API | 🧊 | 已从公共范围删除 | 不兼容恢复；如需重启需独立立项 |

## 已删除的路线

以下能力不再作为兼容或迁移任务保留：

- `fast_gdb::writer_legacy`；
- 旧 WriterSession 公共头；
- 独立 Append/Update/Delete 公共头；
- 旧 WriterTransaction 公共头；
- 直接 `source → backup → source` 公共发布协议；
- legacy package consumer；
- 旧 API deprecation 过渡期。

内部代码是否保留只由实现复用和测试需要决定，不构成 API/ABI 承诺。

## Gate A：公共 API 收敛

- 安装目录仅包含两个 VersionedGdbStore 头；
- 导出目标中不存在 `writer_legacy`；
- `fast_gdb::writer` 不传递源码 writer 目录；
- package consumer 只能编译 versioned API；
- 文档不存在旧 API 使用示例。

## Gate B：并发正确性

- 多 Reader 在 Writer 发布期间持续读取旧 generation；
- 新 Reader 只绑定新版；
- refresh 仅在 Reader 空闲时允许；
- 最后租约释放前旧 generation 不删除；
- 路径别名不能创建第二 Writer；
- Writer 永不修改已发布 generation。

## Gate C：持久化与恢复

- working 文件和目录完成持久化；
- generation promote 后同步 `generations/`；
- 临时 CURRENT 刷新后原子替换；
- store root 同步失败进入明确不确定状态；
- 不确定状态保留新旧 generation并阻止新 Writer；
- recover 不依赖猜测。

## Gate D：跨平台验收

- GDAL ON/OFF Release；
- 完整 CTest；
- no-GDAL 和 GDAL package consumer；
- macOS/APFS clonefile；
- Linux reflink + full-copy fallback；
- Windows full-copy + `MoveFileExW`；
- ASan/UBSan；
- 真实 FileGDB validator；
- ENOSPC 和 crash-phase 故障注入；
- GitHub Actions logs 和绑定 SHA 的 artifacts。

Gate D 未满足前，结论保持 **Implemented / Formal acceptance blocked**。

## 暂不承诺

- 跨进程或跨主机并发；
- 多 Writer；
- S3/对象存储；
- NFS/SMB 上等价持久化；
- schema migration；
- 字段级编辑 DSL；
- savepoint、嵌套事务、跨 GDB 或分布式事务；
- 原生曲线/MultiPatch 写入；
- FID 空洞复用；
- 自动空间预留和配额管理。
