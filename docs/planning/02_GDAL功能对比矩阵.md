# 06 — GDAL 功能对比矩阵

> **核心问题**：explorgdb reader 的矢量读取能力相比 GDAL OpenFileGDB 驱动，覆盖了多少？哪些功能可以用于生产？
>
> **前置知识**：[项目全景](../overview/00_项目全景与架构概览.md)、[组件库设计](../usage/01_组件库设计与使用.md)
>
> **学完能做**：清楚知道 explorgdb 的能力边界，做出是否替换 GDAL 的决策
>
> **预计阅读时间**：20 分钟

---

## 0. 快速结论

| 维度 | 覆盖度 | 一句话 |
|------|:------:|--------|
| 常规几何（点/线/面） | **100%** | ✅ 完全覆盖，WKT 输出一致 |
| 曲线几何（圆弧/贝塞尔） | **0%** | ❌ nCurves 被跳过，坐标丢失 |
| 字段类型（17 种） | **约 88% 完全支持** | ✅ 15/17 完全支持，Raster/DateTimeWithOffset 有边界 |
| 空间参考系统 (SRS) | **0%** | ❌ **最大 gap**，完全未实现 |
| 核心元数据 | **90%+** | ✅ 字段/范围/原点/缩放/网格 |
| 高级元数据 | **0%** | ❌ 字段域/关系类/注记 |
| 索引查询 | **100%** | ✅ .spx + .atx B+ 树完整支持 |
| 读取性能 | **9-12x** | 🚀 零拷贝 seq_scan 大幅领先 |
| 只读生产可用性 | **高性能候选** | ✅ fast-gdb 优先补齐缺口，见 [8. 只读生产可行性分析](#8-只读生产可行性分析) |

---

## 目录

1. [几何类型对比](#1-几何类型对比)
2. [字段类型对比](#2-字段类型对比)
3. [空间参考系统 (SRS)](#3-空间参考系统-srs)
4. [矢量元数据](#4-矢量元数据)
5. [矢量数据读取 API](#5-矢量数据读取-api)
6. [索引支持](#6-索引支持)
7. [其他功能对比](#7-其他功能对比)
8. [只读生产可行性分析](#8-只读生产可行性分析)
9. [迁移路线图](#9-迁移路线图)

---

## 1. 几何类型对比

### 约定

| 标记 | 含义 |
|:----:|------|
| ✅ | 完全支持，WKT 输出与 GDAL OGR 一致 |
| ⚠️ | 部分支持，输出格式不同或坐标虽读但丢弃 |
| ❌ | 不支持，数据被跳过或丢失 |

### 1.1 基础几何类型

| GDB 类型码 | 几何类型 | GDAL OpenFileGDB | explorgdb | explorgdb WKT 输出 | 备注 |
|:----------:|---------|:----------------:|:---------:|-------------------|------|
| 1 | Point | ✅ | ✅ | `POINT (x y)` | 基础点 |
| 3 | Polyline | ✅ | ✅ | `MULTILINESTRING ((...), (...))` | 多部件 → MULTILINESTRING |
| 5 | Polygon | ✅ | ✅ | `MULTIPOLYGON (((...)), (...))` | 多环 → MULTIPOLYGON，自动闭合 |
| 8 | MultiPoint | ✅ | ✅ | `MULTIPOINT ((x y), (x y))` | 多点 |

### 1.2 Z/M 变体

| GDB 类型码 | 几何类型 | GDAL | explorgdb | explorgdb WKT 输出 |
|:----------:|---------|:----:|:---------:|-------------------|
| 9 | PointZ | ✅ | ✅ | `POINT Z (x y z)` |
| 10 | PolylineZ | ✅ | ✅ | `MULTILINESTRING Z (...)` |
| 11 | PointZM | ✅ | ✅ | `POINT ZM (x y z m)` |
| 13 | PolylineZM | ✅ | ✅ | `MULTILINESTRING ZM (...)` |
| 15 | PolygonZM | ✅ | ✅ | `MULTIPOLYGON ZM (...)` |
| 18 | MultiPointZM | ✅ | ✅ | `MULTIPOINT ZM (...)` |
| 19 | PolygonZ | ✅ | ✅ | `MULTIPOLYGON Z (...)` |
| 20 | MultiPointZ | ✅ | ✅ | `MULTIPOINT Z (...)` |
| 21 | PointM | ✅ | ✅ | `POINT M (x y m)` |
| 23 | PolylineM | ✅ | ✅ | `MULTILINESTRING M (...)` |
| 25 | PolygonM | ✅ | ✅ | `MULTIPOLYGON M (...)` |
| 28 | MultiPointM | ✅ | ✅ | `MULTIPOINT M (...)` |

### 1.3 特殊类型

| GDB 类型码 | 几何类型 | GDAL | explorgdb | 说明 |
|:----------:|---------|:----:|:---------:|------|
| 31 | MultiPatchM | ✅ | ⚠️ | 描述文本，非标准 WKT |
| 32 | MultiPatch | ✅ | ⚠️ | 描述文本，坐标已读但丢弃 |
| 50 | GeneralPolyline | ✅ | ✅ | 含 Z/M 高位标志 |
| 51 | GeneralPolygon | ✅ | ✅ | |
| 52 | GeneralPoint | ✅ | ✅ | |
| 53 | GeneralMultiPoint | ✅ | ❓ | 未独立测试，走 decode_multipoint |
| 54 | GeneralMultiPatch | ✅ | ⚠️ | 同 MultiPatch |

### 1.4 曲线几何（❌ 不支持）

| 曲线类型 | GDAL | explorgdb | 说明 |
|---------|:----:|:---------:|------|
| CircularArc (圆弧) | ✅ | ❌ | nCurves varuint 被跳过，见 `gdb_geometry.cpp` 第 288 行注释 `// GeneralPolyline may have nCurves (skip)` |
| BezierCurve (贝塞尔曲线) | ✅ | ❌ | 无任何处理代码 |
| EllipticArc (椭圆弧) | ✅ | ❌ | 无任何处理代码 |

**源码位置**：`src/edgar/explorgdb/reader/gdb_geometry.cpp` 中 `decode_polyline` 和 `decode_polygon` 函数，当 `is_general` 为 true 时：
```cpp
// 读取 nCurves 并丢弃
if (is_general) {
    read_varuint(s);  // ← nCurves 被跳过
}
```

**影响**：如果数据包含曲线几何，曲线段会被完全忽略，只保留线性段。2D 坐标不受影响，但曲线定义丢失。

### 1.5 MultiPatch 详细分析

**当前行为**：输出描述文本而非标准 WKT，例如：
```
MultiPatch(3 parts: [TriangleStrip(3pts), TriangleFan(4pts), OuterRing(5pts)])
```

**坐标处理**：
- XY 坐标：**已读取并解码**（但未输出）
- Z 坐标：**已读取并解码**（但未输出）
- M 坐标：**已读取并解码**（但未输出）

**7 种部件类型**：
| 索引 | 类型 | 索引 | 类型 |
|:----:|------|:----:|------|
| 0 | TriangleStrip | 4 | FirstRing |
| 1 | TriangleFan | 5 | Ring |
| 2 | OuterRing | 6 | Triangles |
| 3 | InnerRing | | |

**与 GDAL 的差异**：GDAL 将 MultiPatch 映射为 `wkbGeometryCollection` 或 `wkbPolyhedralSurface` / `wkbTIN`，输出标准 OGR WKT。explorgdb 的描述文本在 GIS 工具链中不可用。

### 1.6 几何类型总结

```
常规几何（点/线/面/多点及其 Z/M 变体）: 24/24 ✅ 
Special Geometry（General 类型）:          5/5 ✅
MultiPatch（含 M 变体）:                   2/2 ⚠️
曲线几何（CircularArc/Bezier/EllipticArc）: 0/3 ❌
─────────────────────────────────────────────
总计:                                      31/34 ✅/⚠️ (91%)
```

---

## 2. 字段类型对比

### 完整字段类型矩阵

| 类型码 | GDB 字段类型 | 宽度 | GDAL | explorgdb | 备注 |
|:-----:|-------------|:----:|:----:|:---------:|------|
| 0 | FGFT_INT16 | 2 字节 | ✅ | ✅ | int32_t 返回 |
| 1 | FGFT_INT32 | 4 字节 | ✅ | ✅ | int32_t 返回 |
| 2 | FGFT_FLOAT32 | 4 字节 | ✅ | ✅ | double 返回 |
| 3 | FGFT_FLOAT64 | 8 字节 | ✅ | ✅ | double 返回 |
| 4 | FGFT_STRING | 变长 | ✅ | ✅ | UTF-8 解码 |
| 5 | FGFT_DATETIME | 8 字节 | ✅ | ✅ | OLE double 格式 |
| 6 | FGFT_OBJECTID | 隐式 | ✅ | ✅ | fid + 1 |
| 7 | FGFT_GEOMETRY | 变长 | ✅ | ✅ | 解码为 WKT |
| 8 | FGFT_BINARY | 变长 | ✅ | ✅ | vector<uint8_t> |
| 9 | FGFT_RASTER | 复杂 | ✅ | ⚠️ | 描述符已解析，像素数据未解码 |
| 10 | FGFT_GUID | 16 字节 | ✅ | ✅ | 32 字符 hex 字符串 |
| 11 | FGFT_GLOBALID | 16 字节 | ✅ | ✅ | 同上 |
| 12 | FGFT_XML | 变长 | ✅ | ✅ | 作为 string 读取 |
| 13 | FGFT_INT64 | 8 字节 | ✅ (GDAL 3.8+) | ✅ | int64_t 返回 |
| 14 | FGFT_DATE | 8 字节 | ✅ (GDAL 3.8+) | ✅ | double 日期部分 |
| 15 | FGFT_TIME | 8 字节 | ✅ (GDAL 3.8+) | ✅ | double 时间部分 |
| 16 | FGFT_DATETIME_WITH_OFFSET | 10 字节 | ✅ (GDAL 3.8+) | ⚠️ | 普通记录解析读取 double + int16；偏移未暴露，peek 路径需补测试 |

### 字段类型总结

```
完全支持: 15/17 ✅ (88%)
部分支持:  2/17 ⚠️ (Raster, DateTimeWithOffset)
不支持:    0/17 ❌ (全部字段类型均可解析记录)
─────────────────────────────────────────────
总计:     17/17 可解析，15/17 完整暴露
```

**注意**：explorgdb 对 `DateTimeWithOffset` 的普通记录解析会读取 double + int16，但只向上层暴露 double。`peek_geometry_blob` 等快速跳字段路径需要统一使用 10 字节跳过逻辑，避免字段位于几何字段之前时产生错位。

---

## 3. 空间参考系统 (SRS)

### 这是 explorgdb 与 GDAL 之间的最大功能 gap

| 功能 | GDAL OpenFileGDB | explorgdb | 影响 |
|------|:----------------:|:---------:|------|
| 读取坐标系 WKT | ✅ | ❌ | 无法在输出中附带坐标系信息 |
| 读取 WKID (EPSG 编号) | ✅ | ❌ | 无法做坐标系匹配 |
| 读取 LatestWKID | ✅ | ❌ | 同上 |
| 返回 OGRSpatialReference 对象 | ✅ | ❌ | 无法做重投影、坐标变换 |
| 坐标系轴序（传统 GIS 顺序） | ✅ | ❌ | 无法保证坐标轴序匹配 |

### 为什么 SRS 缺失

SRS 信息存储在 GDB 的系统表中，不在 `.gdbtable` 字段描述符中。项目文档和样本中对系统表数字 ID 的描述仍需统一，因此实现时不应写死 `a00000003` / `a00000004` / `a00000005` 这类编号。

推荐做法：

1. 先解析 `GDB_SystemCatalog`，按表名定位 `GDB_SpatialRefs`、`GDB_Items` 等系统表。
2. 从 `GDB_SpatialRefs` 读取 SRID/WKID、SRSName、WKT/XML 等空间参考字段。
3. 必要时从 `GDB_Items.Definition` 中补充图层级 XML 定义。

### 修复方案（见迁移路线图）

需要解析系统表来提取 SRS 信息。优先使用 explorgdb 已有的 `GdbCatalog` + `GdbTableParser` 能力按表名读取系统表，形成 `SpatialReferenceInfo` 之类的纯 C++ 元数据结构。GDAL 只用于测试对照，不作为运行时主路径。

---

## 4. 矢量元数据

### 4.1 核心元数据

| 元数据 | GDAL | explorgdb | 来源 |
|-------|:----:|:---------:|------|
| 图层名称 | ✅ | ✅ | gdb_catalog + 文件名推断 |
| 要素数量 | ✅ | ✅ | .gdbtablx 偏移表大小 |
| 字段定义（名称/类型/宽度） | ✅ | ✅ | .gdbtable 字段描述符区 |
| 字段别名 | ✅ | ✅ | UTF-16 解码 |
| 字段 nullable 标志 | ✅ | ✅ | flag 位 0 |
| 字段默认值 | ✅ | ✅ | flag 位 2 |
| 几何类型 (WKT) | ✅ | ✅ | 字段描述符的 wkt 字段 |
| 空间索引网格大小 | ✅ | ✅ | 字段描述符的 grid_sizes |
| 数据范围 (bbox) | ✅ | ✅ | xmin/ymin/xmax/ymax |
| Z 范围 | ✅ | ✅ | zmin/zmax |
| M 范围 | ✅ | ✅ | mmin/mmax |
| 坐标原点 (XY) | ✅ | ✅ | xorig/yorig |
| 坐标缩放 (XY) | ✅ | ✅ | xyscale |
| 坐标容差 (XY) | ✅ | ✅ | xytolerance |
| 坐标原点 (Z/M) | ✅ | ✅ | zorig/morig |
| 坐标缩放 (Z/M) | ✅ | ✅ | zscale/mscale |
| 坐标容差 (Z/M) | ✅ | ✅ | ztolerance/mtolerance |
| 文件版本 (v3/v4) | ✅ | ✅ | 自动检测 |
| 最大记录大小 | ✅ | ✅ | 表头部 |

### 4.2 高级元数据

| 元数据 | GDAL | explorgdb | 影响 |
|-------|:----:|:---------:|------|
| 字段域 (coded value / range domains) | ✅ | ❌ | 无法验证字段取值合法性 |
| 关系类 (relationship class) | ✅ | ❌ | 无法跨表关联查询 |
| Feature dataset 分组 | ✅ | ❌ | 图层目录结构扁平化 |
| 注解/尺寸注记 (Annotation/Dimension) | ✅ | ❌ | 无法读取注记 |
| 图层 XML 定义 | ✅ | ❌ | 无法获取完整图层元数据 |

### 元数据总结

```
核心元数据: 20/20 ✅ (100%)
高级元数据:  0/5  ❌ (0%)
─────────────────────────────────────────────
总计:      20/25 ✅ (80%)
```

---

## 5. 矢量数据读取 API

| 能力 | GDAL (OGRLayer) | explorgdb | 说明 |
|------|:-------------:|:---------:|------|
| 顺序扫描 (GetNextFeature) | ✅ | ✅ | seq_scan 零拷贝，9-12x 快 |
| 按 FID 读取 (GetFeature) | ✅ | ✅ | read_record_by_fid |
| 要素计数 (GetFeatureCount) | ✅ | ✅ | feature_count() |
| 数据范围 (GetExtent) | ✅ | ✅ | 从字段描述符读取 |
| 空间过滤 (SetSpatialFilter) | ✅ | ⚠️ | 需外部调用两阶段过滤实现 |
| 属性过滤 (SetAttributeFilter) | ✅ | ⚠️ | 需外部实现，无 Layer 级状态 |
| SQL 查询 (ExecuteSQL) | ✅ | ❌ | 无 SQL 解析引擎 |
| 字段定义 (GetLayerDefn) | ✅ | ✅ | fields() 返回字段定义 |
| 坐标系 (GetSpatialRef) | ✅ | ❌ | SRS 未实现 |
| 元数据键值 (GetMetadataItem) | ✅ | ❌ | 无键值元数据接口 |
| 能力检测 (TestCapability) | ✅ | ❌ | 无能力检测机制 |
| 批量读取 (SetNextByIndex) | ✅ | ❌ | 索引跳跃扫描 |
| 重置读取游标 (ResetReading) | ✅ | ✅ | 顺序扫描可重置 |

### 关键差异：两层过滤

**GDAL OGRLayer**：
```
SetSpatialFilter(bbox) → Layer 状态
SetAttributeFilter("pop > 1000") → Layer 状态
while (feat = GetNextFeature()) {
    // 自动应用过滤，只返回匹配的要素
}
```

**explorgdb**：
```
// 需要手动调用两阶段过滤
// 阶段 1：.spx 空间索引 → 候选 FID 列表
auto candidates = spx.query_bbox(xmin, ymin, xmax, ymax, ...);

// 阶段 2：peek_bbox 二次过滤
for (auto fid : candidates) {
    if (table.peek_geometry_blob(fid, blob, size)) {
        if (decoder.intersects_with_peek(blob, size, xmin, ymin, xmax, ymax)) {
            // 完整解码
            auto record = table.read_record_by_fid(fid, ...);
        }
    }
}

// 属性过滤需要额外实现
// 无 Layer 级状态，每次读取需重新设置
```

---

## 6. 索引支持

| 索引 | GDAL OpenFileGDB | explorgdb | 说明 |
|------|:---------------:|:---------:|------|
| .spx 空间索引解析 | ✅ | ✅ | B+ 树按需导航 + LRU 缓存 |
| .spx 空间查询 (bbox 相交) | ✅ | ✅ | query_bbox() |
| 空间查询结果去重 (bitset) | ❌ | ✅ | explorgdb 独有 |
| .atx 属性索引解析 | ✅ | ✅ | 数值 + 字符串索引 |
| .atx 属性查询 (6 种算子) | ✅ | ✅ | Eq/Ne/Lt/Le/Gt/Ge |
| .gdbindexes 元数据解析 | ✅ | ✅ | 索引名称/字段/类型 |
| 索引创建（空间索引） | ✅ (自动) | ✅ (GDAL SQL) | gdb_index_creator |
| 索引创建（属性索引） | ✅ (自动) | ✅ (GDAL SQL) | gdb_index_creator |
| 索引创建（复合索引） | ✅ (自动) | ✅ (GDAL SQL) | gdb_index_creator |
| 索引删除 | ✅ | ✅ | DropIndex |
| 多层 B+ 树 (depth 2-4) | ✅ | ✅ | 已验证 ArcGIS Pro 数据 |
| 索引验证工具 | — | ✅ | verify_arcgis_indexes, verify_gdal_indexes |

### 索引性能对比

| 查询类型 | GDAL | explorgdb | 加速比 |
|---------|:----:|:---------:|:------:|
| 空间查询 1% 覆盖 | 15.61 ms | 0.97 ms | **16x** |
| 空间查询 10% 覆盖 | 15.86 ms | 2.32 ms | **7x** |
| 空间查询 50% 覆盖 | 16.19 ms | 5.10 ms | **3x** |
| 属性范围查询 >8M | 20.76 ms | 1.18 ms | **18x** |
| 顺序扫描 100K | 124 ms | 10.5 ms | **12x** |
| 顺序扫描 10M | 9,258 ms | 1,042 ms | **9x** |

**索引支持结论**: 100% ✅ — 这是 explorgdb 最成熟的模块之一

---

## 7. 其他功能对比

| 功能 | GDAL | explorgdb | 说明 |
|------|:----:|:---------:|------|
| 多线程并发读取 | ✅ | ✅ | shared_mutex 保护 |
| 读写并发 | ⚠️ | ❌ | OpenFileGDB 有限制 |
| CDF 压缩表 | ❌ | ❌ | 同 GDAL 限制 |
| SDC 压缩表 | ❌ | ❌ | 同 GDAL 限制 |
| 事务 (StartTransaction) | ⚠️ | ❌ | 模拟事务，只读场景不需要 |
| 字段名清洗 | ✅ | ❌ | 未实现 GetLaunderedFieldName |
| 保留字转义 | ✅ | ❌ | 同上 |
| 调试输出 (CPL_DEBUG) | ✅ | ❌ | 无类似机制 |
| 错误处理 | ✅ | ✅ | 返回 bool + 错误消息 |

---

## 8. 只读生产可行性分析

### 8.1 总体结论

**fast-gdb 应作为生产化主线，但还需要补齐几个明确缺口。**

explorgdb 在常规点/线/面数据上提供了 **9-12x 的读取性能提升**，索引查询也已经成熟。当前策略不应是“遇到缺口就默认回退 GDAL”，而是优先在 fast-gdb 内补齐 SRS、能力检测、字段跳过一致性和特殊几何处理。

### 8.2 ✅ 适合的场景

| 场景 | 说明 | 例子 |
|------|------|------|
| 批量全表扫描 | seq_scan 0.1 us/要素 | 数据迁移、ETL 管道 |
| 空间索引查询 | 小窗口 16x 加速 | 地图切片、空间分析 |
| 属性索引查询 | 13-21x 稳定加速 | 按属性筛选要素 |
| 字段值零拷贝读取 | 无分配无拷贝 | 高性能数据导出 |
| 大数据量只读服务 | 10M 要素全表扫描 1s | 内部分析、报表 |
| 不需要坐标系的数据处理 | 无 SRS 依赖 | 格式转换(不重投影) |

### 8.3 ❌ 不适合的场景

| 场景 | 根因 | 影响 |
|------|------|------|
| 需要坐标系信息 | SRS 完全缺失 | 无法做重投影、坐标变换 |
| 需要 SQL 查询 | 无 ExecuteSQL | 无法用 SQL 表达式过滤 |
| 包含曲线几何 | nCurves 被跳过 | 曲线段丢失 |
| 需要 MultiPatch 标准 WKT | 输出描述文本 | GIS 工具链无法解析 |
| 需要字段域约束 | 未实现 | 取值验证不可用 |
| 需要关系类关联 | 未实现 | 无法跨表 join |
| 需要 GIS 桌面软件集成 | 需要完整 OGRLayer API | 无法替代 GDAL 驱动 |

### 8.4 ⚠️ 有风险但可缓解的场景

| 场景 | 风险 | 缓解方案 |
|------|------|---------|
| 包含曲线几何 | 曲线定义未解析 | 先检测并标记 unsupported；随后实现曲线解码或线性化 |
| 包含 MultiPatch | 输出非标准 WKT，无法被 GIS 工具接受 | 在 fast-gdb 内输出标准 GeometryCollection / TIN / PolyhedralSurface |
| 需要坐标系信息 | 无 SRS 输出 | 从系统目录定位 SpatialRefs/Items 并读取 WKT/WKID |
| 旧版 GDAL 创建的数据 | INT_MIN origin 标记 | 已有 clamp 处理（offset=-2147483647 → 0.0） |
| 数据含有 Raster 字段 | 像素数据未解码 | 先暴露字段存在和元数据，矢量读取不因 Raster 字段失败 |

### 8.5 fast-gdb 优先目标架构

```
数据 → GdbOpenContext
          │
          ├─ CatalogResolver：按表名定位系统表，不写死 aXXXXXXXX
          ├─ CapabilityReport：记录 SRS/曲线/MultiPatch/Raster/索引能力
          ├─ GdbTableParser：统一字段读取与跳过规则
          ├─ QueryEngine：.spx/.atx + peek_bbox + 顺序扫描
          └─ MetadataReader：SRS、字段域、关系、XML 元数据
```

**open 阶段应产出的能力检测条件**：

1. `has_srs()` / `spatial_ref_status` — 是否能定位并解析 SRS。
2. `has_curve_geometry()` — 是否包含曲线扩展标记。
3. `has_multipatch()` — 是否包含 MultiPatch 或 GeneralMultiPatch。
4. `has_raster_field()` — 是否包含 Raster 字段。
5. `has_spatial_index()` / `has_attribute_indexes()` — 是否可走 .spx/.atx。
6. `unsupported_reasons` — fast-gdb 当前无法完整表达的原因列表。

**策略**：fast-gdb 返回清晰的 capability report。上层默认使用 fast-gdb；如果某项能力尚未实现，先暴露明确错误或降级能力，不把 GDAL 回退作为架构主线。GDAL 主要用于基准测试、兼容性验证和临时人工对照。

### 8.6 覆盖率估算

基于典型生产场景的混合分析：

| 数据类型 | 估算占比 | fast-gdb 路线 | 说明 |
|---------|:----:|:---------------:|------|
| 常规点/线/面 | 85% | ✅ 已适合主路径 | 最常见场景，直接使用 |
| 含曲线几何 | 2% | ⚠️ 检测后补解码 | 先避免静默丢失，再补曲线表达 |
| 含 MultiPatch | 3% | ⚠️ 补标准 WKT | 输出 GeometryCollection/TIN 等标准表达 |
| 需要 SRS/重投影 | 5% | 🔴 优先补 SRS | 先输出 WKT/WKID，重投影可后置 |
| 混合数据 | 5% | ⚠️ 按图层 capability 管理 | 不以整个 GDB 为单位放弃 fast-gdb |

**注意**：以上比例是经验估算，不是生产样本统计。真正的生产覆盖率应通过实际数据集抽样扫描 `CapabilityReport` 后给出。

---

## 9. 迁移路线图

### 9.1 短期（1-2 周，高优先级）

| 阶段 | 内容 | 工作量 | 收益 |
|------|------|:------:|:----:|
| 统一字段跳过逻辑 | 修复 DateTimeWithOffset 在 peek 路径的 10 字节跳过规则 | 0.5 天 | 避免空间过滤错位 |
| 系统表定位器 | 通过 SystemCatalog 按表名定位 SpatialRefs/Items | 1 天 | 避免写死系统表编号 |
| 补 SRS 支持 | 读取 WKT/WKID/LatestWKID，形成 fast-gdb 元数据结构 | 2-3 天 | 消除最大功能 gap |
| CapabilityReport | open 阶段输出 SRS/曲线/MultiPatch/Raster/索引能力 | 1-2 天 | 让上层可安全选择 fast-gdb 能力 |
| 曲线几何检测 | 检测曲线扩展标记并禁止静默丢失 | 0.5 天 | 避免错误数据输出 |

### 9.2 中期（2-4 周，中优先级）

| 阶段 | 内容 | 工作量 | 收益 |
|------|------|:------:|:----:|
| MultiPatch 标准 WKT | 将坐标输出为 OGC MultiSurface 或 GeometryCollection | 1-2 天 | 消除 MultiPatch gap |
| 字段域支持 | 从系统表解析 coded/range domain | 2-3 天 | 对齐 GDAL 元数据能力 |
| 高级元数据 API | GetMetadataItem 风格键值接口 | 1-2 天 | 简化上层集成 |
| fast-gdb 查询门面 | 封装顺序扫描、FID、.spx、.atx 查询入口 | 2-3 天 | 上层不直接拼底层 parser |

### 9.3 长期（可选，低优先级）

| 阶段 | 内容 | 工作量 | 收益 |
|------|------|:------:|:----:|
| 精简表达式过滤器 | 支持常见 WHERE 子句子集 | 1-2 周 | 减少上层手写过滤 |
| 曲线几何解码 | 实现 CircularArc → wkbCircularString | 1-2 周 | 100% 功能覆盖 |
| 关系类 | 解析 GDB_Items 中的关系定义 | 1 周 | 对齐高级 GIS 功能 |

---

## 10. 方法论说明

### 对比方法

本矩阵的对比原则：
1. **同层对比**：explorgdb 的底层二进制解析能力 vs GDAL OpenFileGDB 驱动的底层能力
2. **功能可用性**：是否能够实现相同的功能，不要求 API 签名一致
3. **性能优先**：在功能等价的前提下，explorgdb 的性能优势是加分项，但不作为功能覆盖的判断依据

### 数据来源

- GDAL 3.9.3 OpenFileGDB 驱动源码
- explorgdb reader 源码（`src/edgar/explorgdb/reader/`）
- explorgdb common 类型定义（`src/edgar/explorgdb/common/`）
- explorgdb 测试（`tests/edgar/explorgdb/reader/`）
- GDAL 学习文档（`docs/gdal_source_docs/`）

---

**上一篇**：[性能基准与优化](../technical/01_性能基准与优化.md)
**下一篇**：[项目状态与规划](01_项目状态与规划.md)
