# VersionedGdbStore Known Limitations

- **更新日期**：2026-07-22
- **审查标签**：@深度研究
- **唯一公共入口**：`VersionedGdbStore`
- **状态**：Implemented / Formal acceptance blocked

本文记录当前 Writer Store 明确不支持或尚未完成正式验收的能力。旧 Writer、legacy target、直接 source 替换接口和兼容头已经从公共安装面删除，不再构成支持范围。

本文使用五种状态：

- **Supported**：已实现并有对应契约；
- **Conditionally Supported**：仅在明确条件下支持；
- **Unsupported**：架构明确排除或会破坏不变量；
- **Unspecified**：尚未形成稳定语义，默认按 Unsupported 处理；
- **Internal Only**：可能存在私有实现，但不属于公共 API/ABI。

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

## 2. 架构定位边界

VersionedGdbStore 管理的是 **完整 FileGDB generation 的版本化发布协议**，不是 ArcGIS/GDAL OpenFileGDB 全能力兼容层。

它解决：

- Reader snapshot；
- working generation；
- 单 Writer；
- 发布前校验；
- CURRENT 原子可见性切换；
- 崩溃恢复和保守 GC。

它不解决：

- 字段级 geodatabase 编辑；
- ArcGIS 全部高级对象语义；
- GDAL Dataset transaction 的内部实现；
- 多进程/多主机数据库隔离；
- 网络文件系统或对象存储发布。

GDAL/OpenFileGDB 可以作为 `working_path()` 的编辑器，但 GDAL transaction commit 不等于 Store publish。业务发布成功只能以 `PublishedDurable` 为准。

## 3. 编辑能力

VersionedGdbStore 负责完整 GDB 的版本管理和发布，不提供字段级编辑 DSL。调用方必须使用业务内部编辑器、GDAL 或其他工具生成正确的完整 working GDB。

当前公共 API 不提供：

- schema creation/migration；
- 字段级 Append、Update、Delete；
- FID/ObjectID 分配或空洞复用；
- 原生曲线写入；
- MultiPatch 写入；
- REPACK；
- 索引创建/删除；
- 数据合并和冲突解决。

外部编辑器仅允许修改 working。publish 前必须关闭全部 Dataset、Layer、Feature、SQL result set、cursor、fd、HANDLE、mmap 和外部 lock。

## 4. 并发边界

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
- 容器滚动升级期间多个实例同时挂载并写；
- 绕过托管入口后的安全保证。

路径规范化只能防止同一进程中的符号链接、相对路径和 Windows 大小写别名绕过门禁，不构成外部进程写保护，也不能保证识别所有 junction、UNC、mapped drive、bind mount 或 mount namespace alias。

## 5. Reader 生命周期约束

- snapshot 必须比其派生的 `GdbCatalog`、`QueryEngine`、cursor、GDAL Dataset、fd、HANDLE 和 mmap 活得更久；
- 活动 Reader 不得调用 `refresh()`；
- publish 不会自动切换已有 Reader；
- 调用方直接保存 generation 路径并绕过 snapshot 时，旧版回收可能使该路径失效；
- 同一个 QueryEngine/cursor/Dataset 不应跨线程无约束复用；
- 长时间 Reader 租约会延迟 GC，并可能造成磁盘持续增长。

跨进程直接只读 generation 也不受支持，因为当前 GC 无法感知外部 Reader。

## 6. Writer 生命周期约束

- Writer 只能修改 `working_path()`；
- publish 前必须关闭全部指向 working GDB 的 Writer、Dataset、fd、HANDLE 和 mmap；
- 已发布 generation 绝不能再次修改；
- `publish()==false` 时必须检查 `published()` 或 `publish_state()`；
- `PublishedDurabilityUncertain` 状态下不得 abort、重试、GC 或开始新 Writer；
- recover 前必须释放全部 Reader 和 Writer；
- 析构 abort 只是 best-effort，生产代码应显式 abort 并处理清理错误。

## 7. 文件系统边界

VersionedGdbStore 假设可靠本地文件系统提供：

- 常规文件刷新；
- 目录刷新；
- 同文件系统 rename；
- CURRENT 原子替换；
- 崩溃后的可验证持久化语义；
- 稳定的路径、device/volume identity；
- 可控的文件句柄和删除行为。

明确不支持：

- NFS；
- SMB/CIFS/UNC 网络共享；
- FUSE；
- OneDrive、Google Drive、Dropbox 等云同步目录；
- S3 或其他对象存储；
- 对象存储挂载点；
- ZIP、HTTP、GDAL VSI 写入；
- 跨卷或跨文件系统原子移动；
- 跨主机协调；
- WSL 跨 Windows/Linux 边界路径，除非未来单独验证。

这些不是单纯性能限制，而是正确性边界。strict production mode 应 fail-fast，而不是静默降级。

## 8. CoW 与容量边界

- `clonefile`/`FICLONE` 只是优化；
- 不支持 CoW 时完整复制整个 GDB；
- CoW 成功不代表后续写入空间已预留；
- ENOSPC 可以在 clone 成功后的编辑、索引重建、validator、sync 或 CURRENT tmp 阶段发生；
- 当前没有空间预留、配额、容量预测或自动删除当前版本；
- ENOSPC 必须只影响 working generation，不得破坏 CURRENT；
- 长时间 Reader 租约会延迟旧 generation 回收并增加磁盘占用；
- FullCopy 的时间、空间峰值和功耗可能远高于 CoW；
- 稀疏文件、xattr、ACL 和文件 flags 的跨平台保留语义尚未正式验收。

调用方必须按“完整源 GDB + working 修改放大 + 新旧 generation 保留 + uncertain 恢复余量”的最坏情况规划空间。

## 9. 校验边界

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
- `.spx/.atx` 只能产生候选，最终结果仍需记录/几何复核；
- 自定义 validator 的正确性由调用方负责；
- 外部编辑器尚未关闭时，fresh reopen 也不是稳定候选证明。

当前未提供完整保真证明的能力：

- relationship class 和 attachments；
- domain、subtype、contingent values；
- feature dataset 层级；
- topology、network/utility network、parcel fabric；
- annotation、dimension；
- 原生曲线和 MultiPatch 完整语义；
- raster、raster catalog、mosaic dataset；
- 稀疏 64-bit ObjectID；
- XY/Z/M resolution、tolerance 和 spatial reference 高级元数据；
- 自定义 ArcGIS extension metadata。

在没有 compatibility profile 和真实数据证据前，上述能力状态为 `Unspecified → Unsupported`。

## 10. FID/ObjectID 边界

不承诺：

- FID 从 1 连续到 row count；
- 删除孔洞一定复用或一定不复用；
- 发布前后物理 row slot/offset 不变；
- REPACK 前后 FID 物理布局稳定；
- sparse 64-bit ObjectID 完整支持；
- 与 ArcGIS ObjectID 分配策略完全一致。

逻辑 FID、活动记录数、物理 row slot、删除槽和 max ObjectID 必须分别处理，不能互相替代。

## 11. 几何与空间参考边界

- Point/MultiPoint/Polyline/Polygon 按 Reader 支持矩阵；
- Z/M/ZM 需要对应真实数据测试；
- CircularArc、CubicBezier、EllipticArc 的折线化属于条件/降级支持，结果可能与 GDAL/ArcGIS 不同；
- MultiPatch 仍为 degraded，不承诺完整表面拓扑；
- 原生曲线和 MultiPatch 写入不支持；
- WKB 可解码不等于 precision grid、XYTolerance、Z/M metadata 和 spatial reference 全部正确；
- raster 发布语义当前不支持。

## 12. 事务边界

不支持：

- savepoint；
- 嵌套事务；
- 跨 GDB 事务；
- 分布式事务；
- 多 generation 合并；
- WAL 或业务级增量日志；
- 多 Writer 冲突解决；
- 自动业务回滚。

一个 `GdbWriteTransaction` 的提交单元始终是一个完整 FileGDB generation。

## 13. 恢复边界

- 恢复只信任严格有效的 CURRENT；
- 不根据时间戳、目录名或最大 generation id 猜测最新版；
- CURRENT 非法或指向缺失目录时 fail closed；
- 不确定发布状态下保留新旧 generation；
- 清理失败必须返回错误；
- durability recovery 不等于业务 rollback；
- 当前不提供自动从损坏 CURRENT 中选择候选 generation 的修复工具；
- 手工 rollback 只能在停机、独占、验证和完整持久化 runbook 下进行；
- 外部进程绕过协议后，recovery 不再能保证推理完整。

## 14. Fail-Fast 建议

以下条件应在 strict production mode 直接拒绝：

- 远程/网络/FUSE/云同步/对象存储文件系统；
- work、generations 和 CURRENT 不同 device/volume；
- 无法可靠规范化的 alias、path escape、symlink/junction；
- CURRENT 非法、指向 work、缺失或特殊文件；
- 检测到外部 Writer/owner；
- recover 时仍有 Reader/Writer；
- 目录持久化能力不可用；
- strict profile 发现未支持高级 GDB 对象；
- publish 前 working 仍被外部句柄或 lock 占用。

详细错误分类见 `docs/review/03_明确不支持场景与Fail-Fast策略.md`。

## 15. 可观测性限制

生产接入至少需要记录：

- store/current/working generation；
- transaction id；
- copy mode 和 fallback reason；
- publish phase 和 publish state；
- validator profile 和未支持能力；
- filesystem/device/volume；
- active Reader、最老租约和 pinned bytes；
- free space；
- GC reason/error；
- recovery result。

若没有这些指标，长 Reader、FullCopy 退化、uncertain 和外部误改很难及时发现。

## 16. 平台与验收状态

已完成本地：

- C++17 严格语法检查；
- Reader/Writer smoke；
- 多线程旧/新 generation 可见性；
- 路径别名门禁；
- ASan/UBSan；
- package consumer 更新；
- 核心实现与 latest-only API 三轮自检。

仍待正式证据：

- 完整 CMake/CTest；
- macOS/APFS `clonefile`；
- Linux reflink/非 reflink 矩阵；
- Windows 编译、完整复制、句柄占用和 CURRENT replacement；
- ENOSPC、EIO、权限和持久化阶段故障注入；
- 真实 FileGDB validator 与 GDAL/ArcGIS 对照；
- 长 Reader、容量和 GC 压力；
- 多进程误用 fail-fast；
- 可审计 GitHub Actions logs/artifacts。

因此当前结论保持 **Implemented / Formal acceptance blocked**。

## 17. 深度审查入口

- `docs/review/00_架构自检总览与结论.md`
- `docs/review/01_与GDAL-OpenFileGDB能力和语义对比.md`
- `docs/review/02_当前实现风险陷阱与误用清单.md`
- `docs/review/03_明确不支持场景与Fail-Fast策略.md`
- `docs/review/04_生产运行恢复与故障处理手册.md`
- `docs/review/05_跨平台测试与正式验收矩阵.md`
