# GDAL 写入与 fast-gdb Reader 生命周期边界

## 1. 架构定位

fast-gdb 只读取 FileGDB。GDAL/OpenFileGDB 或 ArcGIS 负责所有编辑。两者不是可对同一目录并发工作的共享事务系统，而是按阶段交接目标 GDB 的两个组件。

```text
Reader phase
    fast-gdb owns read handles and caches

Edit phase
    GDAL owns update handles and FileGDB mutations
```

两个阶段之间必须有完整关闭边界。

## 2. Reader 持有的状态

一个 fast-gdb 查询链可能持有：

- GDB 目录和系统表解析结果；
- `GdbCatalog`；
- `CatalogResolver` 结果；
- `GdbTableParser`；
- `.gdbtable` mmap 或文件句柄；
- `.gdbtablx` FID 到记录偏移映射；
- `.spx` 页面、网格参数和候选；
- `.atx` 元数据、B+ 树页面和候选；
- 字段定义、nullable flags 和记录布局；
- `QueryEngine` 规划状态；
- `FeatureCursor` 的扫描位置。

GDAL 写入后，以上任一状态都可能过期。因此不能只重开某一个文件或清理某一个缓存。

## 3. GDAL 可能修改的内容

不同编辑操作可能影响：

| GDAL 操作 | 可能变化 |
|---|---|
| `CreateFeature` | `.gdbtable`、`.gdbtablx`、空间/属性索引、extent |
| `SetFeature` | 记录内容、记录位置、索引键、几何索引 |
| `DeleteFeature` | 活动记录、删除槽、索引、统计信息 |
| `CreateField/DeleteField` | Schema、记录布局、系统表 |
| 创建/删除属性索引 | `.gdbindexes`、`.atx`、系统元数据 |
| 空间索引重建 | `.spx` 和空间元数据 |
| `REPACK` | 表文件重写、物理偏移、删除槽压缩 |
| extent 重算 | 图层元数据和空间范围 |

所以“字段值只是改了一点”也不能推导出已有 mmap 或索引缓存仍安全。

## 4. 当前受支持的状态机

```text
Reading
  ├─ stop accepting new queries
  ├─ wait for cursors to finish
  ├─ destroy all Reader objects
  └─ close mappings and handles
        ↓
Quiescent
        ↓
GDAL Editing
  ├─ open OpenFileGDB update Dataset
  ├─ perform all edits
  ├─ close Features and SQL result sets
  ├─ flush/commit as required
  └─ GDALClose Dataset
        ↓
Closed
        ↓
Reader Reopen
  ├─ rescan catalog
  ├─ reload schema
  ├─ reopen table/tablx/index files
  └─ resume queries
```

只有最后重新创建的 Reader 才属于当前受支持状态。

## 5. 不支持的状态

### 5.1 已有 Reader 与 GDAL Writer 重叠

```text
fast-gdb parser/mmap open
+ GDAL update same directory
```

结果未定义。

### 5.2 GDALClose 后继续复用旧 Reader

GDALClose 只说明 GDAL 已释放其对象，不会自动使 fast-gdb 的 mmap、fd、Schema 或索引缓存失效。旧 Reader 仍然不受支持。

### 5.3 部分 refresh

以下操作不构成完整重开：

- 只重新扫描目录；
- 只重载 `.gdbtablx`；
- 只清理 `.spx/.atx`；
- 保留旧 `QueryEngine`；
- 保留旧 cursor；
- 保留旧 mmap 后重新读取系统表。

项目不提供局部 refresh API 或正确性合同。

## 6. 可见性分类

观测性测试使用两个同时更新的字段识别：

| 分类 | 含义 |
|---|---|
| `old` | 两个字段都保持写前值 |
| `new` | 两个字段都为写后值 |
| `mixed` | 两个字段来自不同阶段或记录解释不一致 |
| `error` | 打开、解析或读取失败 |

不同 GDAL 版本、操作系统、文件系统、更新操作和数据规模可能得到不同结果。测试输出只用于确认风险，不用于支持声明。

## 7. 线程与进程边界

当前 fast-gdb 无法确定外部进程何时通过 GDAL 修改目录。因此读写互斥必须由调用方提供：

- 进程内读写状态机；
- 服务维护窗口；
- 独占文件锁协议；
- 业务层版本目录切换。

即使调用方使用锁，锁也只是协调手段；当前仍需遵守“写前关闭 Reader、写后重新打开”。

## 8. 在线服务方案

需要不停读时，推荐将版本管理置于 fast-gdb 之外：

```text
logical dataset name
       ↓
application routing / symlink / config
       ↓
immutable data-v1.gdb  ← existing Readers
immutable data-v2.gdb  ← new Readers after external switch
```

GDAL 永远只编辑未被 Reader 使用的工作副本。业务系统负责验证、切换和旧版本回收。

## 9. 当前测试门禁

### 必须通过

- Reader 读取旧值；
- 销毁 Reader；
- GDAL 修改并关闭；
- 新 Reader 读取新值。

### 只记录

- 已有 Reader 在 GDAL Writer 打开期间读取；
- 新 Reader 在 GDAL Writer 打开期间打开；
- GDALClose 后旧 Reader 再次读取。

这些记录可以是 old/new/mixed/error 中任意一种。

## 10. 当前代码审核规则

任何提交若出现以下行为，应直接拒绝或要求明确 ADR：

- 在 fast-gdb 中新增 FileGDB 写入 API；
- 新增 `.gdbtable/.gdbtablx/.spx/.atx` 写代码；
- 在 Reader 存活时调用 GDAL update；
- 写后复用旧 parser、cursor 或 mmap；
- 将观测性测试的单平台结果写成并发支持声明；
- 在安装导出中重新加入 Writer target。

## 11. 可选 Adaptive Reader 扩展

> 本节对应已 Accepted 的 [ADR-008](../adr/ADR-008-adaptive-reader-write-detection-gdal-fallback.md)。同进程协调实现已进入可选 target；跨平台、压力、性能和多 GDAL 版本仍需独立发布验收。

Adaptive Reader 不尝试支持边写边读，而是把不安全重叠转换为：

```text
活动 Writer
  → SourceBusy

读取期间源变化
  → 丢弃结果
  → ReaderExpired

源恢复稳定且 fast 路径不可用
  → fresh GDAL read-only fallback
```

它是读取编排层，不承担写入、事务、锁管理或发布职责。

## 12. 两层写入检测

### 12.1 协调模式

调用方提供只读状态：

```text
writer_active
stable generation
```

正式目标：

- `writer_active=true` 时不调用 fast-gdb，也不调用 GDAL fallback；
- generation 在读取前后变化时丢弃结果；
- generation 变化后旧 Reader 对象图过期；
- 写入方发布 `writer_active=false` 且 generation 稳定后，才允许恢复读取。

fast-gdb 只消费状态，不创建、删除或修改 marker。

### 12.2 无协调模式

对未知外部 Writer 使用 best-effort 文件状态检测：

- device/inode 或 volume/file ID；
- size；
- 高精度 mtime；
- 文件增加、删除和替换；
- 查询依赖的 table/tablx/spx/atx/index/system-table 变化；
- `.lock` 仅作为辅助信号。

无协调模式不能证明 Writer 生命周期，只能发现变化并拒绝可疑结果。连续两次快照相同不能单独证明写入已经结束。

## 13. Adaptive 读取状态机

```text
Ready
  ├─ activity says writer active ─────────→ SourceBusy
  └─ snapshot A
          ↓
      FastReading
          ↓ fully materialize
      snapshot B
          ├─ stable + success ─────────────→ ReturnFast
          └─ changed/unsupported/failure ─→ ExpireFastReader
                                                ↓
                                         CheckQuiescent
                                                ├─ unstable → SourceBusy
                                                └─ stable → FreshGdalReading
                                                               ↓ fully materialize
                                                          close GDALDataset
                                                               ↓ snapshot verify
                                                          stable → ReturnGdal
                                                          changed → SourceBusy
```

### 为什么必须完整物化

读取后校验之前，不得将以下借用状态返回调用方：

- mmap 中的指针；
- `FieldRef`；
- row buffer 视图；
- cursor 当前行；
- GDAL Feature/Geometry 非拥有指针。

否则源变化发生时无法撤回已经暴露的数据。

## 14. fresh GDAL recovery 边界

Adaptive recovery 必须：

1. 确认没有协调模式活动 Writer；
2. 捕获读取前快照；
3. 新开 `GDAL_OF_READONLY` Dataset；
4. 完整物化查询结果；
5. 释放 Feature 和 SQL result set；
6. `GDALClose()`；
7. 捕获读取后快照；
8. 快照一致才返回。

现有 `GdalCurveBackendBridge` 使用 thread-local Dataset 缓存，适用于复杂几何回退，但不能直接承担外部写入后的恢复读取。Adaptive recovery 必须拥有独立的 fresh session。

## 15. Adaptive 失效和缓存规则

检测到 generation 或依赖文件变化时，必须使以下状态全部失效：

- `GdbCatalog` 和 system-table 解析；
- `CatalogResolver`；
- `GdbTableParser`；
- mmap、fd/HANDLE；
- tablx offset 和进程缓存；
- spx/atx/index metadata；
- QueryEngine 和 FeatureCursor；
- 与旧 generation 绑定的 GDAL fallback Dataset。

不得以局部 refresh 将旧对象恢复为有效状态。

## 16. Adaptive 审核规则

在 ADR-008 Accepted 前，相关实现必须满足：

- 所有 API 和 target 只描述 Reader、fallback、busy 和 invalidation；
- 活动 Writer 时 fail closed；
- 不把 GDAL 只读路径当作并发更新快照；
- 无协调模式名称和文档明确包含 best-effort；
- fast 和 GDAL 结果都在源状态后置校验后才发布；
- 不复用旧 GDALDataset；
- 不重新引入 Writer target、Writer header 或 update API；
- 三平台测试、压力测试和 artifact 完成前不得写成已支持能力。
