# 02 — fast-gdb / GDAL 功能对比矩阵

**更新日期**：2026-07-11  
**文档状态**：当前权威能力矩阵  
**对比对象**：`explorgdb reader` 与 GDAL OpenFileGDB 只读能力

## 1. 状态约定

| 标记 | 含义 |
|------|------|
| ✅ | 已实现，自动化边界明确 |
| 🧪 | 已实现并通过本地自动化，但缺真实 FileGDB 验收 |
| ⚠️ | 部分实现或降级支持 |
| ❌ | 当前不支持 |
| ⏸️ | 当前版本明确不实施 |

## 2. 快速结论

| 维度 | fast-gdb | 结论 |
|------|:---:|------|
| 常规点/线/面/多点 | ✅ | 本地自动化通过，内置真实普通样本已验收 |
| GeneralPolyline / GeneralPolygon | 🧪 | Curve flag/header 已修正；曲线记录明确 unsupported |
| GeneralPoint / GeneralMultiPoint | 🧪 | 主 decode 和本地自动化已覆盖；仍待包含 GeneralPoint/GeneralMultiPoint 的真实样本兜底 |
| 曲线标准输出 | ❌ | 不还原参数、不线性化 |
| MultiPatch | ⚠️ | 坐标和 WKT 语法可输出；part type/拓扑不保留，capability 为 degraded |
| 字段类型 | ⚠️ | 物理记录可解析；Raster 和 DateTimeWithOffset 有边界 |
| SRS | ✅ | WKT/WKID/LatestWKID；不重投影 |
| 高级元数据 | ✅/⚠️ | domain、relationship、Feature Dataset 已实现；Annotation/Dimension 未实现 |
| 查询 | ✅/⚠️ | FID、scan、bbox、`.spx`、`.atx`、WHERE 子集；不是完整 SQL/OGRLayer |

## 3. 几何类型

### 3.1 常规类型

| 类型 | fast-gdb | 当前边界 |
|------|:---:|----------|
| Point / PointZ / PointM / PointZM | ✅ | decode、peek、空间过滤坐标公式已统一；内置真实普通样本已验收 |
| MultiPoint 及 Z/M/ZM | ✅ | decode 已实现；`peek_bbox` 按 `nPoints + bbox` 布局读取；内置真实普通样本已验收 |
| Polyline 及 Z/M/ZM | ✅ | 输出 `MULTILINESTRING`；内置真实普通样本已验收 |
| Polygon 及 Z/M/ZM | ✅ | 输出 `MULTIPOLYGON`；环拓扑不宣称与 OGR 完全等价 |
| Null / Empty | ✅ | 有明确 EMPTY 输出 |

### 3.2 General 类型

| 类型 | fast-gdb | 当前边界 |
|------|:---:|----------|
| GeneralPoint (52) | 🧪 | 完整 `decode()` 已接入 point 解码；本地自动化覆盖，真实样本待验收 |
| GeneralMultiPoint (53) | 🧪 | 完整 `decode()` 已接入 multipoint 解码；本地自动化覆盖，真实样本待验收 |
| GeneralPolyline (50) | 🧪 | 非曲线 header 已修正；真实样本待验收 |
| GeneralPolygon (51) | 🧪 | 非曲线 header 已修正；真实样本待验收 |
| GeneralMultiPatch (54) | ⚠️ | 与 MultiPatch 相同的降级语义 |

General 线面的当前规则：

```cpp
const bool has_curve_desc =
    (base_type == 50 || base_type == 51) &&
    (geom_type & 0x20000000ULL) != 0;
```

只有 Curve flag 置位时读取 `nCurves`；`nCurves > 0` 返回 `UNSUPPORTED_CURVE_GEOMETRY`，空间过滤 fail closed；`nCurves == 0` 按普通 General 线面继续 decode、peek 和空间过滤。

### 3.3 曲线

| 曲线类型 | fast-gdb | 发布行为 |
|----------|:---:|----------|
| CircularArc | ❌ | 明确 unsupported |
| Cubic Bezier | ❌ | 明确 unsupported |
| EllipticArc | ❌ | 明确 unsupported |

不输出 `CIRCULARSTRING` / `COMPOUNDCURVE`，不把端点弦线伪装成真实曲线。

### 3.4 MultiPatch

| 能力 | fast-gdb |
|------|:---:|
| XY/Z/M 坐标读取 | ✅ |
| `GEOMETRYCOLLECTION Z/ZM` 输出 | ✅ |
| part type 语义保留 | ❌ |
| TriangleStrip / TriangleFan 重建 | ❌ |
| Ring/Outer/Inner 表面拓扑 | ❌ |
| capability | ⚠️ degraded |

因此 MultiPatch 可读但不是完整语义等价。

## 4. 字段、SRS 和元数据

| 能力 | fast-gdb | 说明 |
|------|:---:|------|
| 常规数值/字符串/XML/Binary/GUID/GlobalID/Int64 | ✅ | 已暴露 |
| DateTimeWithOffset | ⚠️ | 10 字节物理读取安全；offset 未独立暴露 |
| Raster | ⚠️ | 检测并 degraded，不读像素 |
| SRS WKT/WKID/LatestWKID/SRSName | ✅ | 不提供重投影 |
| Definition / Documentation XML | ✅ | 已读取 |
| coded/range domain | ✅ | 已结构化解析 |
| Feature Dataset | ✅ | 已提供摘要 |
| relationship summary/definition | ✅ | 不执行 join/级联 |
| Annotation / Dimension | ❌ | 未实现专用语义 |

## 5. 查询

| 能力 | fast-gdb | 说明 |
|------|:---:|------|
| 顺序扫描 / FID | ✅ | QueryEngine |
| bbox / `.spx` | ✅ | 缺索引时可顺序过滤 |
| `.atx` 数值/字符串 | ✅ | 索引入口已实现 |
| WHERE 比较、AND/OR、括号、IN | ✅ | 明确子集 |
| execution path / fallback reason | ✅ | 曲线跳过也会说明 |
| 完整 SQL/JOIN/聚合/函数 | ❌ | 不在范围 |
| 持久 OGRLayer 状态 | ❌ | 每次 QueryRequest 显式传入 |

## 6. 本地验证

- 构建通过。
- `GeometryTest.*`：`68 passed / 0 failed`。
- 查询和真实数据契约专项：无真实样本环境变量时 `9 passed / 2 skipped / 0 failed`。
- 内置普通真实样本：`RealDataReleaseContractTest.RegularFileGdbMatchesCoreReadContract` 已通过，路径为 `test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb`。
- 全量 runner：`410 passed / 17 skipped / 0 failed`。
- CTest：`427 tests, 0 failed, 17 skipped`。
- 10M benchmark 默认跳过；仅在 `FAST_GDB_RUN_10M_BENCHMARKS=1` 时单独运行。
- 两项真实数据测试在无环境变量时正确 SKIPPED。
- 曲线真实 FileGDB 尚未验收；内置普通样本不包含 GDAL 可识别的原生曲线，不能作为 `FAST_GDB_CURVE_DATASET`。

## 7. v3 状态

以下 v3 P0 几何正确性项已有本地自动化覆盖，其中常规真实 FileGDB 已用内置样本验收；General 类型和曲线仍需专门真实样本兜底：

1. GeneralPoint / GeneralMultiPoint 主 decode。
2. MultiPoint / GeneralMultiPoint `peek_bbox` header。
3. Point 坐标公式在 decode、peek、空间过滤中的一致性。
4. Curve flag + `nCurves == 0` 的一致行为。

仍未关闭的差距：

1. 包含 GeneralPoint / GeneralMultiPoint / GeneralPolyline / GeneralPolygon 的真实 FileGDB 回归。
2. 真实曲线 FileGDB 回归。
3. MultiPatch 完整 part type 语义（可选；未实现前维持 degraded）。

曲线标准输出、GDAL 替换边界和更详细的历史分析已归档到 `archive/`，这里只保留当前能力矩阵。
