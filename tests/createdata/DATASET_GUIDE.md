# testcurve.gdb 数据集生成说明

本文保留 `testcurve.gdb` 的图层、字段和预期结果细节。测试数据分层、跨平台验证、环境变量与 GDAL 对照流程统一见
[`docs/usage/03_测试数据准备与跨平台验证.md`](../../docs/usage/03_测试数据准备与跨平台验证.md)。

## 概述

本脚本在 `testcurve.gdb` 中创建完整的矢量测试数据集，覆盖 File Geodatabase 支持的主要矢量数据类型和几何结构，用于 `fast-gdb` 项目的真实验收。

**使用条件**：ArcGIS Pro 3.0+（含 arcpy）

**输出配置**：通过环境变量指定本机可重建的测试目录：

**执行命令**（PowerShell，使用 ArcGIS Pro 自带 Python）：

```powershell
$env:FAST_GDB_ARCPY_OUTPUT = 'C:\fast-gdb-data\testcurve.gdb'
& 'C:\Program Files\ArcGIS\Pro\bin\Python\envs\arcgispro-py3\python.exe' `
  tests\createdata\generate_all_data.py
```

脚本会删除并重建 `GDB` 指向的数据，不能指向唯一的业务数据。下文的 `testcurve.gdb` 均指该可配置输出目录。

---

## 数据目录结构

```
testcurve.gdb (41.1 MB, 1,120,080 要素, 44 图层)
│
├── CRS_WGS84_Point_FC                ← 独立 CRS 测试（GDB 根目录）
├── CRS_CGCS2000_Point_FC
├── CRS_Xian80_Point_FC
│
├── VectorData (Feature Dataset)      ← 主数据集：基础几何 + 曲线 + 椭圆 + Z/M/ZM + 补充
│   ├── Point_FC              (POINT)      3 要素
│   ├── MultiPoint_FC         (MULTIPOINT) 1 要素（5 点）
│   ├── Polyline_FC           (POLYLINE)   2 要素（含多 part）
│   ├── Polygon_FC            (POLYGON)    3 要素（含洞、岛中岛）
│   ├── Point_Z_FC            (POINT)      3 要素（Z 值）
│   ├── Polyline_ZM_FC        (POLYLINE)   1 要素（Z+M 值）
│   ├── Polygon_Z_FC          (POLYGON)    1 要素（3D 带洞）
│   ├── Curve_Polyline_FC     (POLYLINE)   8 要素 ← 4 原生曲线 + 4 补充 Bezier
│   ├── Curve_Polygon_FC      (POLYGON)    2 要素（曲线面 + 曲线洞）
│   ├── Multpatch_FC          (MULTIPATCH) 3 要素（2/3/1 part，含多面）
│   ├── MultiPolygon_FC       (POLYGON)    3 要素（多面：2part/3part/single）
│   ├── Empty_Point_FC        (POINT)      1 要素（空几何）
│   ├── Empty_Polyline_FC     (POLYLINE)   1 要素（空几何）
│   ├── Empty_Polygon_FC      (POLYGON)    1 要素（空几何）
│   ├── Curve_Polyline_Z_FC   (POLYLINE)   2 要素（曲线 + Z 启用）
│   ├── Curve_Polyline_M_FC   (POLYLINE)   2 要素（曲线 + M 启用）
│   ├── Curve_Polygon_MultiHole_FC (POLYGON) 2 要素（曲线外环 + 2/3 直线洞）
│   ├── Curve_Polygon_Island_FC    (POLYGON) 2 要素（曲线岛中岛、复合曲线岛中岛）
│   ├── Circle_FC             (POLYGON)    2 要素（完整圆，2 段 CircularArc）
│   ├── Ellipse_FC            (POLYGON)    2 要素（4 段 Bezier 近似椭圆）
│   ├── RotatedEllipse_FC     (POLYGON)    3 要素（旋转 30/45/60°）
│   ├── EllipseArc_FC         (POLYLINE)   2 要素（椭圆弧）
│   ├── Point_M_FC            (POINT)      3 要素（M-only）
│   ├── Polyline_M_FC         (POLYLINE)   1 要素（M-only）
│   ├── Point_ZM_FC           (POINT)      3 要素（Z+M）
│   └── Polygon_ZM_FC         (POLYGON)    1 要素（ZM 带洞）
│
├── FIDTest (Feature Dataset)         ← FID 间断测试
│   ├── Point_FIDGap          (POINT)      4 要素（FID: 1,2,6,7）
│   ├── Polyline_FIDGap       (POLYLINE)   3 要素（FID: 1,3,5）
│   ├── Polygon_FIDGap        (POLYGON)    2 要素（FID: 2,4）
│   ├── Point_FIDExact        (POINT)      4 要素（FID: 1,2,6,10）
│   └── Polyline_FIDExact     (POLYLINE)   3 要素（FID: 1,4,8）
│
├── BadTopology (Feature Dataset)     ← 坏拓扑测试（.spx 已删除）
│   ├── SelfIntersect_Polygon (POLYGON)   1 要素（蝴蝶结自交 → arcpy 拆为 2 三角）
│   ├── DegenerateRing_Polygon(POLYGON)   1 要素（三点共线退化 → 空几何）
│   ├── RepeatedPoint_Polyline(POLYLINE)  1 要素（重复点 → arcpy 自动去重）
│   ├── ZeroArea_Polygon      (POLYGON)   1 要素（零面积 → 空几何）
│   ├── MultipleRepeatRing_Polygon (POLYGON) 1 要素（3 同心环 → arcpy 自动合并）
│   └── DuplicateRing_Polygon(POLYGON)   1 要素（完全重合环 → 空几何）
│
└── PerfTest (Feature Dataset)       ← 大规模性能数据
    ├── Perf_Point_100k       (POINT)     100,000 要素
    ├── Perf_Polyline_10k     (POLYLINE)  10,000 要素
    ├── Perf_Polygon_10k      (POLYGON)   10,000 要素
    └── Perf_Point_1M         (POINT)     1,000,000 要素
```

---

## 数据覆盖矩阵

| 验收项 | 图层 | 内容 | 数量 |
|-------|------|------|------|
| 普通 2D Point | `Point_FC` | (0,0), (1,2), (3,1) | 3 |
| MultiPoint | `MultiPoint_FC` | 5 点 (0,0)→(4,2) | 1 |
| 普通 Polyline | `Polyline_FC` | 单 part + 多 part | 2 |
| 普通 Polygon + 洞 | `Polygon_FC` | 矩形 + 面包圈 + 岛中岛 | 3 |
| MultiPolygon（多面） | `MultiPolygon_FC` | 2part/3part/single 多面要素 | 3 |
| Z 几何 | `Point_Z_FC`, `Polygon_Z_FC` | 3D 点 (Z=0,5,10), 3D 带洞面 | 4 |
| ZM 几何 | `Polyline_ZM_FC`, `Point_ZM_FC`, `Polygon_ZM_FC` | ZM 线/点/面 | 5 |
| M-only 几何 | `Point_M_FC`, `Polyline_M_FC` | M=0,10,20 | 4 |
| 空几何 | `Empty_Point_FC`, `Empty_Polyline_FC`, `Empty_Polygon_FC` | 空几何（SHAPE@=None） | 3 |
| CircularArc | `Curve_Polyline_FC` | 三点圆弧 (`{"c": [...]}`) | 1 |
| Bezier | `Curve_Polyline_FC` | 宽控制/S形/直线混合/多part | 5 |
| 混合曲线 | `Curve_Polyline_FC` | 直线+圆弧+直线 | 1 |
| 多 part 曲线 | `Curve_Polyline_FC` | CircularArc + Bezier 分离 | 1 |
| 曲线面 + 曲线洞 | `Curve_Polygon_FC` | 圆弧外环+直线洞, 曲线外环+曲线洞 | 2 |
| 曲线面多洞 | `Curve_Polygon_MultiHole_FC` | 曲线外环 + 2/3 直线洞 | 2 |
| 曲线面岛中岛 | `Curve_Polygon_Island_FC` | 曲线外环→曲线洞→曲线岛, 含复合岛中岛 | 2 |
| 曲线 + Z | `Curve_Polyline_Z_FC` | 2D 曲线写入 Z-enabled FC（Z 自动补 0） | 2 |
| 曲线 + M | `Curve_Polyline_M_FC` | 2D 曲线写入 M-enabled FC | 2 |
| 完整圆 | `Circle_FC` | 2 段 CircularArc 闭合 (r=10, r=20) | 2 |
| 椭圆 | `Ellipse_FC` | 4 段 Bezier 近似 (10×5, 15×4) | 2 |
| 旋转椭圆 | `RotatedEllipse_FC` | 30°/45°/60° | 3 |
| 椭圆弧 | `EllipseArc_FC` | 半弧 + 旋转弧 | 2 |
| CRS 测试 | `CRS_WGS84_Point_FC` | WGS84 (EPSG:4326) | 1 |
| CRS 测试 | `CRS_CGCS2000_Point_FC` | CGCS2000 (EPSG:4490) | 1 |
| CRS 测试 | `CRS_Xian80_Point_FC` | Xian 80 (EPSG:4610) | 1 |
| FID 间断 | `FIDTest/*` | Point/Polyline/Polygon 多种序列 | 15 |
| FID 精确序列 | `FIDTest/Point_FIDExact`, `Polyline_FIDExact` | 1,2,6,10 / 1,4,8 | 7 |
| 坏拓扑 | `BadTopology/*` | 自交/退化/重复点/零面积/多重环/重合环 | 6 |
| 大规模点 | `PerfTest/Perf_Point_100k` | 100k 随机点 | 100,000 |
| 大规模线 | `PerfTest/Perf_Polyline_10k` | 10k 随机折线 | 10,000 |
| 大规模面 | `PerfTest/Perf_Polygon_10k` | 10k 随机矩形面 | 10,000 |
| 超大规模点 | `PerfTest/Perf_Point_1M` | 1M 随机点（~30MB） | 1,000,000 |
| MultiPatch | `Multipatch_FC` | 3 要素（2part/3part/single, 多面） | 3 |

---

## 字段模型

每个要素类（除 FID/BadTopology/PerfTest 外）均包含以下公共字段：

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

| 坐标系 | WKID | 适用图层 |
|--------|------|---------|
| WGS 84 / Web Mercator | **EPSG:3857** | 主坐标系（VectorData/FIDTest/BadTopology/PerfTest） |
| WGS 84 | **EPSG:4326** | `CRS_WGS84_Point_FC`（GDB 根目录） |
| CGCS2000 | **EPSG:4490** | `CRS_CGCS2000_Point_FC`（GDB 根目录） |
| Xian 80 | **EPSG:4610** | `CRS_Xian80_Point_FC`（GDB 根目录） |

---

## 已知限制

| 限制 | 原因 | 解决 |
|------|------|------|
| 原生 3D 曲线 (Curve+Z/M/ZM) | arcpy 3.5 不支持 ESRI JSON 曲线格式含 Z/M 坐标 | Curve_Polyline_Z/M_FC 插入 2D 曲线（Z/M 自动补 0） |
| 原生椭圆弧 | Pro 3.5 不支持 ESRI JSON 椭圆弧格式 `{"e": [...]}` | 椭圆用 4 段 Bezier 近似 |
| 坏拓扑自动修复 | arcpy 自动修复自交/去重/空几何 | 数据入库后非原始"坏"结构，文档注明预期行为 |
| Bezier 曲线 GDAL 线性化 | GDAL FileGDB 驱动不支持原生 Bezier 读取 | 不影响 fast-gdb 原生解析 |

---

## 侧文件清单

脚本执行后在同一目录生成以下文件：

| 文件 | 说明 |
|------|------|
| `testcurve_layer_inventory.csv` | 44 个图层的名称/几何类型/要素数/曲线标识/CRS |
| `testcurve_fid_mapping.csv` | FIDTest 所有图层的 ObjectID、Name 对照表 |
| `testcurve_expected_results.json` | 关键要素的 bbox、area、length、hasCurves、pointCount |
| `testcurve_spatial_cases.csv` | 空间查询测试用例（点包含测试） |
| `testcurve_manifest.json` | 数据集元信息（来源、限制、组成） |
| `testcurve_source_notes.md` | 详细数据来源和创建说明 |

---

## 空间索引

- **VectorData** 和 **PerfTest** 图层：保留有效的 `.spx` 空间索引
- **BadTopology** 图层：`.spx` 已删除，模拟缺失空间索引场景
- **FIDTest** 图层：保留有效 `.spx`

---

## 重跑说明

需要重建数据时，确认 `FAST_GDB_ARCPY_OUTPUT` 后重新执行上面的 ArcGIS Pro Python 命令。

脚本会删除所有现有数据并重建，可重复执行。
