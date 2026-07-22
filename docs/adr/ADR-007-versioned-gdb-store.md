# ADR-007 — VersionedGdbStore 不可变 generation 发布

- **状态**：Implemented / Awaiting Evidence
- **日期**：2026-07-21
- **更新**：2026-07-22
- **审查标签**：@深度研究
- **公共实现**：`include/fast_gdb/writer/versioned_gdb_store.h`、`versioned_gdb_validator.h`
- **适用范围**：可靠本地文件系统上的同一进程多个独立 Reader 与单个 Writer并发访问 FileGDB
- **取代**：ADR-001～ADR-005 的公共 Writer API 和直接 source 发布决策

## 1. 背景

直接修改或替换业务 GDB 目录会产生两个不可接受的问题：

1. 发布窗口中新 Reader 可能找不到完整 source；
2. Writer 可能替换 Reader 正在 mmap 的文件。

因此公共 Writer 模型不再暴露原地编辑、`source → backup → source`、legacy target 或独立 Append/Update/Delete/Transaction。提交单元改为一个完整、不可变的 FileGDB generation。

本 ADR 决定的是 **FileGDB 的版本化发布协议**，不是 ArcGIS/GDAL 全部 FileGDB 数据模型和原地编辑行为的兼容层。GDAL/OpenFileGDB、ArcGIS 或业务编辑器可以在受控 `working_path()` 上生成候选 GDB，但其字段级事务、锁和 Dataset 生命周期不替代 VersionedGdbStore 的 publish、租约、持久化和恢复协议。

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
- snapshot 必须长于其派生的 `GdbCatalog`、`QueryEngine`、cursor、GDAL Dataset、fd、HANDLE 和 mmap；
- 已有 Reader 不自动切换；
- 空闲 Reader 关闭全部派生资源后可 `refresh()`；
- 旧 generation 仅在不是 CURRENT、租约为零且不存在持久性不确定状态时删除；
- 只保存 `snapshot.path()` 而提前释放 snapshot 属于协议绕过，不受支持；
- 外部进程直接打开 generation 不会登记租约，因此当前不支持跨进程 Reader。

结果：旧 Reader 连续读取旧版，新 Reader 获取新版，Writer 不触碰任何已发布文件。

## 4. 单 Writer 门禁

- 同一规范化 store root 的实例共享进程内状态；
- `weakly_canonical` 合并符号链接路径别名；
- Windows registry key 进行大小写折叠；
- 任意时刻最多一个 `GdbWriteTransaction`；
- Writer 只能修改 `working_path()`；
- 未发布事务析构或 `abort()` 删除 work 并释放门禁；
- 不提供跨进程锁或选主；
- 不允许多个进程、容器实例、定时任务或外部工具同时对同一 Store 写入。

进程内门禁不等价于 ArcGIS lock，也不能阻止另一个进程直接打开 `.gdb`。未实现 OS 级跨进程锁前，部署必须保证只有一个进程负责 Writer。

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

CoW 只是性能优化，完整复制是规范路径。CoW 成功不表示后续编辑、索引重建或持久化所需容量已预留；ENOSPC 可以在 working 创建成功后发生。

## 6. 外部编辑器生命周期契约

GDAL/OpenFileGDB、ArcGIS 或业务编辑器仅允许修改 `working_path()`。publish 前必须：

1. 完成或回滚编辑器内部事务；
2. 释放 Feature/row 对象；
3. 释放 SQL result set 和 cursor；
4. 关闭 Layer、Dataset 和后台任务；
5. 关闭所有 fd、HANDLE、mmap 和 lock；
6. 确认没有外部进程继续写 working；
7. 再由 fresh Reader validator 重开候选。

GDAL `CommitTransaction()` 只表示 working GDB 内部编辑已提交，不表示 VersionedGdbStore 已发布。业务发布成功点必须是 `PublishedDurable`。

ArcGIS Pro 等交互式工具不应长期直接连接 Store 内 work 目录；推荐在 Store 外生成完整 GDB，完全关闭外部连接后再导入 working。

## 7. 发布前验证

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

validator 的通过只证明当前配置的不变量成立，不代表 ArcGIS/GDAL 全部高级 geodatabase 语义完整兼容。除非进入明确 compatibility profile，以下能力默认不在发布证明范围：

- relationship class、attachments；
- domain、subtype、contingent values；
- feature dataset 层级；
- topology、network/utility network、parcel fabric；
- annotation、dimension；
- 原生曲线、MultiPatch 完整语义；
- raster、mosaic dataset；
- 稀疏 64-bit ObjectID；
- XY/Z/M resolution、tolerance 和其他 precision metadata；
- 自定义 ArcGIS 扩展元数据。

## 8. 持久化与切换顺序

1. 调用方关闭全部 working Writer、Dataset、fd、HANDLE 和 mmap；
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

原子替换保证命名层面的原子可见性，不自动等于崩溃后持久性。文件、候选目录、`generations/`、CURRENT temp 和 store root 的持久化屏障属于独立正确性步骤。

## 9. 发布结果

`GdbWriteTransaction` 提供：

- `NotPublished`：CURRENT 未切换；
- `PublishedDurable`：切换与最终持久化成功；
- `PublishedDurabilityUncertain`：CURRENT 已切换，但最终 store root 同步失败。

第三种状态下 `publish()` 返回 `false`，但 `published()==true`。事务已终结，不允许重试或 abort。

调用方不得只根据 bool 决定回滚。公共示例和生产代码必须检查 `publish_state()`。

## 10. 故障与恢复

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
- 禁止 abort 和 publish 重试；
- 无活动 Reader/Writer 后必须 `recover()`；
- recover 重读 CURRENT，完成目录持久化屏障，再恢复垃圾回收。

### 崩溃恢复

`open()` / `recover()`：

- 创建必要目录；
- 在安全状态下处理 stale work 和 `CURRENT.tmp-*`；
- 严格解析 CURRENT 并确认目标存在；
- 完成目录持久化屏障；
- 清除不确定状态；
- 在无活动租约时删除非 CURRENT generation；
- CURRENT 非法或目标缺失时 fail closed，不猜测最新目录。

技术 recovery 只恢复 manifest 与持久化一致性，不自动完成业务回滚，也不根据最大 generation id 或 mtime 选择“最新”版本。

## 11. 必要假设

本 ADR 的全部保证只在下列假设同时成立时有效：

1. **托管入口**：所有访问经 VersionedGdbStore；不直接读写 generation、work 或 CURRENT；
2. **单进程协议**：同一 Store 只有一个进程负责 Reader 租约、Writer 和 GC；
3. **单 Writer**：任意时刻只有一个 Writer，外部 ArcGIS/GDAL 不并发 update；
4. **Reader 生命周期**：snapshot 覆盖全部派生资源；
5. **本地可靠文件系统**：支持常规文件刷新、目录刷新、同设备 rename 和可验证的 CURRENT 替换；
6. **同一设备**：store root、work、generations 和 CURRENT 位于同一 volume/device；
7. **外部句柄关闭**：publish 前工作 GDB 的全部编辑器、锁和句柄已关闭；
8. **validator profile 足够**：覆盖业务关键图层和能力；
9. **容量充分**：可承受 FullCopy 和 CoW 后续写放大；
10. **保守恢复**：uncertain 时不 GC、不重试、不启动新 Writer。

以下环境不满足默认假设：

- NFS、SMB/CIFS、UNC 网络共享；
- FUSE；
- OneDrive、Google Drive、Dropbox 等同步目录；
- S3、对象存储或其挂载点；
- ZIP、HTTP、GDAL VSI；
- WSL 跨 Windows/Linux 边界路径；
- 多主机共享；
- 容器滚动升级中多个实例同时挂载和写入。

这些环境当前明确不支持，不能仅标记为“性能可能较差”。

## 12. 非目标

- 旧 Writer API 或兼容层；
- 字段级 Append/Update/Delete 公共接口；
- schema creation/migration；
- FID/ObjectID 分配、稠密性、空洞复用或物理 row offset 稳定承诺；
- 原生曲线或 MultiPatch 写入；
- raster/mosaic 发布兼容承诺；
- relationship/domain/subtype 等高级对象的默认完整保真；
- savepoint、嵌套、跨 GDB 或分布式事务；
- 跨进程 Reader 租约或 Writer 锁；
- 多 Writer 合并或 last-writer-wins；
- S3、对象存储或网络对象清单；
- 网络文件系统和跨主机一致性；
- 已发布 generation 原地修补；
- 自动磁盘预留、配额或容量管理；
- 自动从损坏 CURRENT 中猜测候选 generation。

## 13. 后果

正面：Reader 发布窗口连续可见；mmap 文件不被修改；提交边界明确；CoW 可降低副本成本；公共 API 单一；CURRENT 前后故障状态可推理。

代价：调用方必须一次性切换新 API；字段级编辑器由调用方提供；外部编辑器生命周期需要严格管理；长 Reader 租约保留旧代次；Windows 通常完整复制；全树验证和持久化增加尾延迟；高级 geodatabase 对象需要独立 profile。

与 GDAL 的关系：GDAL/OpenFileGDB 更适合作为 working GDB 的编辑器和兼容性对照；VersionedGdbStore 的优势是发布隔离、Reader snapshot 和恢复边界，而不是更短的原地更新路径或更完整的数据模型。

## 14. Fail-Fast 原则

下列条件应在运行时直接拒绝或在 strict production mode 下拒绝：

- 远程/网络/FUSE/云同步/对象存储文件系统；
- store 目录跨 device/volume；
- 无法可靠规范化的 path alias；
- CURRENT 非法、指向 work、symlink、缺失目录或路径逃逸；
- 检测到外部 Writer/owner；
- recover 时仍有 Reader/Writer；
- 目录持久化能力不可用；
- strict compatibility profile 检测到未支持高级对象；
- publish 前 working 仍被外部句柄或 lock 占用。

详细分类见 `docs/review/03_明确不支持场景与Fail-Fast策略.md`。

## 15. 验收

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
- 不确定发布保留新旧 generation 并要求 recover；
- 损坏 CURRENT fail closed；
- 长 Reader 阻止 GC 的容量行为可观测；
- 跨进程 Writer 和不支持文件系统可 fail-fast。

### 三平台

- macOS/APFS `clonefile` 与 full-copy 回退；
- Linux reflink 与非 reflink；
- Windows full-copy、大小写/路径别名门禁和 manifest replacement；
- ENOSPC、EIO、句柄占用和 crash-phase 故障注入；
- 真实 FileGDB validator 和 GDAL/ArcGIS 对照；
- 完整 CMake/CTest 和 package consumer artifacts。

当前详细验收矩阵见 `docs/review/05_跨平台测试与正式验收矩阵.md`。在正式证据闭环前，状态保持 **Implemented / Awaiting Evidence**。

## 16. 深度审查文档

- `docs/review/00_架构自检总览与结论.md`
- `docs/review/01_与GDAL-OpenFileGDB能力和语义对比.md`
- `docs/review/02_当前实现风险陷阱与误用清单.md`
- `docs/review/03_明确不支持场景与Fail-Fast策略.md`
- `docs/review/04_生产运行恢复与故障处理手册.md`
- `docs/review/05_跨平台测试与正式验收矩阵.md`
