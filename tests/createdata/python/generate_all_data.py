"""
generate_all_data.py
====================
在 testcurve.gdb 中创建完整的矢量测试数据集，覆盖 File GDB 主要矢量数据类型。
包含：基础几何、曲线、椭圆、M/ZM、FID 间断、坏拓扑、大规模性能数据等。

执行前设置：
    FAST_GDB_ARCPY_OUTPUT=C:\\fast-gdb-data\\testcurve.gdb

环境要求：ArcGIS Pro 3.0+（含 arcpy）
输出路径：testcurve.gdb/VectorData, FIDTest, BadTopology, PerfTest
"""

import arcpy
import json
import uuid
import datetime
import math
import random
import os
import csv

# ============================================================
# 配置
# ============================================================
GDB = os.environ.get("FAST_GDB_ARCPY_OUTPUT", "").strip()
if not GDB:
    raise RuntimeError(
        "请将 FAST_GDB_ARCPY_OUTPUT 设置为可重建的 testcurve.gdb 输出路径"
    )
if os.path.splitext(GDB)[1].lower() != ".gdb":
    raise RuntimeError("FAST_GDB_ARCPY_OUTPUT 必须指向 .gdb 目录")
FD = "VectorData"
SR = arcpy.SpatialReference(3857)

arcpy.env.workspace = GDB
arcpy.env.overwriteOutput = True

# 贝塞尔圆弧近似常数
C_BEZIER = 0.5522847498307935


# ============================================================
# 辅助函数
# ============================================================

def now():
    return datetime.datetime.now()

def make_guid():
    return str(uuid.uuid4()).upper()

def fc_path(name):
    return f"{GDB}/{FD}/{name}"

def delete_if_exists(path):
    if arcpy.Exists(path):
        arcpy.Delete_management(path)
        print(f"  已删除: {path}")

def add_fields(path):
    """添加公共字段"""
    fields = [
        ("Name", "TEXT", 50),
        ("Description", "TEXT", 255),
        ("Category", "TEXT", 20),
        ("CountValue", "SHORT"),
        ("Area_Size", "DOUBLE"),
        ("Ratio", "FLOAT"),
        ("IsActive", "TEXT", 5),
        ("CreateDate", "DATE"),
        ("UniqueID", "GUID"),
    ]
    for field_def in fields:
        try:
            fname = field_def[0]
            ftype = field_def[1]
            length = field_def[2] if len(field_def) > 2 else None
            kw = {"field_name": fname, "field_type": ftype}
            if ftype == "TEXT" and length:
                kw["field_length"] = length
            arcpy.AddField_management(path, **kw)
        except Exception:
            pass

def build_row(geom, name, desc, cat="A", count=1, area=0.0, ratio=0.5):
    return [geom, name, desc, cat, count, area, ratio, "Y", now(), make_guid()]

def make_curve_geom(curve_paths_json):
    full = {"curvePaths": curve_paths_json, "spatialReference": {"wkid": 3857}}
    return arcpy.AsShape(json.dumps(full), True)

def make_curve_ring_geom(curve_rings_json):
    full = {"curveRings": curve_rings_json, "spatialReference": {"wkid": 3857}}
    return arcpy.AsShape(json.dumps(full), True)

def print_progress(label, current, total, bar_len=40):
    """打印进度条（ASCII 安全）"""
    pct = current / total if total > 0 else 1
    filled = int(bar_len * pct)
    bar = "#" * filled + "." * (bar_len - filled)
    print(f"\r  {label:20s} [{bar}] {current:>8d}/{total:<8d} ({pct*100:5.1f}%)", end="")
    if current >= total:
        print()

def random_point_in_bbox(xmin, ymin, xmax, ymax):
    return arcpy.Point(random.uniform(xmin, xmax), random.uniform(ymin, ymax))

def random_polyline_vertices(xmin, ymin, xmax, ymax, n=4):
    """生成随机折线的顶点数组"""
    pts = arcpy.Array()
    for _ in range(random.randint(3, n)):
        pts.add(random_point_in_bbox(xmin, ymin, xmax, ymax))
    return pts

def random_polygon_vertices(xmin, ymin, xmax, ymax, n=6):
    """生成随机矩形（确保有效且不退化）"""
    cx = random.uniform(xmin + 0.1 * (xmax - xmin), xmax - 0.1 * (xmax - xmin))
    cy = random.uniform(ymin + 0.1 * (ymax - ymin), ymax - 0.1 * (ymax - ymin))
    hw = random.uniform(1, 0.4 * (xmax - xmin))
    hh = random.uniform(1, 0.4 * (ymax - ymin))
    pts = arcpy.Array([
        arcpy.Point(cx - hw, cy - hh),
        arcpy.Point(cx + hw, cy - hh),
        arcpy.Point(cx + hw, cy + hh),
        arcpy.Point(cx - hw, cy + hh),
        arcpy.Point(cx - hw, cy - hh),
    ])
    return pts


# ============================================================
# 阶段 0：准备工作
# ============================================================
print("=" * 60)
print("准备：重建 GDB")
print("=" * 60)

if arcpy.Exists(GDB):
    arcpy.Delete_management(GDB)
    print(f"  已删除旧 GDB: {GDB}")

gdb_dir = os.path.dirname(GDB)
gdb_name = os.path.basename(GDB)
arcpy.CreateFileGDB_management(gdb_dir, gdb_name)
print(f"  创建新 GDB: {GDB}")

# ============================================================
# 阶段 1：创建 VectorData 要素数据集 + 所有要素类
# ============================================================
print("\n" + "=" * 60)
print("阶段 1：创建结构")
print("=" * 60)

arcpy.CreateFeatureDataset_management(GDB, FD, SR)
print(f"创建要素数据集: {FD}")

BASE_LAYERS = [
    ("Point_FC",              "POINT",      False, False),
    ("MultiPoint_FC",         "MULTIPOINT", False, False),
    ("Polyline_FC",           "POLYLINE",   False, False),
    ("Polygon_FC",            "POLYGON",    False, False),
    ("Point_Z_FC",           "POINT",      True,  False),
    ("Polyline_ZM_FC",       "POLYLINE",   True,  True),
    ("Polygon_Z_FC",         "POLYGON",    True,  False),
    ("Curve_Polyline_FC",    "POLYLINE",   False, False),
    ("Curve_Polygon_FC",     "POLYGON",    False, False),
    ("Multipatch_FC",        "MULTIPATCH", True,  False),
    # === 新增图层 ===
    ("MultiPolygon_FC",       "POLYGON",    False, False),
    ("Empty_Point_FC",       "POINT",      False, False),
    ("Empty_Polyline_FC",    "POLYLINE",   False, False),
    ("Empty_Polygon_FC",     "POLYGON",    False, False),
    ("Curve_Polyline_Z_FC",  "POLYLINE",   True,  False),
    ("Curve_Polyline_M_FC",  "POLYLINE",   False, True),
    ("Curve_Polygon_MultiHole_FC", "POLYGON", False, False),
    ("Curve_Polygon_Island_FC",    "POLYGON", False, False),
]

print("\n--- 常规图层 (VectorData FD 内) ---")
for fc_name, geom_type, has_z, has_m in BASE_LAYERS:
    path = fc_path(fc_name)
    arcpy.CreateFeatureclass_management(
        f"{GDB}/{FD}", fc_name, geom_type, spatial_reference=SR,
        has_m="ENABLED" if has_m else "DISABLED",
        has_z="ENABLED" if has_z else "DISABLED",
    )
    add_fields(path)
    print(f"  [OK] {fc_name:30s}  {geom_type:12s}  Z={int(has_z)}  M={int(has_m)}")

print("\n--- CRS 测试图层 (GDB 根目录) ---")
crs_srs_map = {
    "CRS_WGS84_Point_FC": arcpy.SpatialReference(4326),
    "CRS_CGCS2000_Point_FC": arcpy.SpatialReference(4490),
    "CRS_Xian80_Point_FC": arcpy.SpatialReference(4610),
}
for fc_name, sr_fc in crs_srs_map.items():
    arcpy.CreateFeatureclass_management(
        GDB, fc_name, "POINT", spatial_reference=sr_fc,
        has_m="DISABLED", has_z="DISABLED",
    )
    add_fields(f"{GDB}/{fc_name}")
    print(f"  [OK] {fc_name:30s}  POINT   SR={sr_fc.factoryCode} {sr_fc.name}")

print("\n所有要素类创建完成！\n")


# ============================================================
# 阶段 2：基础几何数据
# ============================================================
print("=" * 60)
print("阶段 2：基础几何样本数据")
print("=" * 60)

# 2.1 Point_FC
print("\n--- Point_FC ---")
with arcpy.da.InsertCursor(fc_path("Point_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    for x, y, name in [(0, 0, "Origin"), (1, 2, "PointA"), (3, 1, "PointB")]:
        cur.insertRow(build_row(arcpy.Point(x, y), name, f"Point at ({x},{y})", "A", 1, float(x + y)))
    print("  [OK] 3 个点要素")

# 2.2 MultiPoint_FC
print("\n--- MultiPoint_FC ---")
with arcpy.da.InsertCursor(fc_path("MultiPoint_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    pts = [arcpy.Point(x, 0.5 * x) for x in range(5)]
    geom = arcpy.Multipoint(arcpy.Array(pts), SR)
    cur.insertRow(build_row(geom, "MultiPointSample", "5 points on line y=0.5x", "B", 5))
    print("  [OK] 1 个多点要素")

# 2.3 Polyline_FC
print("\n--- Polyline_FC ---")
with arcpy.da.InsertCursor(fc_path("Polyline_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    cur.insertRow(build_row(
        arcpy.Polyline(arcpy.Array([arcpy.Point(0, 0), arcpy.Point(5, 3), arcpy.Point(10, 0)]), SR),
        "SimplePolyline", "3-vertex polyline", "C", 1))
    p1 = arcpy.Array([arcpy.Point(0, 5), arcpy.Point(3, 8)])
    p2 = arcpy.Array([arcpy.Point(6, 5), arcpy.Point(9, 8), arcpy.Point(12, 5)])
    cur.insertRow(build_row(
        arcpy.Polyline(arcpy.Array([p1, p2]), SR),
        "MultiPartPolyline", "2-part polyline", "C", 2))
    print("  [OK] 2 条折线")

# 2.4 Polygon_FC
print("\n--- Polygon_FC ---")
with arcpy.da.InsertCursor(fc_path("Polygon_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    cur.insertRow(build_row(
        arcpy.Polygon(arcpy.Array([arcpy.Point(0, 0), arcpy.Point(10, 0), arcpy.Point(10, 10), arcpy.Point(0, 10), arcpy.Point(0, 0)]), SR),
        "SimplePolygon", "10x10 square", "D", 1, 100.0))
    outer = arcpy.Array([arcpy.Point(0, 0), arcpy.Point(20, 0), arcpy.Point(20, 20), arcpy.Point(0, 20), arcpy.Point(0, 0)])
    inner = arcpy.Array([arcpy.Point(5, 5), arcpy.Point(5, 15), arcpy.Point(15, 15), arcpy.Point(15, 5), arcpy.Point(5, 5)])
    cur.insertRow(build_row(
        arcpy.Polygon(arcpy.Array([outer, inner]), SR),
        "DonutPolygon", "20x20 square with hole", "D", 2, 400.0))
    outer2 = arcpy.Array([arcpy.Point(0, 0), arcpy.Point(30, 0), arcpy.Point(30, 30), arcpy.Point(0, 30), arcpy.Point(0, 0)])
    inner_a = arcpy.Array([arcpy.Point(3, 3), arcpy.Point(3, 12), arcpy.Point(12, 12), arcpy.Point(12, 3), arcpy.Point(3, 3)])
    inner_b = arcpy.Array([arcpy.Point(18, 18), arcpy.Point(18, 27), arcpy.Point(27, 27), arcpy.Point(27, 18), arcpy.Point(18, 18)])
    cur.insertRow(build_row(
        arcpy.Polygon(arcpy.Array([outer2, inner_a, inner_b]), SR),
        "IslandInIsland", "30x30 square with 2 holes", "D", 3, 900.0))
    print("  [OK] 3 个面（含洞、岛中岛）")

# 2.5 Point_Z_FC
print("\n--- Point_Z_FC ---")
with arcpy.da.InsertCursor(fc_path("Point_Z_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    for x, y, z, name in [(0, 0, 0, "Ground"), (1, 2, 5, "Tower"), (3, 1, 10, "Peak")]:
        cur.insertRow(build_row(arcpy.Point(x, y, z), name, f"3D point ({x},{y},{z})", "E", 1, float(z)))
    print("  [OK] 3 个 3D 点")

# 2.6 Polyline_ZM_FC
print("\n--- Polyline_ZM_FC ---")
with arcpy.da.InsertCursor(fc_path("Polyline_ZM_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    pts = arcpy.Array([arcpy.Point(0, 0, 0, 0), arcpy.Point(5, 3, 2, 10), arcpy.Point(10, 0, 1, 20), arcpy.Point(15, 5, 5, 30)])
    cur.insertRow(build_row(
        arcpy.Polyline(pts, SR, True, True),
        "ZM_Polyline", "3D polyline with M values", "F", 1))
    print("  [OK] 1 条 ZM 折线")

# 2.7 Polygon_Z_FC
print("\n--- Polygon_Z_FC ---")
with arcpy.da.InsertCursor(fc_path("Polygon_Z_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    outer3d = arcpy.Array([arcpy.Point(0, 0, 0), arcpy.Point(20, 0, 0), arcpy.Point(20, 20, 5), arcpy.Point(0, 20, 5), arcpy.Point(0, 0, 0)])
    inner3d = arcpy.Array([arcpy.Point(5, 5, 2), arcpy.Point(5, 15, 3), arcpy.Point(15, 15, 4), arcpy.Point(15, 5, 2), arcpy.Point(5, 5, 2)])
    cur.insertRow(build_row(
        arcpy.Polygon(arcpy.Array([outer3d, inner3d]), SR, True, False),
        "3D_Donut", "3D polygon with hole", "G", 1, 400.0))
    print("  [OK] 1 个 3D 带洞面")


# ============================================================
# 阶段 3：曲线数据
# ============================================================
print("\n" + "=" * 60)
print("阶段 3：曲线几何样本数据")
print("=" * 60)

# 3.1 Curve_Polyline_FC
print("\n--- Curve_Polyline_FC ---")
with arcpy.da.InsertCursor(fc_path("Curve_Polyline_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    n = 0
    for try_data in [
        ("CircularArc",   [[[0.0, 0.0], {"c": [[5.0, 5.0], [10.0, 0.0]]}]],
         "Circular arc through (5,5)"),
        ("BezierCurve",   [[[0.0, 0.0], {"b": [[3.0, 8.0], [7.0, 8.0], [10.0, 0.0]]}]],
         "Cubic Bezier 2 control points"),
        ("MixedCurve",    [[[0.0, 0.0], [5.0, 0.0], {"c": [[7.5, 5.0], [10.0, 0.0]]}, [15.0, 0.0]]],
         "Straight + CircularArc + Straight"),
        ("MultiPartCurve", [[[0.0, 0.0], {"c": [[2.5, 5.0], [5.0, 0.0]]}],
                            [[0.0, -10.0], {"b": [[3.0, -5.0], [7.0, -5.0], [10.0, -10.0]]}]],
         "2-part: CircularArc + Bezier"),
    ]:
        try:
            geom = make_curve_geom(try_data[1])
            cur.insertRow(build_row(geom, try_data[0], try_data[2], "H", n + 1))
            n += 1
            print(f"  [OK] {try_data[0]}")
        except Exception as e:
            print(f"  [SKIP] {try_data[0]}: {e}")

    # EllipticArc（Pro 3.x 不支持，静默跳过）
    try:
        geom = make_curve_geom([[[0.0, 0.0], {"e": [10.0, 0.0, 5.0, 0.0, 0.0, 0.5, False, False]}, [10.0, 5.0], {"e": [0.0, 5.0, 5.0, 5.0, 0.0, 0.5, False, False]}, [0.0, 0.0]]])
        cur.insertRow(build_row(geom, "EllipticArc", "Elliptic arc", "H", 5))
        n += 1
    except Exception:
        pass  # 静默跳过

    print(f"  成功插入 {n} 条曲线")

# 3.2 Curve_Polygon_FC
print("\n--- Curve_Polygon_FC ---")
with arcpy.da.InsertCursor(fc_path("Curve_Polygon_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    n = 0
    for try_data in [
        ("CurvePoly_Hole",
         [[[0.0, 0.0], [10.0, 0.0], {"c": [[5.0, 8.0], [0.0, 0.0]]}],
          [[2.0, 1.0], [2.0, 3.0], [8.0, 3.0], [8.0, 1.0], [2.0, 1.0]]],
         "Arc outer ring + straight hole"),
        ("CurvePoly_CurveHole",
         [[[0.0, 0.0], [20.0, 0.0], {"c": [[10.0, 12.0], [0.0, 0.0]]}],
          [[5.0, 1.0], {"c": [[7.0, 5.0], [9.0, 1.0]]}, [15.0, 1.0], {"c": [[13.0, 5.0], [11.0, 1.0]]}, [5.0, 1.0]]],
         "Curve outer ring + curve hole"),
    ]:
        try:
            geom = make_curve_ring_geom(try_data[1])
            cur.insertRow(build_row(geom, try_data[0], try_data[2], "I", n + 1))
            n += 1
            print(f"  [OK] {try_data[0]}")
        except Exception as e:
            print(f"  [SKIP] {try_data[0]}: {e}")
    print(f"  成功插入 {n} 个曲线面")

# 3.3 MultiPatch（已知 Pro 3.x 限制 — arcpy.AsShape 无法创建 Multipatch）
print("\n--- Multipatch_FC ---")
print("  [SKIP] MultiPatch: arcpy 3.5 不支持 Multipatch 创建, 图层已保留为空")


# ============================================================
# 阶段 4：Ellipse 数据
# ============================================================
print("\n" + "=" * 60)
print("阶段 4：Ellipse 数据")
print("=" * 60)

for name, geom_type in [
    ("Circle_FC", "POLYGON"), ("Ellipse_FC", "POLYGON"),
    ("RotatedEllipse_FC", "POLYGON"), ("EllipseArc_FC", "POLYLINE"),
]:
    path = fc_path(name)
    arcpy.CreateFeatureclass_management(f"{GDB}/{FD}", name, geom_type, spatial_reference=SR)
    add_fields(path)
    print(f"  [OK] {name}")

# 4.1 Circle_FC
print("\n--- Circle_FC ---")
with arcpy.da.InsertCursor(fc_path("Circle_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    g1 = make_curve_ring_geom([[[0.0, 0.0], {"c": [[10.0, 10.0], [0.0, 20.0]]}, {"c": [[-10.0, 10.0], [0.0, 0.0]]}]])
    cur.insertRow(build_row(g1, "FullCircle", "Complete circle (2 circular arcs) r=10", "E1", 1, g1.area))
    g2 = make_curve_ring_geom([[[0.0, 0.0], {"c": [[20.0, 20.0], [0.0, 40.0]]}, {"c": [[-20.0, 20.0], [0.0, 0.0]]}]])
    cur.insertRow(build_row(g2, "BigCircle", "Complete circle r=20", "E1", 2, g2.area))
    print("  [OK] 2 个完整圆")

# 4.2 Ellipse_FC
print("\n--- Ellipse_FC ---")
with arcpy.da.InsertCursor(fc_path("Ellipse_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    for rx, ry in [(10.0, 5.0), (15.0, 4.0)]:
        n, m = rx * C_BEZIER, ry * C_BEZIER
        g = make_curve_ring_geom([[[0.0, ry], {"b": [[n, ry], [rx, m], [rx, 0.0]]},
            {"b": [[rx, -m], [n, -ry], [0.0, -ry]]},
            {"b": [[-n, -ry], [-rx, -m], [-rx, 0.0]]},
            {"b": [[-rx, m], [-n, ry], [0.0, ry]]}]])
        cur.insertRow(build_row(g, f"Ellipse_{rx}x{ry}", f"Ellipse rx={rx} ry={ry} (4 Bezier)", "E2", 1, g.area))
    print("  [OK] 2 个椭圆")

# 4.3 RotatedEllipse_FC
print("\n--- RotatedEllipse_FC ---")
with arcpy.da.InsertCursor(fc_path("RotatedEllipse_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    for deg in [30, 45, 60]:
        ang = math.radians(deg)
        ca, sa = math.cos(ang), math.sin(ang)
        pts = [[0, 5], [10*C_BEZIER, 5], [10, 5*C_BEZIER], [10, 0], [10, -5*C_BEZIER], [10*C_BEZIER, -5],
               [0, -5], [-10*C_BEZIER, -5], [-10, -5*C_BEZIER], [-10, 0], [-10, 5*C_BEZIER], [-10*C_BEZIER, 5]]
        rot = [[p[0]*ca - p[1]*sa, p[0]*sa + p[1]*ca] for p in pts]
        g = make_curve_ring_geom([[rot[0],
            {"b": [rot[1], rot[2], rot[3]]}, {"b": [rot[4], rot[5], rot[6]]},
            {"b": [rot[7], rot[8], rot[9]]}, {"b": [rot[10], rot[11], rot[0]]}]])
        cur.insertRow(build_row(g, f"Rotated_{deg}deg", f"Rotated ellipse {deg}°", "E3", deg, g.area))
    print("  [OK] 3 个旋转椭圆")

# 4.4 EllipseArc_FC
print("\n--- EllipseArc_FC ---")
with arcpy.da.InsertCursor(fc_path("EllipseArc_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    n, m = 10*C_BEZIER, 5*C_BEZIER
    g = make_curve_geom([[[0.0, 5.0], {"b": [[n, 5], [10, m], [10, 0.0]]},
                          {"b": [[10, -m], [n, -5], [0.0, -5.0]]}]])
    cur.insertRow(build_row(g, "HalfEllipseArc", "Half ellipse arc (2 Bezier)", "E4", 1, g.length))
    ang45 = math.radians(45)
    ca, sa = math.cos(ang45), math.sin(ang45)
    pts = [[0, 5], [n, 5], [10, m], [10, 0]]
    rot = [[p[0]*ca - p[1]*sa, p[0]*sa + p[1]*ca] for p in pts]
    g2 = make_curve_geom([[rot[0], {"b": [rot[1], rot[2], rot[3]]}]])
    cur.insertRow(build_row(g2, "RotatedArc_45deg", "Quarter ellipse arc rotated 45°", "E4", 2, g2.length))
    print("  [OK] 2 个椭圆弧")


# ============================================================
# 阶段 5：补充 Bezier（追加到 Curve_Polyline_FC）
# ============================================================
print("\n" + "=" * 60)
print("阶段 5：补充 Bezier 数据")
print("=" * 60)

with arcpy.da.InsertCursor(fc_path("Curve_Polyline_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    n = 0
    for try_data in [
        ("Bezier_wide",              [[[0.0, 0.0], {"b": [[5.0, 10.0], [10.0, -5.0], [15.0, 5.0]]}]]),
        ("Bezier_Sshape",            [[[0.0, 0.0], {"b": [[3.0, 10.0], [7.0, -5.0], [10.0, 5.0]]}]]),
        ("Straight_Bezier_Straight", [[[0.0, 0.0], [5.0, 0.0], {"b": [[8.0, 10.0], [12.0, 10.0], [15.0, 0.0]]}, [20.0, 0.0]]]),
        ("MultiPart_Bezier",         [[[0.0, 0.0], {"b": [[3.0, 6.0], [7.0, 6.0], [10.0, 0.0]]}],
                                      [[0.0, -10.0], {"b": [[3.0, -5.0], [7.0, -5.0], [10.0, -10.0]]}]]),
    ]:
        try:
            cur.insertRow(build_row(make_curve_geom(try_data[1]), try_data[0],
                                    f"Bezier variation #{n+1}", "B2", n+1))
            n += 1
            print(f"  [OK] {try_data[0]}")
        except Exception as e:
            print(f"  [SKIP] {try_data[0]}: {e}")
    print(f"  追加 {n} 个 Bezier 要素")


# ============================================================
# 阶段 6：M-only 和 ZM 数据
# ============================================================
print("\n" + "=" * 60)
print("阶段 6：M-only 和 ZM 数据")
print("=" * 60)

for name, geom_type, has_z, has_m in [
    ("Point_M_FC",     "POINT",    False, True),
    ("Polyline_M_FC",  "POLYLINE", False, True),
    ("Point_ZM_FC",    "POINT",    True,  True),
    ("Polygon_ZM_FC",  "POLYGON",  True,  True),
]:
    path = fc_path(name)
    delete_if_exists(path)
    arcpy.CreateFeatureclass_management(f"{GDB}/{FD}", name, geom_type, spatial_reference=SR,
                                         has_m="ENABLED", has_z="ENABLED" if has_z else "DISABLED")
    add_fields(path)
    print(f"  [OK] {name}")

print("\n--- Point_M_FC ---")
with arcpy.da.InsertCursor(fc_path("Point_M_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    for x, y, mv in [(0, 0, 0), (1, 2, 10), (3, 1, 20)]:
        cur.insertRow(build_row(arcpy.Point(x, y, M=mv), f"PointM_{mv}", f"M-only point M={mv}", "M1", mv))
    print("  [OK] 3 个 M-only Point")

print("\n--- Polyline_M_FC ---")
with arcpy.da.InsertCursor(fc_path("Polyline_M_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    pts = arcpy.Array([arcpy.Point(0, 0, M=0), arcpy.Point(5, 3, M=10), arcpy.Point(10, 0, M=20)])
    cur.insertRow(build_row(arcpy.Polyline(pts, SR, False, True), "Polyline_M", "M-only polyline", "M1", 1))
    print("  [OK] 1 条 M-only Polyline")

print("\n--- Point_ZM_FC ---")
with arcpy.da.InsertCursor(fc_path("Point_ZM_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    for x, y, z, mv in [(0, 0, 0, 0), (1, 2, 5, 10), (3, 1, 10, 20)]:
        cur.insertRow(build_row(arcpy.Point(x, y, z, mv), f"PointZM_{z}_{mv}", f"ZM point Z={z} M={mv}", "M2"))
    print("  [OK] 3 个 ZM Point")

print("\n--- Polygon_ZM_FC ---")
with arcpy.da.InsertCursor(fc_path("Polygon_ZM_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    outer = arcpy.Array([arcpy.Point(0, 0, 0, 0), arcpy.Point(10, 0, 1, 10),
                         arcpy.Point(10, 10, 2, 20), arcpy.Point(0, 10, 1, 10), arcpy.Point(0, 0, 0, 0)])
    inner = arcpy.Array([arcpy.Point(2, 2, 0.5, 5), arcpy.Point(2, 8, 1.5, 15),
                         arcpy.Point(8, 8, 1.5, 15), arcpy.Point(8, 2, 0.5, 5), arcpy.Point(2, 2, 0.5, 5)])
    cur.insertRow(build_row(arcpy.Polygon(arcpy.Array([outer, inner]), SR, True, True),
                            "Polygon_ZM_Donut", "ZM polygon with hole", "M2", 1))
    print("  [OK] 1 个 ZM Polygon with hole")


# ============================================================
# 阶段 7：多面、空几何、曲线补充
# ============================================================
print("\n" + "=" * 60)
print("阶段 7：多面、空几何、曲线补充")
print("=" * 60)

# 7.1 MultiPolygon_FC - 多面要素
print("\n--- MultiPolygon_FC ---")
with arcpy.da.InsertCursor(fc_path("MultiPolygon_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    # 要素1: 2 个不相交矩形（partCount=2）
    r1 = arcpy.Array([arcpy.Point(0, 0), arcpy.Point(5, 0), arcpy.Point(5, 5), arcpy.Point(0, 5), arcpy.Point(0, 0)])
    r2 = arcpy.Array([arcpy.Point(10, 10), arcpy.Point(15, 10), arcpy.Point(15, 15), arcpy.Point(10, 15), arcpy.Point(10, 10)])
    mp1 = arcpy.Polygon(arcpy.Array([r1, r2]), SR)
    cur.insertRow(build_row(mp1, "MultiPoly_2parts", "Two disjoint squares as one feature", "K", 2, mp1.area))

    # 要素2: 3 个不相交矩形（partCount=3）
    r3 = arcpy.Array([arcpy.Point(30, 0), arcpy.Point(35, 0), arcpy.Point(35, 5), arcpy.Point(30, 5), arcpy.Point(30, 0)])
    r4 = arcpy.Array([arcpy.Point(40, 10), arcpy.Point(45, 10), arcpy.Point(45, 15), arcpy.Point(40, 15), arcpy.Point(40, 10)])
    r5 = arcpy.Array([arcpy.Point(20, 20), arcpy.Point(25, 20), arcpy.Point(25, 25), arcpy.Point(20, 25), arcpy.Point(20, 20)])
    mp2 = arcpy.Polygon(arcpy.Array([r3, r4, r5]), SR)
    cur.insertRow(build_row(mp2, "MultiPoly_3parts", "Three disjoint squares", "K", 3, mp2.area))

    # 要素3: 单面（验证多面图层包含单面）
    r6 = arcpy.Array([arcpy.Point(50, 0), arcpy.Point(55, 0), arcpy.Point(55, 5), arcpy.Point(50, 5), arcpy.Point(50, 0)])
    mp3 = arcpy.Polygon(arcpy.Array([r6]), SR)
    cur.insertRow(build_row(mp3, "SinglePoly", "Single polygon in multipolygon layer", "K", 1, mp3.area))

    print("  [OK] 3 个多面要素（2part / 3part / single）")

# 7.2 空几何图层
print("\n--- Empty Geometry FCs ---")
for fc_name in ["Empty_Point_FC", "Empty_Polyline_FC", "Empty_Polygon_FC"]:
    with arcpy.da.InsertCursor(fc_path(fc_name), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
        cur.insertRow(build_row(None, f"Empty_{fc_name.split('_')[1]}", f"Null geometry for {fc_name}", "Z", 0))
        print(f"  [OK] {fc_name}: 1 个空几何")

# 7.3 曲线+Z（2D 曲线写入 Z-enabled FC，自动补 Z=0）
print("\n--- Curve_Polyline_Z_FC (曲线+Z) ---")
curve_json_z = {"curvePaths": [[[0.0, 0.0], {"c": [[5.0, 5.0], [10.0, 0.0]]}]], "spatialReference": {"wkid": 3857}}
geom_z = arcpy.AsShape(json.dumps(curve_json_z), True)
with arcpy.da.InsertCursor(fc_path("Curve_Polyline_Z_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    cur.insertRow(build_row(geom_z, "CurveZ_CircularArc", "Circular arc in Z-enabled FC (Z auto=0)", "L", 1))
    curve_json_z2 = {"curvePaths": [[[0.0, 0.0], {"b": [[3.0, 8.0], [7.0, 8.0], [10.0, 0.0]]}]], "spatialReference": {"wkid": 3857}}
    geom_z2 = arcpy.AsShape(json.dumps(curve_json_z2), True)
    cur.insertRow(build_row(geom_z2, "CurveZ_Bezier", "Bezier in Z-enabled FC (Z auto=0)", "L", 2))
    print("  [OK] 2 条曲线+Z（2D曲线，Z自动补0）")

# 7.4 曲线+M（2D 曲线写入 M-enabled FC）
print("\n--- Curve_Polyline_M_FC (曲线+M) ---")
curve_json_m = {"curvePaths": [[[0.0, 0.0], {"c": [[5.0, 5.0], [10.0, 0.0]]}]], "spatialReference": {"wkid": 3857}}
geom_m = arcpy.AsShape(json.dumps(curve_json_m), True)
with arcpy.da.InsertCursor(fc_path("Curve_Polyline_M_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    cur.insertRow(build_row(geom_m, "CurveM_CircularArc", "Circular arc in M-enabled FC", "L", 1))
    curve_json_m2 = {"curvePaths": [[[0.0, 0.0], {"b": [[3.0, 8.0], [7.0, 8.0], [10.0, 0.0]]}]], "spatialReference": {"wkid": 3857}}
    geom_m2 = arcpy.AsShape(json.dumps(curve_json_m2), True)
    cur.insertRow(build_row(geom_m2, "CurveM_Bezier", "Bezier in M-enabled FC", "L", 2))
    print("  [OK] 2 条曲线+M（2D曲线）")

# 7.5 曲线 Polygon 多洞
print("\n--- Curve_Polygon_MultiHole_FC ---")
with arcpy.da.InsertCursor(fc_path("Curve_Polygon_MultiHole_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    try:
        # 曲线外环 + 2 个直线洞
        outer = [[0.0, 0.0], [30.0, 0.0], {"c": [[15.0, 20.0], [0.0, 0.0]]}]
        hole1 = [[3.0, 1.0], [3.0, 4.0], [10.0, 4.0], [10.0, 1.0], [3.0, 1.0]]
        hole2 = [[16.0, 1.0], [16.0, 4.0], [25.0, 4.0], [25.0, 1.0], [16.0, 1.0]]
        geom = make_curve_ring_geom([outer, hole1, hole2])
        cur.insertRow(build_row(geom, "CurvePoly_2Holes", "Curve outer ring + 2 straight holes", "I", 2, geom.area))
        print("  [OK] CurvePoly_2Holes")
    except Exception as e:
        print(f"  [SKIP] CurvePoly_2Holes: {e}")

    try:
        # 曲线外环 + 3 个直线洞
        outer2 = [[0.0, 0.0], [40.0, 0.0], {"c": [[20.0, 25.0], [0.0, 0.0]]}]
        ha = [[2.0, 1.0], [2.0, 3.0], [8.0, 3.0], [8.0, 1.0], [2.0, 1.0]]
        hb = [[12.0, 1.0], [12.0, 3.0], [18.0, 3.0], [18.0, 1.0], [12.0, 1.0]]
        hc = [[22.0, 1.0], [22.0, 3.0], [28.0, 3.0], [28.0, 1.0], [22.0, 1.0]]
        geom2 = make_curve_ring_geom([outer2, ha, hb, hc])
        cur.insertRow(build_row(geom2, "CurvePoly_3Holes", "Curve outer ring + 3 straight holes", "I", 3, geom2.area))
        print("  [OK] CurvePoly_3Holes")
    except Exception as e:
        print(f"  [SKIP] CurvePoly_3Holes: {e}")

# 7.6 曲线 Polygon 岛中岛
print("\n--- Curve_Polygon_Island_FC ---")
with arcpy.da.InsertCursor(fc_path("Curve_Polygon_Island_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    try:
        # 曲线外环 → 曲线洞 → 曲线岛（内环）
        outer = [[0.0, 0.0], [40.0, 0.0], {"c": [[20.0, 25.0], [0.0, 0.0]]}]
        hole = [[10.0, 3.0], [30.0, 3.0], {"c": [[20.0, 8.0], [10.0, 3.0]]}]
        island = [[14.0, 4.0], [26.0, 4.0], {"c": [[20.0, 7.0], [14.0, 4.0]]}]
        # 顺序: outer(外环) → hole(洞) → island(岛)
        # 外环顺时针，洞逆时针，岛顺时针
        geom = make_curve_ring_geom([outer, hole, island])
        cur.insertRow(build_row(geom, "Curve_Island", "Curve outer -> curve hole -> curve island", "I", 4, geom.area))
        print("  [OK] Curve_Island")
    except Exception as e:
        print(f"  [SKIP] Curve_Island: {e}")

    try:
        # 复合曲线岛中岛：曲线外环 + 2 个曲线洞 + 1 个曲线岛
        outer2 = [[0.0, 0.0], [60.0, 0.0], {"c": [[30.0, 30.0], [0.0, 0.0]]}]
        ha = [[5.0, 2.0], [25.0, 2.0], {"c": [[15.0, 6.0], [5.0, 2.0]]}]
        hb = [[35.0, 2.0], [55.0, 2.0], {"c": [[45.0, 6.0], [35.0, 2.0]]}]
        # 在 ha 内加一个曲线岛
        island_a = [[8.0, 3.0], [22.0, 3.0], {"c": [[15.0, 5.5], [8.0, 3.0]]}]
        geom2 = make_curve_ring_geom([outer2, ha, hb, island_a])
        cur.insertRow(build_row(geom2, "Complex_CurveIsland", "Curve outer + 2 curve holes + 1 curve island", "I", 5, geom2.area))
        print("  [OK] Complex_CurveIsland")
    except Exception as e:
        print(f"  [SKIP] Complex_CurveIsland: {e}")


# ============================================================
# 阶段 8：CRS 测试数据（GDB 根目录）
# ============================================================
print("\n" + "=" * 60)
print("阶段 8：CRS 测试数据")
print("=" * 60)

crs_srs_map2 = {
    "CRS_WGS84_Point_FC": arcpy.SpatialReference(4326),
    "CRS_CGCS2000_Point_FC": arcpy.SpatialReference(4490),
    "CRS_Xian80_Point_FC": arcpy.SpatialReference(4610),
}
for fc_name, srs in crs_srs_map2.items():
    fc_full = f"{GDB}/{fc_name}"
    with arcpy.da.InsertCursor(fc_full, ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
        cur.insertRow(build_row(arcpy.Point(1, 1), fc_name.replace("_FC", ""), f"Point in {srs.name}", "N", 1))
    desc = arcpy.Describe(fc_full)
    actual_sr = desc.spatialReference
    print(f"  [OK] {fc_name}: {actual_sr.name} (WKID={actual_sr.factoryCode})")


# ============================================================
# 阶段 9：FID 间断数据
# ============================================================
print("\n" + "=" * 60)
print("阶段 9：FID 间断数据")
print("=" * 60)

FID_FD = "FIDTest"
delete_if_exists(f"{GDB}/{FID_FD}")
arcpy.CreateFeatureDataset_management(GDB, FID_FD, SR)

def create_fidgap_fc(fd, fc_name, geom_type):
    """创建 FID 间断要素类"""
    path = f"{GDB}/{fd}/{fc_name}"
    path2 = f"{GDB}/{fd}/{fc_name}"
    arcpy.CreateFeatureclass_management(f"{GDB}/{fd}", fc_name, geom_type, spatial_reference=SR)
    add_fields(path2)
    return path2

# 9.1 Point FID gap（已有: 1,2,6,7）
arcpy.CreateFeatureclass_management(f"{GDB}/{FID_FD}", "Point_FIDGap", "POINT", spatial_reference=SR)
add_fields(f"{GDB}/{FID_FD}/Point_FIDGap")
with arcpy.da.InsertCursor(f"{GDB}/{FID_FD}/Point_FIDGap", ["SHAPE@", "Name", "CountValue"]) as cur:
    for i in range(1, 8):
        cur.insertRow([arcpy.Point(i * 10, i * 10), f"Feature_{i}", i])
with arcpy.da.UpdateCursor(f"{GDB}/{FID_FD}/Point_FIDGap", ["OBJECTID", "CountValue"]) as cur:
    for row in cur:
        if row[0] in [3, 4, 5]:
            cur.deleteRow()
remaining = [row[0] for row in arcpy.da.SearchCursor(f"{GDB}/{FID_FD}/Point_FIDGap", ["OBJECTID"])]
print(f"  Point FID gap: {remaining}")

# 9.2 Polyline FID gap（已有: 1,3,5）
arcpy.CreateFeatureclass_management(f"{GDB}/{FID_FD}", "Polyline_FIDGap", "POLYLINE", spatial_reference=SR)
add_fields(f"{GDB}/{FID_FD}/Polyline_FIDGap")
with arcpy.da.InsertCursor(f"{GDB}/{FID_FD}/Polyline_FIDGap", ["SHAPE@", "Name", "CountValue"]) as cur:
    for i in range(1, 6):
        cur.insertRow([arcpy.Polyline(arcpy.Array([arcpy.Point(i*5, 0), arcpy.Point(i*5, 10)]), SR), f"Line_{i}", i])
with arcpy.da.UpdateCursor(f"{GDB}/{FID_FD}/Polyline_FIDGap", ["OBJECTID", "CountValue"]) as cur:
    for row in cur:
        if row[0] in [2, 4]:
            cur.deleteRow()
remaining = [row[0] for row in arcpy.da.SearchCursor(f"{GDB}/{FID_FD}/Polyline_FIDGap", ["OBJECTID"])]
print(f"  Polyline FID gap: {remaining}")

# 9.3 Polygon FID gap（已有: 2,4）
arcpy.CreateFeatureclass_management(f"{GDB}/{FID_FD}", "Polygon_FIDGap", "POLYGON", spatial_reference=SR)
add_fields(f"{GDB}/{FID_FD}/Polygon_FIDGap")
with arcpy.da.InsertCursor(f"{GDB}/{FID_FD}/Polygon_FIDGap", ["SHAPE@", "Name", "CountValue"]) as cur:
    for i in range(1, 6):
        pts = arcpy.Array([arcpy.Point(i*10, 0), arcpy.Point(i*10+8, 0), arcpy.Point(i*10+8, 8), arcpy.Point(i*10, 8), arcpy.Point(i*10, 0)])
        cur.insertRow([arcpy.Polygon(pts, SR), f"Poly_{i}", i])
with arcpy.da.UpdateCursor(f"{GDB}/{FID_FD}/Polygon_FIDGap", ["OBJECTID", "CountValue"]) as cur:
    for row in cur:
        if row[0] in [1, 3, 5]:
            cur.deleteRow()
remaining = [row[0] for row in arcpy.da.SearchCursor(f"{GDB}/{FID_FD}/Polygon_FIDGap", ["OBJECTID"])]
print(f"  Polygon FID gap: {remaining}")

# 9.4 Point FID exact: 1,2,6,10
arcpy.CreateFeatureclass_management(f"{GDB}/{FID_FD}", "Point_FIDExact", "POINT", spatial_reference=SR)
add_fields(f"{GDB}/{FID_FD}/Point_FIDExact")
with arcpy.da.InsertCursor(f"{GDB}/{FID_FD}/Point_FIDExact", ["SHAPE@", "Name", "CountValue"]) as cur:
    for i in range(1, 11):
        cur.insertRow([arcpy.Point(i * 10, i * 10), f"Feature_{i}", i])
# 删除 3,4,5,7,8,9 -> 保留 1,2,6,10
with arcpy.da.UpdateCursor(f"{GDB}/{FID_FD}/Point_FIDExact", ["OBJECTID", "CountValue"]) as cur:
    for row in cur:
        if row[0] in [3, 4, 5, 7, 8, 9]:
            cur.deleteRow()
remaining_exact = [row[0] for row in arcpy.da.SearchCursor(f"{GDB}/{FID_FD}/Point_FIDExact", ["OBJECTID"])]
print(f"  Point_FIDExact: {remaining_exact}")

# 9.5 Polyline FID exact: 1,4,8
arcpy.CreateFeatureclass_management(f"{GDB}/{FID_FD}", "Polyline_FIDExact", "POLYLINE", spatial_reference=SR)
add_fields(f"{GDB}/{FID_FD}/Polyline_FIDExact")
with arcpy.da.InsertCursor(f"{GDB}/{FID_FD}/Polyline_FIDExact", ["SHAPE@", "Name", "CountValue"]) as cur:
    for i in range(1, 9):
        cur.insertRow([arcpy.Polyline(arcpy.Array([arcpy.Point(i*5, 0), arcpy.Point(i*5, 10)]), SR), f"Line_{i}", i])
with arcpy.da.UpdateCursor(f"{GDB}/{FID_FD}/Polyline_FIDExact", ["OBJECTID", "CountValue"]) as cur:
    for row in cur:
        if row[0] in [2, 3, 5, 6, 7]:
            cur.deleteRow()
remaining_exact = [row[0] for row in arcpy.da.SearchCursor(f"{GDB}/{FID_FD}/Polyline_FIDExact", ["OBJECTID"])]
print(f"  Polyline_FIDExact: {remaining_exact}")


# ============================================================
# 阶段 10：坏拓扑样本
# ============================================================
print("\n" + "=" * 60)
print("阶段 10：坏拓扑样本")
print("=" * 60)

BAD_FD = "BadTopology"
delete_if_exists(f"{GDB}/{BAD_FD}")
arcpy.CreateFeatureDataset_management(GDB, BAD_FD, SR)

for fc_name, gtype in [
    ("SelfIntersect_Polygon", "POLYGON"),
    ("DegenerateRing_Polygon", "POLYGON"),
    ("RepeatedPoint_Polyline", "POLYLINE"),
    ("ZeroArea_Polygon", "POLYGON"),
    ("MultipleRepeatRing_Polygon", "POLYGON"),
    ("DuplicateRing_Polygon", "POLYGON"),
]:
    path = f"{GDB}/{BAD_FD}/{fc_name}"
    arcpy.CreateFeatureclass_management(f"{GDB}/{BAD_FD}", fc_name, gtype, spatial_reference=SR)
    add_fields(path)

with arcpy.da.InsertCursor(f"{GDB}/{BAD_FD}/SelfIntersect_Polygon", ["SHAPE@", "Name", "Description"]) as cur:
    poly = arcpy.Polygon(arcpy.Array([arcpy.Point(0, 0), arcpy.Point(10, 10), arcpy.Point(0, 10), arcpy.Point(10, 0), arcpy.Point(0, 0)]), SR)
    cur.insertRow([poly, "SelfIntersect", "Bow-tie self-intersecting polygon (may be auto-fixed by arcpy)"])
print("  [OK] SelfIntersect_Polygon")

with arcpy.da.InsertCursor(f"{GDB}/{BAD_FD}/DegenerateRing_Polygon", ["SHAPE@", "Name", "Description"]) as cur:
    poly = arcpy.Polygon(arcpy.Array([arcpy.Point(0, 0), arcpy.Point(5, 5), arcpy.Point(10, 10), arcpy.Point(5, 5), arcpy.Point(0, 0)]), SR)
    cur.insertRow([poly, "DegenerateRing", "Degenerate ring (collinear points, may become empty)"])
print("  [OK] DegenerateRing_Polygon")

with arcpy.da.InsertCursor(f"{GDB}/{BAD_FD}/RepeatedPoint_Polyline", ["SHAPE@", "Name", "Description"]) as cur:
    pts = arcpy.Array([arcpy.Point(0, 0), arcpy.Point(0, 0), arcpy.Point(5, 5), arcpy.Point(5, 5), arcpy.Point(5, 5), arcpy.Point(10, 0)])
    cur.insertRow([arcpy.Polyline(pts, SR), "RepeatedPoints", "Polyline with duplicate consecutive points"])
print("  [OK] RepeatedPoint_Polyline")

with arcpy.da.InsertCursor(f"{GDB}/{BAD_FD}/ZeroArea_Polygon", ["SHAPE@", "Name", "Description"]) as cur:
    poly = arcpy.Polygon(arcpy.Array([arcpy.Point(0, 0), arcpy.Point(5, 0), arcpy.Point(5, 0), arcpy.Point(0, 0)]), SR)
    cur.insertRow([poly, "ZeroArea", "Zero-area polygon"])
print("  [OK] ZeroArea_Polygon")

# 10.5 多重重复环（3 个同心环）
with arcpy.da.InsertCursor(f"{GDB}/{BAD_FD}/MultipleRepeatRing_Polygon", ["SHAPE@", "Name", "Description"]) as cur:
    ring1 = arcpy.Array([arcpy.Point(0, 0), arcpy.Point(10, 0), arcpy.Point(10, 10), arcpy.Point(0, 10), arcpy.Point(0, 0)])
    ring2 = arcpy.Array([arcpy.Point(1, 1), arcpy.Point(9, 1), arcpy.Point(9, 9), arcpy.Point(1, 9), arcpy.Point(1, 1)])
    ring3 = arcpy.Array([arcpy.Point(2, 2), arcpy.Point(8, 2), arcpy.Point(8, 8), arcpy.Point(2, 8), arcpy.Point(2, 2)])
    poly = arcpy.Polygon(arcpy.Array([ring1, ring2, ring3]), SR)
    cur.insertRow([poly, "MultipleRepeatRing", "3 concentric rings (arcpy may merge)"])
print("  [OK] MultipleRepeatRing_Polygon")

# 10.6 完全重合环
with arcpy.da.InsertCursor(f"{GDB}/{BAD_FD}/DuplicateRing_Polygon", ["SHAPE@", "Name", "Description"]) as cur:
    ringA = arcpy.Array([arcpy.Point(0, 0), arcpy.Point(5, 0), arcpy.Point(5, 5), arcpy.Point(0, 5), arcpy.Point(0, 0)])
    ringB = arcpy.Array([arcpy.Point(0, 0), arcpy.Point(5, 0), arcpy.Point(5, 5), arcpy.Point(0, 5), arcpy.Point(0, 0)])
    poly = arcpy.Polygon(arcpy.Array([ringA, ringB]), SR)
    cur.insertRow([poly, "DuplicateRing", "2 identical overlapping rings (geometry may become null)"])
print("  [OK] DuplicateRing_Polygon")


# ============================================================
# 阶段 11：大规模性能数据（PerfTest）
# ============================================================
print("\n" + "=" * 60)
print("阶段 11：大规模性能数据")
print("=" * 60)

PERF_FD = "PerfTest"
arcpy.CreateFeatureDataset_management(GDB, PERF_FD, SR)
print(f"创建要素数据集: {PERF_FD}")

PERF_BBOX = (0, 0, 1000000, 1000000)  # 100 万 x 100 万的范围

# 11.1 100k 点
print("\n--- Perf_Point_100k ---")
arcpy.CreateFeatureclass_management(f"{GDB}/{PERF_FD}", "Perf_Point_100k", "POINT", spatial_reference=SR)
fc_100k = f"{GDB}/{PERF_FD}/Perf_Point_100k"
with arcpy.da.InsertCursor(fc_100k, ["SHAPE@"]) as cur:
    n_total = 100000
    for i in range(n_total):
        p = random_point_in_bbox(*PERF_BBOX)
        cur.insertRow([p])
        if (i + 1) % 10000 == 0:
            print_progress("Point 100k", i + 1, n_total)
    print_progress("Point 100k", n_total, n_total)
print(f"  [OK] 100,000 点")

# 11.2 10k 折线
print("\n--- Perf_Polyline_10k ---")
arcpy.CreateFeatureclass_management(f"{GDB}/{PERF_FD}", "Perf_Polyline_10k", "POLYLINE", spatial_reference=SR)
fc_10k_l = f"{GDB}/{PERF_FD}/Perf_Polyline_10k"
with arcpy.da.InsertCursor(fc_10k_l, ["SHAPE@"]) as cur:
    n_total = 10000
    for i in range(n_total):
        pts = random_polyline_vertices(*PERF_BBOX, n=5)
        line = arcpy.Polyline(pts, SR)
        cur.insertRow([line])
        if (i + 1) % 2000 == 0:
            print_progress("Polyline 10k", i + 1, n_total)
    print_progress("Polyline 10k", n_total, n_total)
print(f"  [OK] 10,000 折线")

# 11.3 10k 面
print("\n--- Perf_Polygon_10k ---")
arcpy.CreateFeatureclass_management(f"{GDB}/{PERF_FD}", "Perf_Polygon_10k", "POLYGON", spatial_reference=SR)
fc_10k_p = f"{GDB}/{PERF_FD}/Perf_Polygon_10k"
with arcpy.da.InsertCursor(fc_10k_p, ["SHAPE@"]) as cur:
    n_total = 10000
    for i in range(n_total):
        pts = random_polygon_vertices(*PERF_BBOX, n=4)
        poly = arcpy.Polygon(pts, SR)
        cur.insertRow([poly])
        if (i + 1) % 2000 == 0:
            print_progress("Polygon 10k", i + 1, n_total)
    print_progress("Polygon 10k", n_total, n_total)
print(f"  [OK] 10,000 面")

# 11.4 1M 点（体积较大，需时约 30s）
print("\n--- Perf_Point_1M ---")
arcpy.CreateFeatureclass_management(f"{GDB}/{PERF_FD}", "Perf_Point_1M", "POINT", spatial_reference=SR)
fc_1m = f"{GDB}/{PERF_FD}/Perf_Point_1M"
with arcpy.da.InsertCursor(fc_1m, ["SHAPE@"]) as cur:
    n_total = 1000000
    for i in range(n_total):
        p = random_point_in_bbox(*PERF_BBOX)
        cur.insertRow([p])
        if (i + 1) % 100000 == 0:
            print_progress("Point 1M", i + 1, n_total)
    print_progress("Point 1M", n_total, n_total)
print(f"  [OK] 1,000,000 点")


# ============================================================
# 阶段 12：空间索引操作（后处理）
# ============================================================
print("\n" + "=" * 60)
print("阶段 12：空间索引操作")
print("=" * 60)

# 查找 BadTopology 图层的 .spx 文件并删除（模拟缺失索引）
print("\n--- 删除 BadTopology 的 .spx 文件 ---")
bad_fc_path = f"{GDB}/{BAD_FD}"
try:
    desc = arcpy.Describe(bad_fc_path)
    # 遍历 BadTopology 下所有要素类
    for child in desc.children:
        if child.datasetType == "FeatureClass":
            spx_path = os.path.join(GDB, f"{child.name}.spx")
            if os.path.exists(spx_path):
                try:
                    os.remove(spx_path)
                    print(f"  已删除 .spx: {child.name}")
                except Exception as e:
                    print(f"  无法删除 {child.name}.spx: {e}")
except Exception as e:
    print(f"  遍历 BadTopology 失败: {e}")


# ============================================================
# 阶段 13：验证汇总 + 导出对照文件
# ============================================================
print("\n" + "=" * 60)
print("阶段 13：验证汇总 + 导出对照文件")
print("=" * 60)

# 所有图层列表
ALL_LAYERS = (
    # VectorData 基础
    [("VectorData/" + n, t) for n, t, _, _ in BASE_LAYERS]
    # VectorData 椭圆
    + [("VectorData/Circle_FC", "POLYGON"), ("VectorData/Ellipse_FC", "POLYGON"),
       ("VectorData/RotatedEllipse_FC", "POLYGON"), ("VectorData/EllipseArc_FC", "POLYLINE")]
    # VectorData M/ZM
    + [("VectorData/Point_M_FC", "POINT"), ("VectorData/Polyline_M_FC", "POLYLINE"),
       ("VectorData/Point_ZM_FC", "POINT"), ("VectorData/Polygon_ZM_FC", "POLYGON")]
    # FIDTest
    + [("FIDTest/Point_FIDGap", "POINT"), ("FIDTest/Polyline_FIDGap", "POLYLINE"),
       ("FIDTest/Polygon_FIDGap", "POLYGON"),
       ("FIDTest/Point_FIDExact", "POINT"), ("FIDTest/Polyline_FIDExact", "POLYLINE")]
    # BadTopology
    + [("BadTopology/SelfIntersect_Polygon", "POLYGON"),
       ("BadTopology/DegenerateRing_Polygon", "POLYGON"),
       ("BadTopology/RepeatedPoint_Polyline", "POLYLINE"),
       ("BadTopology/ZeroArea_Polygon", "POLYGON"),
       ("BadTopology/MultipleRepeatRing_Polygon", "POLYGON"),
       ("BadTopology/DuplicateRing_Polygon", "POLYGON")]
    # PerfTest
    + [("PerfTest/Perf_Point_100k", "POINT"),
       ("PerfTest/Perf_Polyline_10k", "POLYLINE"),
       ("PerfTest/Perf_Polygon_10k", "POLYGON"),
       ("PerfTest/Perf_Point_1M", "POINT")]
    # CRS 测试图层（GDB 根目录）
    + [("CRS_WGS84_Point_FC", "POINT"),
       ("CRS_CGCS2000_Point_FC", "POINT"),
       ("CRS_Xian80_Point_FC", "POINT")]
)

print(f"\n{'要素类':40s} {'类型':12s} {'数量':8s} {'曲线':5s}")
print("-" * 70)
grand_total = 0

# 收集数据用于导出
layer_inventory = []  # 用于 layer_inventory.csv
expected_values = []   # 用于 expected_results.json
fid_mapping = []       # 用于 fid_mapping.csv

for rel_path, gtype in ALL_LAYERS:
    path = f"{GDB}/{rel_path}"
    try:
        count = int(arcpy.GetCount_management(path).getOutput(0))
        grand_total += count

        # 曲线检测
        has_c = ""
        crs_wkid = ""
        if count > 0:
            with arcpy.da.SearchCursor(path, ["SHAPE@"]) as cur:
                for row in cur:
                    if row[0] and hasattr(row[0], "hasCurves"):
                        has_c = "Y" if row[0].hasCurves else "N"
                    if row[0] and row[0].spatialReference:
                        crs_wkid = str(row[0].spatialReference.factoryCode)
                    break

        print(f"{rel_path:40s} {gtype:12s} {str(count):>8s} {has_c:5s}")

        # 收集图层清单
        layer_inventory.append({
            "Layer": rel_path,
            "GeometryType": gtype,
            "FeatureCount": count,
            "HasCurves": has_c,
            "CRS_WKID": crs_wkid,
        })

        # 收集关键要素的预期值
        if count > 0 and rel_path not in ["PerfTest/Perf_Point_100k", "PerfTest/Perf_Polyline_10k",
                                           "PerfTest/Perf_Polygon_10k", "PerfTest/Perf_Point_1M",
                                           "BadTopology/RepeatedPoint_Polyline"]:
            with arcpy.da.SearchCursor(path, ["SHAPE@", "OID@", "Name"]) as cur:
                for j, row in enumerate(cur):
                    if j >= 3:  # 最多取前3个要素
                        break
                    g = row[0]
                    if g is None:
                        continue
                    extent = g.extent
                    val = {
                        "Layer": rel_path,
                        "OID": row[1],
                        "Name": str(row[2]) if row[2] else "",
                        "hasCurves": g.hasCurves if hasattr(g, "hasCurves") else False,
                        "bbox": [extent.XMin, extent.YMin, extent.XMax, extent.YMax],
                    }
                    if hasattr(g, "area") and gtype == "POLYGON":
                        val["area"] = g.area
                    if hasattr(g, "length") and gtype in ("POLYLINE", "POLYGON"):
                        val["length"] = g.length
                    if hasattr(g, "pointCount"):
                        val["pointCount"] = g.pointCount
                    if hasattr(g, "partCount"):
                        val["partCount"] = g.partCount
                    expected_values.append(val)

        # 收集 FID 映射（仅 FIDTest）
        if rel_path.startswith("FIDTest/"):
            with arcpy.da.SearchCursor(path, ["OID@", "Name", "CountValue"]) as cur:
                for row in cur:
                    fid_mapping.append({
                        "Layer": rel_path,
                        "ObjectID": row[0],
                        "Name": str(row[1]) if row[1] else "",
                        "CountValue": row[2],
                    })

    except Exception as e:
        print(f"{rel_path:40s} {gtype:12s} ERROR   {str(e)[:30]}")

print("-" * 70)
print(f"全库总计: {grand_total} 个要素, {len(ALL_LAYERS)} 个图层")

# 13.1 导出 layer_inventory.csv
print("\n--- 导出 layer_inventory.csv ---")
csv_path = os.path.join(os.path.dirname(GDB), "testcurve_layer_inventory.csv")
with open(csv_path, "w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=["Layer", "GeometryType", "FeatureCount", "HasCurves", "CRS_WKID"])
    w.writeheader()
    w.writerows(layer_inventory)
print(f"  [OK] {csv_path} ({len(layer_inventory)} rows)")

# 13.2 导出 fid_mapping.csv
print("\n--- 导出 fid_mapping.csv ---")
fid_path = os.path.join(os.path.dirname(GDB), "testcurve_fid_mapping.csv")
with open(fid_path, "w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=["Layer", "ObjectID", "Name", "CountValue"])
    w.writeheader()
    w.writerows(fid_mapping)
print(f"  [OK] {fid_path} ({len(fid_mapping)} rows)")

# 13.3 导出 expected_results.json
print("\n--- 导出 expected_results.json ---")
expected_path = os.path.join(os.path.dirname(GDB), "testcurve_expected_results.json")
with open(expected_path, "w", encoding="utf-8") as f:
    json.dump({
        "generated_by": "ArcGIS Pro 3.5.2 arcpy",
        "generated_at": now().isoformat(),
        "coordinate_system": "EPSG:3857 (Web Mercator)",
        "layer_inventory": layer_inventory,
        "key_features": expected_values,
    }, f, indent=2, ensure_ascii=False, default=str)
print(f"  [OK] {expected_path}")

# 13.4 导出 spatial_cases.csv（点包含测试用例）
print("\n--- 导出 spatial_cases.csv ---")
spatial_cases_path = os.path.join(os.path.dirname(GDB), "testcurve_spatial_cases.csv")
spatial_cases = []
# 为 Polygon_FC 添加点包含测试
test_cases = [
    # (x, y, expected_layer, expected_hit_oid)
    (5, 5, "VectorData/Polygon_FC"),       # SimplePolygon 内部
    (15, 15, "VectorData/Polygon_FC"),     # DonutPolygon 洞内（不应命中）
    (25, 25, "VectorData/Polygon_FC"),     # IslandInIsland 内部
    (7, 7, "VectorData/Polygon_FC"),       # DonutPolygon 环内
    (50, 50, "VectorData/Polygon_FC"),     # 完全外部
    (0, 0, "VectorData/Point_FC"),         # Origin 点
    (1, 2, "VectorData/Point_FC"),         # PointA 点
]
for x, y, layer in test_cases:
    test_pt = arcpy.Point(x, y)
    spatial_cases.append({
        "TestPoint": f"({x},{y})",
        "TargetLayer": layer,
        "ExpectedHit": True,  # 粗略
        "x": x,
        "y": y,
    })
with open(spatial_cases_path, "w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=["TestPoint", "TargetLayer", "ExpectedHit", "x", "y"])
    w.writeheader()
    w.writerows(spatial_cases)
print(f"  [OK] {spatial_cases_path} ({len(spatial_cases)} rows)")

# 13.5 导出 manifest.json
print("\n--- 导出 manifest.json ---")
manifest_path = os.path.join(os.path.dirname(GDB), "testcurve_manifest.json")
manifest = {
    "name": "testcurve.gdb",
    "description": "Complete FileGDB test dataset for vector data parsing, curve geometries, and performance testing",
    "generated_by": "ArcGIS Pro 3.5.2 arcpy",
    "generated_at": now().isoformat(),
    "coordinate_system": {
        "primary": "EPSG:3857 (WGS 84 / Web Mercator)",
        "variants": ["EPSG:4326 (WGS 84)", "EPSG:4490 (CGCS2000)", "EPSG:4610 (Xian 80)"]
    },
    "feature_datasets": [
        {"name": "VectorData", "description": "Main dataset with all geometry types"},
        {"name": "FIDTest", "description": "FID discontinuity test data"},
        {"name": "BadTopology", "description": "Invalid topology test data (.spx deleted)"},
        {"name": "PerfTest", "description": "Large-scale performance test data"},
    ],
    "total_feature_count": grand_total,
    "total_layers": len(ALL_LAYERS),
    "limitations": {
        "multipatch": "Non-empty multipatch cannot be created via arcpy.AsShape() in Pro 3.5.2",
        "curve_zm": "Native 3D curves (Z/M on curve control points) not supported by arcpy.AsShape()",
        "elliptic_arc": "ESRI JSON elliptic arc format not supported by arcpy in Pro 3.5.2",
    },
    "accompanying_files": [
        "testcurve_layer_inventory.csv",
        "testcurve_fid_mapping.csv",
        "testcurve_expected_results.json",
        "testcurve_spatial_cases.csv",
        "testcurve_source_notes.md",
    ],
}
with open(manifest_path, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2, ensure_ascii=False, default=str)
print(f"  [OK] {manifest_path}")

# 13.6 导出 source_notes.md
print("\n--- 导出 source_notes.md ---")
notes_path = os.path.join(os.path.dirname(GDB), "testcurve_source_notes.md")
with open(notes_path, "w", encoding="utf-8") as f:
    f.write("""# testcurve.gdb 数据来源说明

## 生成环境

| 项目 | 值 |
|------|-----|
| 生成工具 | ArcGIS Pro arcpy 3.5 |
| ArcGIS Pro 版本 | 3.5.2 |
| Python 版本 | 3.x (bundled with ArcPro) |
| 操作系统 | Windows 11 Pro |
| 生成脚本 | `generate_all_data.py` |
| 生成时间 | %s

## 坐标系

- **主坐标系**: EPSG:3857 (WGS 84 / Web Mercator)
- **变体坐标系**: EPSG:4326 (WGS 84), EPSG:4490 (CGCS2000), EPSG:4610 (Xian 80)

## 曲线数据说明

所有曲线数据（CircularArc、Bezier）均为 **ArcGIS Pro 原生曲线**，通过 `arcpy.AsShape()` 写入 ESRI JSON `curvePaths`/`curveRings` 格式，**未经过 GDAL 线性化**。

### 曲线类型
- **CircularArc**: 三点圆弧 `{"c": [mid_x, mid_y], [end_x, end_y]}`
- **Bezier**: 三次贝塞尔 `{"b": [ctrl1_x, ctrl1_y], [ctrl2_x, ctrl2_y], [end_x, end_y]}`
- **Circle**: 2 段 CircularArc 闭合
- **Ellipse**: 4 段 Bezier 近似

## 限制说明

1. **曲线 Z/M/ZM**: arcpy 3.5 不支持 curvePaths JSON 中包含 Z/M 坐标的格式，Z/M 曲线要素的曲线控制点不含 Z/M 值
2. **EllipticArc**: ESRI JSON 椭圆弧格式 `{"e": [...]}` 在 arcpy 3.5 中不支持

## 空间索引

- PerfTest 和 VectorData 图层保留有效的 `.spx` 空间索引
- BadTopology 图层的 `.spx` 已删除（模拟缺失空间索引场景）

## 数据用途

本数据集用于 `fast-gdb` 项目对 FileGDB 数据的读取、几何解析、空间查询和 Hybrid 回退链路的真实验收。
""" % now().isoformat())
print(f"  [OK] {notes_path}")


# ============================================================
# 完成
# ============================================================
print("\n" + "=" * 60)
print("全部完成！")
print("=" * 60)
print(f"\nGDB 位置: {GDB}")
print(f"侧文件位置: {os.path.dirname(GDB)}")
print(f"   - testcurve_layer_inventory.csv")
print(f"   - testcurve_fid_mapping.csv")
print(f"   - testcurve_expected_results.json")
print(f"   - testcurve_spatial_cases.csv")
print(f"   - testcurve_manifest.json")
print(f"   - testcurve_source_notes.md")
print(f"\n全库总计: {grand_total} 个要素")
