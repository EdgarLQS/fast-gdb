# Reader 读取流程专题

## 1. 目标

本文描述 fast-gdb 从 FileGDB 目录到查询结果的完整 Reader 链路。fast-gdb 不提供 FileGDB Writer；外部 GDAL 编辑必须与当前 Reader 生命周期完全隔离。

ADR-008 另外规划一个位于低层 Reader 之上的 Adaptive 编排层，用于观察 Writer 活动、校验数据源变化、使旧 Reader 失效，并在写入结束且数据源稳定后使用 fresh GDAL 只读连接恢复。该能力尚未实现，当前正式合同仍由 ADR-007 定义。

## 2. 总体流程

```text
FileGDB directory
    ↓
GdbCatalog::scan
    ↓
system table discovery
    ↓
CatalogResolver::resolve
    ↓
GdbTableParser::open + load_tablx
    ↓
QueryEngine planning
    ↓
Sequential / FID / Attribute / Spatial / SpatialWhere
    ↓
FeatureCursor
    ↓
FeatureRecord + GeometryValue
    ↓
ISO WKB-first output
```

规划中的 Adaptive 层不会改变上述低层解析链，而是在调用前后增加 activity/generation 和 source snapshot 校验。

## 3. 目录和系统表

`GdbCatalog::scan()`：

- 验证目标目录；
- 发现 FileGDB 系统表；
- 建立内部 table id、文件名和能力信息；
- 为后续 resolver 和索引规划提供目录级元数据。

`CatalogResolver::load()` 解析系统目录并将逻辑图层名映射到：

- `.gdbtable`；
- `.gdbtablx`；
- 可选 `.spx`；
- 可选 `.atx`；
- 图层和空间参考元数据。

ADR-008 Phase 1 计划为这些依赖增加跨平台文件身份、大小和高精度 mtime 快照，但当前 `GdbCatalog` 还没有该完整能力。

## 4. 表和 FID

`GdbTableParser`：

1. 打开 `.gdbtable`；
2. 解析字段描述；
3. 加载 `.gdbtablx`；
4. 将零基 fast-gdb FID 映射到物理记录；
5. 读取 nullable flags、字段值和 geometry blob。

不得假设：

- FID 连续；
- 删除槽一定复用；
- 物理 row offset 永久稳定；
- 外部 REPACK 后旧 offset 仍有效。

现有 `TablxCacheKey` 的 device/inode 或 volume/file ID、size 和 mtime 能力将作为未来通用 `FileStamp` 的实现基础。

## 5. 几何

```text
FileGDB geometry blob
    ↓
GeometryModel
    ↓
PolygonTopologyBuilder / curve linearizer
    ↓
GeometryValue
    ├── ISO WKB
    ├── bbox
    ├── geometry status
    └── optional WKT
```

正式输出是 ISO WKB。WKT 仅在调用 `to_wkt()` 时按需生成。

## 6. 查询规划

`QueryEngine` 支持：

- SequentialScan；
- FID lookup；
- AttributeWhere；
- Spatial；
- SpatialWhere。

索引原则：

- `.spx` 和 `.atx` 只生成候选；
- 最终属性表达式必须复核；
- 最终空间关系必须使用真实几何复核；
- 索引损坏、编码不明确或能力不足时安全回退。

## 7. FeatureCursor

`FeatureCursor` 是 move-only 的查询流：

- `next()` 顺序返回完整要素；
- `move_to(fid)` 可定位；
- `done()` 表示正常结束；
- `error()` 提供失败原因；
- 活动 cursor 期间不得重开或移动对应 engine。

Cursor 不应跨线程共享。

Adaptive 校验完成前不得将依赖旧 mmap、row buffer 或 cursor 生命周期的借用数据发布给调用方。需要前后快照校验的结果必须先完整物化。

## 8. 对象生命周期

推荐生命周期：

```text
GdbCatalog
  └── CatalogResolver result
       └── QueryEngine / GdbTableParser
            └── FeatureCursor
```

销毁顺序反向进行：

```text
FeatureCursor
  → QueryEngine / GdbTableParser
  → resolver state
  → GdbCatalog
```

任何 generation 或依赖文件变化都必须使整条对象链失效，不能只刷新 tablx 或索引。

## 9. 外部写入边界

Reader 对象可能持有：

- mmap；
- fd/HANDLE；
- Schema；
- FID offset；
- `.spx/.atx` 页面；
- 查询规划和 cursor 状态。

因此当前合同要求 GDAL 修改同一 GDB 前销毁所有 Reader 对象。GDALClose 后也不能继续复用旧 Reader，必须从 `GdbCatalog::scan()` 开始完整重建。

```text
Reader objects alive + GDAL update same GDB = Unsupported
```

并发期间 old/new/mixed/error 均可能出现，项目不提供固定可见性合同。

## 10. 受支持的编辑后重开流程

```text
stop queries
    ↓
destroy cursor / engine / table / catalog
    ↓
GDAL update
    ↓
close Feature / SQL results / Dataset
    ↓
new catalog scan
    ↓
new resolver / table / engine / cursor
```

## 11. Proposed Adaptive Reader 流程

该流程是 ADR-008 的设计目标，不是当前 API：

```text
observe writer activity/generation
    ├─ writer active → SourceBusy; neither backend runs
    └─ capture source snapshot A
            ↓
       fast-gdb read and fully materialize
            ↓
       capture source snapshot B
            ├─ stable + success → ReturnFast
            └─ changed/unsupported → discard + expire Reader
                                         ↓
                                    verify quiescence
                                         ├─ unstable → SourceBusy
                                         └─ stable → fresh GDAL read-only
                                                        ↓
                                                   materialize + GDALClose
                                                        ↓
                                                   verify source unchanged
                                                        ├─ stable → ReturnGdal
                                                        └─ changed → SourceBusy
```

关键规则：

- 检测到正在写时不能立即通过 GDAL 强行读取；
- recovery 每次必须 fresh open/close GDAL Dataset；
- 不能复用曲线 Hybrid 的 thread-local Dataset cache；
- 无协调 Writer 检测只能标记为 best-effort；
- 默认不无限等待，无法确认稳定时失败关闭。

## 12. 错误处理

Reader 应失败关闭而不是猜测：

- 目录或系统表缺失；
- table/tablx 不一致；
- 页边界或记录长度非法；
- `.spx/.atx` 结构不完整；
- geometry 编码未知；
- 外部写入导致文件状态变化。

ADR-008 计划进一步区分：

- `SourceBusy`；
- `SourceChangedDuringRead`；
- `ReaderExpired`；
- `FastBackendUnsupported`；
- `FastBackendReadFailed`；
- `GdalOpenFailed/GdalReadFailed`；
- `SourceNeverStabilized`。

## 13. 性能原则

- 优先 mmap 和顺序访问；
- 避免重复定位同一记录；
- WKB-first，避免无需求 WKT；
- 索引只在选择性足够时使用；
- 候选复核不可删除；
- 性能测试必须与 GDAL 和 legacy read path 做校验和一致性对比；
- Adaptive 快路径快照应依赖感知，不默认哈希整个 GDB；
- 性能优化不能削弱 fail-closed 和结果丢弃语义。

## 14. 测试

当前核心测试覆盖：

- 字段和几何解析；
- FID 和删除槽；
- `.spx/.atx`；
- WHERE 和 SpatialWhere；
- FeatureCursor；
- GDAL parity；
- 写前关闭 Reader、GDAL 写入、写后完整重开；
- 同目录并发读写的观测性分类。

ADR-008 计划新增：

- Writer active 时两个后端调用数均为零；
- generation/source 变化使旧 Reader 过期；
- fast 和 GDAL 读取期间变化均丢弃结果；
- 写后 fresh GDAL 读取完整新状态；
- fresh fallback 不复用缓存 Dataset；
- 三平台压力测试无 mixed、无崩溃。

相关文档：

- [GDAL 写入与 fast-gdb 读取边界](../usage/11_GDAL写入与fast-gdb读取边界.md)
- [Reader-only ADR](../adr/ADR-007-reader-only-gdal-edit-boundary.md)
- [Adaptive Reader ADR](../adr/ADR-008-adaptive-reader-write-detection-gdal-fallback.md)
- [Adaptive Reader 实施计划](../planning/22_AdaptiveReader写入检测与GDAL回退计划.md)
- [并发可见性观测证据](../evidence/gdal-write-fast-gdb-read-characterization-2026-07-22.md)
