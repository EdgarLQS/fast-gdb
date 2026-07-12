"""
generate_all_data.py
====================
在 testcurve.gdb 中创建完整的矢量测试数据集，覆盖 File GDB 主要矢量数据类型。
包含：基础几何、曲线、椭圆、M/ZM、FID 间断、坏拓扑。

执行方式：
    "D:\\software\\install\\ArcPro301\\bin\\Python\\envs\\arcgispro-py3\\python.exe" generate_all_data.py

环境要求：ArcGIS Pro 3.0+（含 arcpy）
输出路径：testcurve.gdb/VectorData, FIDTest, BadTopology
"""

import arcpy
import json
import uuid
import datetime
import math

# ============================================================
# 配置
# ============================================================
GDB = r"C:\Users\EdgarLQS\Documents\ArcGIS\Projects\MyProject\testcurve.gdb"
FD = "VectorData"
SR = arcpy.SpatialReference(3857)

arcpy.env.workspace = GDB
arcpy.env.overwriteOutput = True

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

C_BEZIER = 0.5522847498307935  # 贝塞尔圆弧近似常数


# ============================================================
# 阶段 1：创建 VectorData 要素数据集 + 基础 10 层
# ============================================================
print("=" * 60)
print("阶段 1：创建结构")
print("=" * 60)

delete_if_exists(f"{GDB}/{FD}")
arcpy.CreateFeatureDataset_management(GDB, FD, SR)
print(f"创建要素数据集: {FD}")

BASE_LAYERS = [
    ("Point_FC",          "POINT",      False, False),
    ("MultiPoint_FC",     "MULTIPOINT", False, False),
    ("Polyline_FC",       "POLYLINE",   False, False),
    ("Polygon_FC",        "POLYGON",    False, False),
    ("Point_Z_FC",        "POINT",      True,  False),
    ("Polyline_ZM_FC",    "POLYLINE",   True,  True),
    ("Polygon_Z_FC",      "POLYGON",    True,  False),
    ("Curve_Polyline_FC", "POLYLINE",   False, False),
    ("Curve_Polygon_FC",  "POLYGON",    False, False),
    ("Multipatch_FC",     "MULTIPATCH", True,  False),
]

for fc_name, geom_type, has_z, has_m in BASE_LAYERS:
    path = fc_path(fc_name)
    arcpy.CreateFeatureclass_management(
        f"{GDB}/{FD}", fc_name, geom_type, spatial_reference=SR,
        has_m="ENABLED" if has_m else "DISABLED",
        has_z="ENABLED" if has_z else "DISABLED",
    )
    add_fields(path)
    print(f"  [OK] {fc_name:20s}  {geom_type:12s}  Z={int(has_z)}  M={int(has_m)}")

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
print("\n--- Curve_Polyline_FC （4 条原生曲线）---")
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

    # EllipticArc（Pro 3.0.1 不支持，跳过不报错）
    try:
        geom = make_curve_geom([[[0.0, 0.0], {"e": [10.0, 0.0, 5.0, 0.0, 0.0, 0.5, False, False]}, [10.0, 5.0], {"e": [0.0, 5.0, 5.0, 5.0, 0.0, 0.5, False, False]}, [0.0, 0.0]]])
        cur.insertRow(build_row(geom, "EllipticArc", "Elliptic arc (Pro 3.0.1 不支持此格式)", "H", 5))
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

# 3.3 MultiPatch（尝试创建，已知 Pro 3.0.1 不支持）
print("\n--- Multipatch_FC ---")
with arcpy.da.InsertCursor(fc_path("Multipatch_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    try:
        mp_json = {
            "rings": [
                [[0, 0, 0], [10, 0, 0], [10, 10, 0], [0, 10, 0], [0, 0, 0]],
                [[0, 0, 5], [10, 0, 5], [10, 10, 5], [0, 10, 5], [0, 0, 5]],
                [[0, 0, 0], [10, 0, 0], [10, 0, 5], [0, 0, 5], [0, 0, 0]],
                [[0, 10, 0], [10, 10, 0], [10, 10, 5], [0, 10, 5], [0, 10, 0]],
                [[0, 0, 0], [0, 10, 0], [0, 10, 5], [0, 0, 5], [0, 0, 0]],
                [[10, 0, 0], [10, 10, 0], [10, 10, 5], [10, 0, 5], [10, 0, 0]],
            ],
            "hasZ": True,
            "spatialReference": {"wkid": 3857},
        }
        mp = arcpy.AsShape(json.dumps(mp_json), True)
        cur.insertRow(build_row(mp, "SimpleBox", "3D box multipatch (6 faces)", "J", 1, 600.0))
        print("  [OK] MultiPatch")
    except Exception as e:
        print(f"  [SKIP] MultiPatch (Pro 3.0.1 arcpy 限制): {e}")


# ============================================================
# 阶段 4：Ellipse 数据（完整圆 / 椭圆 / 旋转椭圆 / 椭圆弧）
# ============================================================
print("\n" + "=" * 60)
print("阶段 4：Ellipse 数据")
print("=" * 60)

for name, geom_type in [
    ("Circle_FC", "POLYGON"), ("Ellipse_FC", "POLYGON"),
    ("RotatedEllipse_FC", "POLYGON"), ("EllipseArc_FC", "POLYLINE"),
]:
    path = fc_path(name)
    delete_if_exists(path)
    arcpy.CreateFeatureclass_management(f"{GDB}/{FD}", name, geom_type, spatial_reference=SR)
    add_fields(path)
    print(f"  [OK] {name}")

c = C_BEZIER

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
        n, m = rx * c, ry * c
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
        pts = [[0, 5], [10*c, 5], [10, 5*c], [10, 0], [10, -5*c], [10*c, -5],
               [0, -5], [-10*c, -5], [-10, -5*c], [-10, 0], [-10, 5*c], [-10*c, 5]]
        rot = [[p[0]*ca - p[1]*sa, p[0]*sa + p[1]*ca] for p in pts]
        g = make_curve_ring_geom([[rot[0],
            {"b": [rot[1], rot[2], rot[3]]}, {"b": [rot[4], rot[5], rot[6]]},
            {"b": [rot[7], rot[8], rot[9]]}, {"b": [rot[10], rot[11], rot[0]]}]])
        cur.insertRow(build_row(g, f"Rotated_{deg}deg", f"Rotated ellipse {deg}°", "E3", deg, g.area))
    print("  [OK] 3 个旋转椭圆")

# 4.4 EllipseArc_FC
print("\n--- EllipseArc_FC ---")
with arcpy.da.InsertCursor(fc_path("EllipseArc_FC"), ["SHAPE@", "Name", "Description", "Category", "CountValue", "Area_Size", "Ratio", "IsActive", "CreateDate", "UniqueID"]) as cur:
    n, m = 10*c, 5*c
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
# 阶段 7：FID 间断数据
# ============================================================
print("\n" + "=" * 60)
print("阶段 7：FID 间断数据")
print("=" * 60)

FID_FD = "FIDTest"
delete_if_exists(f"{GDB}/{FID_FD}")
arcpy.CreateFeatureDataset_management(GDB, FID_FD, SR)

# Point FID gap
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
print(f"  Point FID: {remaining}")

# Polyline FID gap
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
print(f"  Polyline FID: {remaining}")

# Polygon FID gap
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
print(f"  Polygon FID: {remaining}")


# ============================================================
# 阶段 8：坏拓扑样本
# ============================================================
print("\n" + "=" * 60)
print("阶段 8：坏拓扑样本")
print("=" * 60)

BAD_FD = "BadTopology"
delete_if_exists(f"{GDB}/{BAD_FD}")
arcpy.CreateFeatureDataset_management(GDB, BAD_FD, SR)

for fc_name, gtype in [
    ("SelfIntersect_Polygon", "POLYGON"),
    ("DegenerateRing_Polygon", "POLYGON"),
    ("RepeatedPoint_Polyline", "POLYLINE"),
    ("ZeroArea_Polygon", "POLYGON"),
]:
    path = f"{GDB}/{BAD_FD}/{fc_name}"
    arcpy.CreateFeatureclass_management(f"{GDB}/{BAD_FD}", fc_name, gtype, spatial_reference=SR)
    add_fields(path)

with arcpy.da.InsertCursor(f"{GDB}/{BAD_FD}/SelfIntersect_Polygon", ["SHAPE@", "Name", "Description"]) as cur:
    poly = arcpy.Polygon(arcpy.Array([arcpy.Point(0, 0), arcpy.Point(10, 10), arcpy.Point(0, 10), arcpy.Point(10, 0), arcpy.Point(0, 0)]), SR)
    cur.insertRow([poly, "SelfIntersect", "Bow-tie self-intersecting polygon"])
print("  [OK] SelfIntersect_Polygon")

with arcpy.da.InsertCursor(f"{GDB}/{BAD_FD}/DegenerateRing_Polygon", ["SHAPE@", "Name", "Description"]) as cur:
    poly = arcpy.Polygon(arcpy.Array([arcpy.Point(0, 0), arcpy.Point(5, 5), arcpy.Point(10, 10), arcpy.Point(5, 5), arcpy.Point(0, 0)]), SR)
    cur.insertRow([poly, "DegenerateRing", "Degenerate ring (collinear points)"])
print("  [OK] DegenerateRing_Polygon")

with arcpy.da.InsertCursor(f"{GDB}/{BAD_FD}/RepeatedPoint_Polyline", ["SHAPE@", "Name", "Description"]) as cur:
    pts = arcpy.Array([arcpy.Point(0, 0), arcpy.Point(0, 0), arcpy.Point(5, 5), arcpy.Point(5, 5), arcpy.Point(5, 5), arcpy.Point(10, 0)])
    cur.insertRow([arcpy.Polyline(pts, SR), "RepeatedPoints", "Polyline with duplicate consecutive points"])
print("  [OK] RepeatedPoint_Polyline")

with arcpy.da.InsertCursor(f"{GDB}/{BAD_FD}/ZeroArea_Polygon", ["SHAPE@", "Name", "Description"]) as cur:
    poly = arcpy.Polygon(arcpy.Array([arcpy.Point(0, 0), arcpy.Point(5, 0), arcpy.Point(5, 0), arcpy.Point(0, 0)]), SR)
    cur.insertRow([poly, "ZeroArea", "Zero-area polygon"])
print("  [OK] ZeroArea_Polygon")


# ============================================================
# 阶段 9：验证汇总
# ============================================================
print("\n" + "=" * 60)
print("阶段 9：验证汇总")
print("=" * 60)

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
       ("FIDTest/Polygon_FIDGap", "POLYGON")]
    # BadTopology
    + [("BadTopology/SelfIntersect_Polygon", "POLYGON"),
       ("BadTopology/DegenerateRing_Polygon", "POLYGON"),
       ("BadTopology/RepeatedPoint_Polyline", "POLYLINE"),
       ("BadTopology/ZeroArea_Polygon", "POLYGON")]
)

print(f"\n{'要素类':34s} {'类型':12s} {'数量':6s} {'曲线':5s}")
print("-" * 60)
grand_total = 0
for rel_path, gtype in ALL_LAYERS:
    path = f"{GDB}/{rel_path}"
    try:
        count = int(arcpy.GetCount_management(path).getOutput(0))
        grand_total += count
        has_c = ""
        if count > 0:
            with arcpy.da.SearchCursor(path, ["SHAPE@"]) as cur:
                for row in cur:
                    if row[0] and hasattr(row[0], "hasCurves"):
                        has_c = "Y" if row[0].hasCurves else "N"
                    break
        print(f"{rel_path:34s} {gtype:12s} {str(count):6s} {has_c:5s}")
    except Exception as e:
        print(f"{rel_path:34s} {gtype:12s} ERROR   {str(e)[:25]}")

print("-" * 60)
print(f"全库总计: {grand_total} 个要素, {len(ALL_LAYERS)} 个图层")
print("\n" + "=" * 60)
print("全部完成！")
print("=" * 60)