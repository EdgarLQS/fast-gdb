# testcurve.gdb 数据集生成说明

## 概述

本脚本在 `testcurve.gdb` 中创建完整的矢量测试数据集，覆盖 File Geodatabase 支持的主要矢量数据类型和几何结构。

**使用条件**：ArcGIS Pro 3.0+（含 arcpy）

**执行命令**：
```bash
"D:\software\install\ArcPro301\bin\Python\envs\arcgispro-py3\python.exe" generate_all_data.py
```

---

## 数据目录结构

```
testcurve.gdb
├── VectorData (Feature Dataset)        ← 主数据集：基础几何 + 曲线 + 椭圆 + M/ZM
│   ├── Point_FC              (POINT)      3 要素
│   ├── MultiPoint_FC         (MULTIPOINT) 1 要素（5 点）
│   ├── Polyline_FC           (POLYLINE)   2 要素（含多 part）
│   ├── Polygon_FC            (POLYGON)    3 要素（含洞、岛中岛）
│   ├── Point_Z_FC            (POINT)      3 要素（Z 值）
│   ├── Polyline_ZM_FC        (POLYLINE)   1 要素（Z+M 值）
│   ├── Polygon_Z_FC          (POLYGON)    1 要素（3D 带洞）
│   ├── Curve_Polyline_FC     (POLYLINE)   8 要素 ← 4 原生曲线 + 4 补充 Bezier
│   ├── Curve_Polygon_FC      (POLYGON)    2 要素（曲线面 + 曲线洞）
│   ├── Multpatch_FC          (MULTIPATCH) 0 要素（Pro 3.0.1 arcpy 限制）
│   ├── Circle_FC             (POLYGON)    2 要素（完整圆，2 段 CircularArc）
│   ├── Ellipse_FC            (POLYGON)    2 要素（4 段 Bezier 近似椭圆）
│   ├── RotatedEllipse_FC     (POLYGON)    3 要素（旋转 30/45/60°）
│   ├── EllipseArc_FC         (POLYLINE)   2 要素（椭圆弧）
│   ├── Point_M_FC            (POINT)      3 要素（M-only）
│   ├── Polyline_M_FC         (POLYLINE)   1 要素（M-only）
│   ├── Point_ZM_FC           (POINT)      3 要素（Z+M）
│   └── Polygon_ZM_FC         (POLYGON)    1 要素（ZM 带洞）
│
├── FIDTest (Feature Dataset)           ← FID 间断测试
│   ├── Point_FIDGap          (POINT)      4 要素（FID: 1,2,6,7）
│   ├── Polyline_FIDGap       (POLYLINE)   3 要素（FID: 1,3,5）
│   └── Polygon_FIDGap        (POLYGON)    2 要素（FID: 2,4）
│
└── BadTopology (Feature Dataset)       ← 坏拓扑测试
    ├── SelfIntersect_Polygon (POLYGON)   1 要素（蝴蝶结自交）
    ├── DegenerateRing_Polygon(POLYGON)   1 要素（三点共线退化）
    ├── RepeatedPoint_Polyline(POLYLINE)  1 要素（重复点）
    └── ZeroArea_Polygon      (POLYGON)   1 要素（零面积）
```

---

## 数据覆盖矩阵

| 验收项 | 图层 | 内容 | 数量 |
|-------|------|------|------|
| 普通 2D Point | `Point_FC` | (0,0), (1,2), (3,1) | 3 |
| MultiPoint | `MultiPoint_FC` | 5 点 (0,0)→(4,2) | 1 |
| 普通 Polyline | `Polyline_FC` | 单 part + 多 part | 2 |
| 普通 Polygon + 洞 | `Polygon_FC` | 矩形 + 面包圈 + 岛中岛 | 3 |
| Z 几何 | `Point_Z_FC`, `Polygon_Z_FC` | 3D 点 (Z=0,5,10), 3D 带洞面 | 4 |
| ZM 几何 | `Polyline_ZM_FC`, `Point_ZM_FC`, `Polygon_ZM_FC` | ZM 线/点/面 | 5 |
| M-only 几何 | `Point_M_FC`, `Polyline_M_FC` | M=0,10,20 | 4 |
| CircularArc | `Curve_Polyline_FC` | 三点圆弧 (`{"c": [...]}`) | 1 |
| Bezier | `Curve_Polyline_FC` | 宽控制/S形/直线混合/多part | 5 |
| 混合曲线 | `Curve_Polyline_FC` | 直线+圆弧+直线 | 1 |
| 多 part 曲线 | `Curve_Polyline_FC` | CircularArc + Bezier 分离 | 1 |
| 曲线面 + 曲线洞 | `Curve_Polygon_FC` | 圆弧外环+直线洞, 曲线外环+曲线洞 | 2 |
| 完整圆 | `Circle_FC` | 2 段 CircularArc 闭合 (r=10, r=20) | 2 |
| 椭圆 | `Ellipse_FC` | 4 段 Bezier 近似 (10×5, 15×4) | 2 |
| 旋转椭圆 | `RotatedEllipse_FC` | 30°/45°/60° | 3 |
| 椭圆弧 | `EllipseArc_FC` | 半弧 + 旋转弧 | 2 |
| FID 间断 | `FIDTest/*` | Point/Polyline/Polygon | 9 |
| 坏拓扑 | `BadTopology/*` | 自交/退化/重复点/零面积 | 4 |
| MultiPatch | `Multipatch_FC` | **空（Pro 3.0.1 arcpy 限制）** | 0 |

---

## 字段模型

每个要素类（除 FID/BadTopology 外）均包含以下公共字段：

| 字段名 | 类型 | 说明 |
|--------|------|------|
| Name | Text(50) | 名称 |
| Description | Text(255) | 描述 |
| Category | Text(20) | 分类编码 |
| CountValue | Short | 整数值 |
| Area_Size | Double | 浮点值 |
| Ratio | Float | 比值 |
| IsActive | Text(5) | "Y"/"N" |
| CreateDate | Date | 创建时间 |
| UniqueID | GUID | UUID |

---

## 坐标系

- **EPSG:3857** (WGS 84 / Web Mercator)
- 如需更换坐标系，修改脚本中 `SR = arcpy.SpatialReference(3857)` 行

---

## 已知限制

| 限制 | 原因 | 解决 |
|------|------|------|
| MultiPatch 为空 | Pro 3.0.1 arcpy.AsShape() 不支持 multipatch 几何 | 需在 ArcGIS Pro 中手动创建 |
| 原生椭圆弧 | Pro 3.0.1 不支持 ESRI JSON 椭圆弧格式 | 椭圆用 4 段 Bezier 近似 |
| Bezier 曲线 GDAL 线性化 | GDAL FileGDB 驱动不支持原生 Bezier 读取 | 不影响 fast-gdb 原生解析 |

---

## 重跑说明

需要重建数据时，直接执行：
```bash
"D:\software\install\ArcPro301\bin\Python\envs\arcgispro-py3\python.exe" generate_all_data.py
```

脚本会删除所有现有数据并重建，可重复执行。