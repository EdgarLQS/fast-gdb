# 09 — fast-gdb 曲线几何分析

**更新日期**：2026-07-10  
**当前决策**：保留分析和显式保护；当前 reader 不实现曲线标准输出

## 1. 当前实现状态

v2 已完成：

- GeneralPolyline / GeneralPolygon 只有在 `geom_type & 0x20000000` 时读取 `nCurves`。
- 普通 General 线面不再把 bbox 值误读成曲线数量。
- `nCurves > 0` 时返回 `UNSUPPORTED_CURVE_GEOMETRY(nCurves=...)`。
- 空间精确过滤 fail closed。
- QueryEngine 在 `fallback_reason` 中说明曲线记录被跳过。
- 合成 fixture 不再为无 Curve flag 的 General 几何写入 `nCurves=0`。

当前未实现：

- CircularArc、Cubic Bezier、EllipticArc 参数还原；
- 按 start vertex index 重建直线段和曲线段；
- `CIRCULARSTRING` / `COMPOUNDCURVE` 输出；
- 带误差模型的线性化；
- 理解真实曲线的精确空间过滤。

## 2. Curve flag 格式规则

| 标志 | 值 | 含义 |
|------|------|------|
| Z | `0x80000000` | 包含 Z 数组 |
| M | `0x40000000` | 包含 M 数组 |
| Curve | `0x20000000` | header 包含 `nCurves` 和曲线描述符 |

低字节类型：

- 50：GeneralPolyline
- 51：GeneralPolygon
- 52：GeneralPoint
- 53：GeneralMultiPoint
- 54：GeneralMultiPatch

统一判断：

```cpp
const bool has_curve_desc =
    (base_type == 50 || base_type == 51) &&
    (geom_type & 0x20000000ULL) != 0;
```

不能使用 `base_type >= 50` 代替 Curve flag。

## 3. 二进制布局

```text
geom_type
nPoints
nParts
[nCurves]          仅 Curve flag 置位时存在
bbox
part_sizes
XY vertex arrays
[Z vertex array]
[M vertex array]
[curve descriptors]
```

曲线描述符至少包含起始顶点索引、曲线类型和类型专属参数。GDAL 的参考实现位于 OpenFileGDB 驱动的 `filegdbtable.cpp` 及相关私有头文件。

## 4. 当前发布行为

1. 无 Curve flag：按普通 General 线面读取。
2. 有 Curve flag 且 `nCurves > 0`：明确 unsupported。
3. 不把端点弦线输出为伪普通线面。
4. 不使用隐式 GDAL runtime fallback。
5. 曲线记录参与空间查询时 fail closed，并返回明确 reason。

## 5. v3 边界任务

Curve flag 置位但 `nCurves == 0` 时，decode、peek 和空间过滤应继续按普通 General 线面读取。目前该边界统一工作进入 [10_fast-gdb-v3几何正确性与真实数据计划.md](10_fast-gdb-v3几何正确性与真实数据计划.md)。

真实曲线样本仍需满足：

- GDAL `hasCurveGeometry(TRUE)` 确认存在非线性几何；
- fast-gdb 明确 unsupported；
- 不产生伪 `MULTILINESTRING` / `MULTIPOLYGON`。

## 6. 未来完整实现要求

若恢复曲线标准输出，必须独立完成：

- CircularArc / Bezier / EllipticArc descriptor 解析；
- part 内直线和曲线段重建；
- 标准 WKT 或带误差容限的线性化；
- bbox 和精确空间过滤共用同一曲线模型；
- ArcGIS Pro 真实曲线数据回归。

少量字符串拼接不能视为曲线实现完成。
