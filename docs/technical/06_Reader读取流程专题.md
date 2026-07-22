# Reader 读取流程专题

## 1. 目标

本文描述 fast-gdb 从 FileGDB 目录到查询结果的完整 Reader 链路。fast-gdb 不提供 FileGDB Writer；外部 GDAL 编辑必须与 Reader 生命周期完全隔离。

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

## 9. 外部写入边界

Reader 对象可能持有：

- mmap；
- fd/HANDLE；
- Schema；
- FID offset；
- `.spx/.atx` 页面；
- 查询规划和 cursor 状态。

因此 GDAL 修改同一 GDB 前必须销毁所有 Reader 对象。GDALClose 后也不能继续复用旧 Reader，必须从 `GdbCatalog::scan()` 开始完整重建。

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

## 11. 错误处理

Reader 应失败关闭而不是猜测：

- 目录或系统表缺失；
- table/tablx 不一致；
- 页边界或记录长度非法；
- `.spx/.atx` 结构不完整；
- geometry 编码未知；
- 外部写入导致文件状态变化。

## 12. 性能原则

- 优先 mmap 和顺序访问；
- 避免重复定位同一记录；
- WKB-first，避免无需求 WKT；
- 索引只在选择性足够时使用；
- 候选复核不可删除；
- 性能测试必须与 GDAL 和 legacy read path 做校验和一致性对比。

## 13. 测试

核心测试覆盖：

- 字段和几何解析；
- FID 和删除槽；
- `.spx/.atx`；
- WHERE 和 SpatialWhere；
- FeatureCursor；
- GDAL parity；
- 写前关闭 Reader、GDAL 写入、写后完整重开；
- 同目录并发读写的观测性分类。

相关文档：

- [GDAL 写入与 fast-gdb 读取边界](../usage/11_GDAL写入与fast-gdb读取边界.md)
- [Reader-only ADR](../adr/ADR-007-reader-only-gdal-edit-boundary.md)
- [并发可见性观测证据](../evidence/gdal-write-fast-gdb-read-characterization-2026-07-22.md)
