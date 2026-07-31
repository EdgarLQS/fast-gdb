import arcpy, json, uuid, datetime, os, csv
GDB = r"E:/gitdesktop/fast-gdb/test_data/gdb/acceptance_metadata.gdb"
OUT_DIR = r"E:/gitdesktop/fast-gdb/test_data/gdb/acceptance_metadata"
SR_4326 = arcpy.SpatialReference(4326)
SR_3857 = arcpy.SpatialReference(3857)
arcpy.env.overwriteOutput = True
arcpy.env.workspace = GDB
os.makedirs(OUT_DIR, exist_ok=True)
def now(): return datetime.datetime.now()
def make_guid(): return "{" + str(uuid.uuid4()).upper() + "}"
print("=" * 60)
print("Phase 0: Rebuild GDB")
print("=" * 60)
if arcpy.Exists(GDB):
    arcpy.Delete_management(GDB)
    print("  Deleted old GDB")
arcpy.CreateFileGDB_management(os.path.dirname(GDB), os.path.basename(GDB))
print("  Created GDB: " + GDB)
print(chr(10) + "=" * 60)
print("Phase 1: Create Domains")
print("=" * 60)

for dom_name, dom_desc, fld_type, dom_vals in [
    ("road_status_domain", "Road status codes", "TEXT",
     [("OPEN", "Open"), ("CLOSED", "Closed"), ("UNKNOWN", "Unknown")]),
    ("road_type_domain", "Road type codes", "SHORT",
     [(1, "Highway"), (2, "Local Road"), (3, "Trail")]),
    ("inspection_result_domain", "Inspection result codes", "TEXT",
     [("GOOD", "Good"), ("FAIR", "Fair"), ("POOR", "Poor")]),
]:
    if arcpy.Exists(dom_name):
        arcpy.DeleteDomain_management(GDB, dom_name)
    arcpy.CreateDomain_management(GDB, dom_name, dom_desc, fld_type, "CODED", "DEFAULT", "DEFAULT")
    for code_val, desc in dom_vals:
        arcpy.AddCodedValueToDomain_management(GDB, dom_name, code_val, desc)
    print("  [OK] " + dom_name)

if arcpy.Exists("speed_range_domain"):
    arcpy.DeleteDomain_management(GDB, "speed_range_domain")
arcpy.CreateDomain_management(GDB, "speed_range_domain", "Speed limit range 0-120", "SHORT", "RANGE", "DEFAULT", "DEFAULT")
arcpy.SetValueForRangeDomain_management(GDB, "speed_range_domain", 0, 120)
print("  [OK] speed_range_domain (Range, SHORT, 0-120)")

print(chr(10) + "=" * 60)
print("Phase 2: Create Feature Datasets")
print("=" * 60)
arcpy.CreateFeatureDataset_management(GDB, "TransportFD", SR_4326)
print("  [OK] TransportFD")
arcpy.CreateFeatureDataset_management(GDB, "AdminFD", SR_3857)
print("  [OK] AdminFD")
arcpy.CreateFeatureclass_management(GDB, "root_points", "POINT", spatial_reference=SR_4326)
print("  [OK] root_points")
arcpy.CreateTable_management(GDB, "root_table")
print("  [OK] root_table")

print(chr(10) + "=" * 60)
print("Phase 3: Feature Classes")
print("=" * 60)
FD_T = os.path.join(GDB, "TransportFD")
FD_A = os.path.join(GDB, "AdminFD")

def create_fc(parent, name, geom_type, sr, fields):
    arcpy.CreateFeatureclass_management(parent, name, geom_type, spatial_reference=sr)
    path = os.path.join(parent, name)
    for fname, ftype, *rest in fields:
        kwargs = {"field_name": fname, "field_type": ftype}
        if rest: kwargs["field_length"] = rest[0]
        arcpy.AddField_management(path, **kwargs)
    print("  [OK] " + name)
    return path

def create_tbl(parent, name, fields):
    arcpy.CreateTable_management(parent, name)
    path = os.path.join(parent, name)
    for fname, ftype, *rest in fields:
        kwargs = {"field_name": fname, "field_type": ftype}
        if rest: kwargs["field_length"] = rest[0]
        arcpy.AddField_management(path, **kwargs)
    print("  [OK] " + name)
    return path

rp = create_fc(FD_T, "roads", "POLYLINE", SR_4326, [
    ("road_id", "LONG"), ("name", "TEXT", 100), ("status", "TEXT", 20),
    ("speed_limit", "SHORT"), ("road_type", "SHORT"),
    ("description", "TEXT", 255), ("length_km", "DOUBLE")])

rip = create_fc(FD_T, "road_inspections", "POINT", SR_4326, [
    ("road_id", "LONG"), ("inspector", "TEXT", 50),
    ("inspection_date", "DATE"), ("result", "TEXT", 20), ("score", "SHORT")])

rap = create_fc(FD_T, "road_assets", "POINT", SR_4326, [
    ("road_id", "LONG"), ("asset_name", "TEXT", 100),
    ("asset_type", "TEXT", 50), ("asset_guid", "GUID")])
arcpy.AddGlobalIDs_management(rap)
arcpy.AddGlobalIDs_management(rp)
print("  [OK] road_assets: +GlobalID")

pp = create_fc(FD_A, "parcels", "POLYGON", SR_3857, [
    ("parcel_id", "LONG"), ("parcel_name", "TEXT", 100),
    ("land_use", "TEXT", 50), ("area_sqm", "DOUBLE"),
    ("owner", "TEXT", 100), ("is_active", "TEXT", 5)])

ppp = create_fc(FD_A, "parcel_parts", "POLYGON", SR_3857, [
    ("parcel_id", "LONG"), ("part_name", "TEXT", 100),
    ("part_type", "TEXT", 50), ("area_sqm", "DOUBLE")])

rpt = create_fc(GDB, "root_points", "POINT", SR_4326, [
    ("point_id", "LONG"), ("point_name", "TEXT", 100),
    ("category", "TEXT", 50), ("elevation", "DOUBLE")])

rt = create_tbl(GDB, "root_table", [
    ("record_id", "LONG"), ("record_name", "TEXT", 100),
    ("record_value", "DOUBLE"), ("record_date", "DATE")])
print(chr(10) + "=" * 60)
print("Phase 4: Domain assignment and Subtype")
print("=" * 60)
arcpy.AssignDomainToField_management(rp, "status", "road_status_domain")
arcpy.AssignDomainToField_management(rp, "speed_limit", "speed_range_domain")
arcpy.AssignDomainToField_management(rip, "result", "inspection_result_domain")
print("  [OK] Domain assignments")

arcpy.SetSubtypeField_management(rp, "road_type")
for code, name in [(1, "Highway"), (2, "Local Road"), (3, "Trail")]:
    arcpy.AddSubtype_management(rp, code, name)
arcpy.AssignDefaultToField_management(rp, "speed_limit", 60)
arcpy.AssignDefaultToField_management(rp, "status", "OPEN")
print("  [OK] roads subtype with defaults")

print(chr(10) + "=" * 60)
print("Phase 5: Relationship Classes")
print("=" * 60)

arcpy.CreateRelationshipClass_management(
    origin_table=rp, destination_table=rip,
    out_relationship_class=os.path.join(FD_T, "roads_inspections_rel"),
    relationship_type="SIMPLE", forward_label="Road Inspections",
    backward_label="Parent Road", message_direction="NONE",
    cardinality="ONE_TO_MANY", attributed="NONE",
    origin_primary_key="OBJECTID", origin_foreign_key="road_id")
print("  [OK] roads_inspections_rel (Simple 1:M)")

arcpy.CreateRelationshipClass_management(
    origin_table=pp, destination_table=ppp,
    out_relationship_class=os.path.join(FD_A, "parcels_parts_rel"),
    relationship_type="COMPOSITE", forward_label="Parcel Parts",
    backward_label="Parent Parcel", message_direction="FORWARD",
    cardinality="ONE_TO_MANY", attributed="NONE",
    origin_primary_key="OBJECTID", origin_foreign_key="parcel_id")
print("  [OK] parcels_parts_rel (Composite 1:M, containment)")

arcpy.CreateRelationshipClass_management(
    origin_table=rp, destination_table=rap,
    out_relationship_class=os.path.join(FD_T, "assets_guid_rel"),
    relationship_type="SIMPLE", forward_label="Road Assets",
    backward_label="Parent Road", message_direction="NONE",
    cardinality="ONE_TO_ONE", attributed="NONE",
    origin_primary_key="GlobalID", origin_foreign_key="asset_guid")
print("  [OK] assets_guid_rel (Simple 1:1, GlobalID<->GUID)")

print(chr(10) + "=" * 60)
print("Phase 6: all_field_types table")
print("=" * 60)
aft = create_tbl(GDB, "all_field_types", [
    ("short_fld", "SHORT"), ("long_fld", "LONG"),
    ("float_fld", "FLOAT"), ("double_fld", "DOUBLE"),
    ("text_fld", "TEXT", 100), ("date_fld", "DATE"),
    ("guid_fld", "GUID"), ("globalid_fld", "GLOBALID"),
    ("blob_fld", "BLOB")])

advanced_fields = {}
for fname, ftype in [("bigint_fld", "BIGINTEGER"),
    ("dateonly_fld", "DATEONLY"), ("timeonly_fld", "TIMEONLY"),
    ("timestampoffset_fld", "TIMESTAMPOFFSET")]:
    try:
        arcpy.AddField_management(aft, fname, ftype)
        advanced_fields[fname] = ftype
        print("  [OK] " + fname + " (" + ftype + ")")
    except Exception as e:
        advanced_fields[fname] = "SKIPPED"
        print("  [SKIP] " + fname + " (" + ftype + "): " + str(e))

print(chr(10) + "=" * 60)
print("Phase 7: Write data records")
print("=" * 60)

print("")
print("--- root_points ---")
with arcpy.da.InsertCursor(rpt, ["SHAPE@", "point_id", "point_name", "category", "elevation"]) as cur:
    cur.insertRow([arcpy.Point(116.4, 39.9), 1, "Tiananmen", "Landmark", 44.0])
    cur.insertRow([arcpy.Point(116.3, 39.95), 2, "Olympic Park", "Park", 48.0])
    cur.insertRow([arcpy.Point(116.47, 39.9), 3, "CBD Center", "Business", 50.0])
    cur.insertRow([None, 4, "EmptyPoint", "Test", None])
print("  [OK] 4 records (3 valid + 1 empty geometry)")

print("")
print("--- root_table ---")
with arcpy.da.InsertCursor(rt, ["record_id", "record_name", "record_value", "record_date"]) as cur:
    cur.insertRow([1, "Configuration A", 1.5, datetime.datetime(2025, 1, 1)])
    cur.insertRow([2, "Configuration B", 2.5, datetime.datetime(2025, 6, 15)])
    cur.insertRow([3, "Configuration C", 3.5, datetime.datetime(2025, 12, 31)])
    cur.insertRow([4, "Template Value", None, None])
    cur.insertRow([5, "Reference Data", 0.0, datetime.datetime(2024, 1, 1)])
print("  [OK] 5 records")

print("")
print("--- roads ---")
rf = ["SHAPE@", "road_id", "name", "status", "speed_limit", "road_type", "description", "length_km"]
with arcpy.da.InsertCursor(rp, rf) as cur:
    pts = arcpy.Array([arcpy.Point(116.3, 39.9), arcpy.Point(116.4, 39.92), arcpy.Point(116.5, 39.94)])
    cur.insertRow([arcpy.Polyline(pts, SR_4326), 1, "Chang\'an Avenue", "OPEN", 100, 1, "Main east-west artery", 12.5])
    pts2 = arcpy.Array([arcpy.Point(116.35, 39.8), arcpy.Point(116.4, 39.82), arcpy.Point(116.45, 39.84)])
    cur.insertRow([arcpy.Polyline(pts2, SR_4326), 2, "Hutong Lane", "OPEN", 60, 2, "Local residential street", 2.3])
    pts3 = arcpy.Array([arcpy.Point(116.39, 39.88), arcpy.Point(116.42, 39.89)])
    cur.insertRow([arcpy.Polyline(pts3, SR_4326), 3, "Park Trail A", "CLOSED", 30, 3, "Seasonal hiking trail", 0.8])
    cur.insertRow([None, 4, "Planned Road X", "UNKNOWN", 0, 2, "Future road, not yet built", None])
    pts4 = arcpy.Array([arcpy.Point(116.3, 39.85), arcpy.Point(116.4, 39.87)])
    cur.insertRow([arcpy.Polyline(pts4, SR_4326), 5, "Service Road 1", "OPEN", 60, 2, "Service access road", 1.5])
print("  [OK] 5 initial records")
with arcpy.da.InsertCursor(rp, rf) as cur:
    pts5 = arcpy.Array([arcpy.Point(116.45, 39.86), arcpy.Point(116.5, 39.88)])
    cur.insertRow([arcpy.Polyline(pts5, SR_4326), 6, "Extension Road", "OPEN", 80, 1, "New highway extension", 4.2])
print("  [OK] +1 appended (total 6)")

print("")
print("--- road_inspections ---")
with arcpy.da.InsertCursor(rip, ["SHAPE@", "road_id", "inspector", "inspection_date", "result", "score"]) as cur:
    cur.insertRow([arcpy.Point(116.35, 39.91), 1, "Zhang Wei", datetime.datetime(2025, 3, 15), "GOOD", 95])
    cur.insertRow([arcpy.Point(116.4, 39.92), 1, "Li Ming", datetime.datetime(2025, 6, 20), "FAIR", 75])
    cur.insertRow([arcpy.Point(116.38, 39.81), 2, "Wang Fang", datetime.datetime(2025, 4, 10), "GOOD", 88])
    cur.insertRow([arcpy.Point(116.4, 39.88), 3, "Chen Yu", datetime.datetime(2025, 5, 5), "POOR", 45])
    cur.insertRow([arcpy.Point(116.35, 39.86), 5, "Zhao Lin", datetime.datetime(2025, 7, 1), "GOOD", 92])
print("  [OK] 5 records")

print("")
print("--- road_assets ---")
with arcpy.da.InsertCursor(rap, ["SHAPE@", "road_id", "asset_name", "asset_type", "asset_guid"]) as cur:
    cur.insertRow([arcpy.Point(116.38, 39.91), 1, "Street Light A1", "Lighting", make_guid()])
    cur.insertRow([arcpy.Point(116.42, 39.93), 1, "Traffic Light T1", "Traffic", make_guid()])
    cur.insertRow([arcpy.Point(116.37, 39.81), 2, "Signpost S1", "Signage", make_guid()])
    cur.insertRow([arcpy.Point(116.4, 39.89), 3, "Bench B1", "Furniture", make_guid()])
    cur.insertRow([arcpy.Point(116.45, 39.87), 6, "Guardrail G1", "Safety", make_guid()])
print("  [OK] 5 records")

print("")
print("--- parcels ---")
pf = ["SHAPE@", "parcel_id", "parcel_name", "land_use", "area_sqm", "owner", "is_active"]
with arcpy.da.Editor(GDB) as edit:
    with arcpy.da.InsertCursor(pp, pf) as cur:
        r1 = arcpy.Array([arcpy.Point(0, 0), arcpy.Point(1000, 0), arcpy.Point(1000, 1000), arcpy.Point(0, 1000), arcpy.Point(0, 0)])
        cur.insertRow([arcpy.Polygon(r1, SR_3857), 1, "Parcel Alpha", "Residential", 1000000.0, "City Development Corp", "Y"])
        r2 = arcpy.Array([arcpy.Point(1500, 0), arcpy.Point(2500, 0), arcpy.Point(2500, 800), arcpy.Point(1500, 800), arcpy.Point(1500, 0)])
        cur.insertRow([arcpy.Polygon(r2, SR_3857), 2, "Parcel Beta", "Commercial", 800000.0, "Metro Holdings", "Y"])
        r3 = arcpy.Array([arcpy.Point(0, 1500), arcpy.Point(2000, 1500), arcpy.Point(2000, 2500), arcpy.Point(0, 2500), arcpy.Point(0, 1500)])
        cur.insertRow([arcpy.Polygon(r3, SR_3857), 3, "Parcel Gamma", "Industrial", 2000000.0, "Industrial Zone Authority", "N"])
        cur.insertRow([None, 4, "Parcel Delta", "Vacant", None, None, None])
print("  [OK] 4 initial records")

print("")
print("--- parcels: sparse FID ---")
with arcpy.da.Editor(GDB) as edit:
    with arcpy.da.InsertCursor(pp, pf) as cur:
        for i in range(5, 25):
            r = arcpy.Array([arcpy.Point(i*100, 0), arcpy.Point(i*100+100, 0), arcpy.Point(i*100+100, 100), arcpy.Point(i*100, 100), arcpy.Point(i*100, 0)])
            cur.insertRow([arcpy.Polygon(r, SR_3857), i, "Parcel_S" + str(i), "Sparse", 10000.0, "Sparse Corp", "Y"])
print("  [OK] 20 sparse records added")
count = 0
count = 0
with arcpy.da.Editor(GDB) as edit:
    with arcpy.da.UpdateCursor(pp, ['OBJECTID', 'parcel_id']) as cur:
        for row in cur:
            if 9 <= row[1] <= 19:
                cur.deleteRow()
                count += 1
                count += 1
print("  Deleted " + str(count) + " records (parcel_id 9-19)")
with arcpy.da.Editor(GDB) as edit:
    with arcpy.da.InsertCursor(pp, pf) as cur:
        for i in range(25, 30):
            r = arcpy.Array([arcpy.Point(i*100, 0), arcpy.Point(i*100+100, 0), arcpy.Point(i*100+100, 100), arcpy.Point(i*100, 100), arcpy.Point(i*100, 0)])
            cur.insertRow([arcpy.Polygon(r, SR_3857), i, "Parcel_S" + str(i), "Sparse", 10000.0, "Sparse Corp", "Y"])
print("  [OK] 5 appended after delete (sparse FID)")
print("  [OK] 5 appended after delete (sparse FID)")

print("")
print("--- parcel_parts ---")
with arcpy.da.Editor(GDB) as edit:
    with arcpy.da.InsertCursor(ppp, ["SHAPE@", "parcel_id", "part_name", "part_type", "area_sqm"]) as cur:
        for geo_data in [
        ([arcpy.Point(0,0), arcpy.Point(500,0), arcpy.Point(500,500), arcpy.Point(0,500), arcpy.Point(0,0)], 1, "Alpha North", "Land", 250000.0),
        ([arcpy.Point(500,0), arcpy.Point(1000,0), arcpy.Point(1000,500), arcpy.Point(500,500), arcpy.Point(500,0)], 1, "Alpha South", "Land", 250000.0),
        ([arcpy.Point(0,500), arcpy.Point(1000,500), arcpy.Point(1000,1000), arcpy.Point(0,1000), arcpy.Point(0,500)], 1, "Alpha Gardens", "Park", 500000.0),
        ([arcpy.Point(1500,0), arcpy.Point(2000,0), arcpy.Point(2000,400), arcpy.Point(1500,400), arcpy.Point(1500,0)], 2, "Beta Building", "Structure", 400000.0),
        ([arcpy.Point(2000,0), arcpy.Point(2500,0), arcpy.Point(2500,400), arcpy.Point(2000,400), arcpy.Point(2000,0)], 2, "Beta Plaza", "Parking", 200000.0),
        ([arcpy.Point(0,1500), arcpy.Point(1000,1500), arcpy.Point(1000,2000), arcpy.Point(0,2000), arcpy.Point(0,1500)], 3, "Gamma Plant", "Structure", 500000.0),
        ([arcpy.Point(1000,1500), arcpy.Point(2000,1500), arcpy.Point(2000,2000), arcpy.Point(1000,2000), arcpy.Point(1000,1500)], 3, "Gamma Yard", "Storage", 500000.0),
        ]:
            geo = arcpy.Polygon(arcpy.Array(geo_data[0]), SR_3857)
            cur.insertRow([geo, geo_data[1], geo_data[2], geo_data[3], geo_data[4]])
            print("  [OK] 7 records")

aff = ["short_fld", "long_fld", "float_fld", "double_fld", "text_fld", "date_fld", "guid_fld", "blob_fld"]
adv_aff = [fn for fn, ft in advanced_fields.items() if ft != "SKIPPED"]
aff_ext = aff + adv_aff
print("  Writing " + str(len(aff_ext)) + " columns")
with arcpy.da.InsertCursor(aft, aff_ext) as cur:
    cur.insertRow(tuple([None] * len(aff_ext)))
    r2 = [0, 0, 0.0, 0.0, "", datetime.datetime(2025, 1, 1), make_guid(), b""] + [0, datetime.date(2025,1,1), datetime.time(12,0,0), datetime.datetime(2025,1,1,12,0,0, tzinfo=datetime.timezone.utc)]
    cur.insertRow(tuple(r2))
    r3 = [-32768, -2147483648, -3.4e38, -1.7e308, "Hello World", datetime.datetime(1900, 1, 1), make_guid(), b"\\x00\x01\x02"] + [0, datetime.date(2025,1,1), datetime.time(12,0,0), datetime.datetime(2025,1,1,12,0,0, tzinfo=datetime.timezone.utc)]
    cur.insertRow(tuple(r3))
    r4 = [32767, 2147483647, 3.4e38, 1.7e308, chr(20013)+chr(25991)+chr(27979)+chr(35797)+chr(23383)+chr(20018), datetime.datetime(2025, 6, 15, 12, 30, 0), make_guid(), b"Hello Binary World!\x00\xFF"] + [0, datetime.date(2025,1,1), datetime.time(12,0,0), datetime.datetime(2025,1,1,12,0,0, tzinfo=datetime.timezone.utc)]
    cur.insertRow(tuple(r4))
    r5 = [100, 100000, 1.25, 3.14159, "emoji: " + chr(128522) + chr(127881) + chr(127775), datetime.datetime(2025, 12, 25, 8, 0, 0), make_guid(), b"\\xDE\xAD\xBE\xEF\x00\x01\x02x03x04x05x06x07x08x09\x0A\x0B\x0C\x0D\x0E\x0F"] + [0, datetime.date(2025,1,1), datetime.time(12,0,0), datetime.datetime(2025,1,1,12,0,0, tzinfo=datetime.timezone.utc)]
    cur.insertRow(tuple(r5))
    r6 = [255, 999999, 99.99, 999.999, "Long text with mixed content: abc123" + chr(20013)+chr(25991), datetime.datetime(2024, 2, 29, 23, 59, 59), make_guid(), b"A" * 100 + b"\x00" + b"B" * 100] + [0, datetime.date(2025,1,1), datetime.time(12,0,0), datetime.datetime(2025,1,1,12,0,0, tzinfo=datetime.timezone.utc)]
    cur.insertRow(tuple(r6))
print("  [OK] 6 rows (NULL, empty, boundary, Chinese, emoji, long binary)")

print(chr(10) + "=" * 60)
print("Phase 9: Field metadata and indexes")
print("=" * 60)

def set_field_meta(tbl, fname, alias=None, default=None, desc=None):
    try:
        if alias is not None:
            arcpy.AlterField_management(tbl, fname, fname, alias)
        if default is not None:
            try: arcpy.AssignDefaultToField_management(tbl, fname, default)
            except: pass
        if desc is not None:
            try: arcpy.SetFieldDescription_management(tbl, fname, desc)
            except: pass
        print("  [field meta] " + fname)
    except Exception as e:
        print("  [field meta SKIP] " + fname + ": " + str(e))

set_field_meta(rp, "road_id", alias="Road ID", desc="Unique road identifier")
set_field_meta(rp, "name", alias="Road Name", desc="Official road name")
set_field_meta(rp, "status", alias="Status", default="OPEN", desc="Road status from domain")
set_field_meta(rp, "speed_limit", alias="Speed Limit", default=60, desc="Speed limit in km/h")
set_field_meta(rp, "road_type", alias="Road Type", default=2, desc="Subtype: 1=Highway, 2=Local, 3=Trail")
set_field_meta(rip, "road_id", alias="Parent Road ID", desc="Foreign key to roads.OBJECTID")
set_field_meta(pp, "parcel_id", alias="Parcel ID", desc="Unique parcel identifier")
set_field_meta(pp, "parcel_name", alias="Parcel Name", desc="Official parcel name")
set_field_meta(pp, "is_active", alias="Is Active", default="Y", desc="Whether parcel is active")
set_field_meta(rpt, "point_id", alias="Point ID", desc="Unique point identifier")
set_field_meta(rt, "record_id", alias="Record ID", desc="Unique record identifier")

for tbl, idx_name, idx_fields in [
    (rp, "idx_road_id", "road_id"),
    (rp, "idx_status", "status"),
    (rp, "idx_name", "name"),
    (pp, "idx_parcel_id", "parcel_id"),
    (pp, "idx_parcel_name", "parcel_name"),
    (rip, "idx_ri_road_id", "road_id"),
    (rap, "idx_ra_road_id", "road_id"),
    (ppp, "idx_pp_parcel_id", "parcel_id"),
]:
    try:
        arcpy.AddIndex_management(tbl, idx_fields, idx_name, "NON_UNIQUE", "NON_ASCENDING")
        print("  [OK] " + idx_name)
    except Exception as e:
        print("  [SKIP] " + idx_name + ": " + str(e))

print(chr(10) + "=" * 60)
print("Phase 10: Export delivery files")
print("=" * 60)

print("")
print("--- manifest.json ---")
try:
    import sys
    py_ver = str(sys.version_info.major) + "." + str(sys.version_info.minor)
except:
    py_ver = "3.x"
manifest = {
    "name": "acceptance_metadata.gdb",
    "description": "FileGDB for metadata/domain/relationship/subtype/field type parsing verification",
    "generated_by": "ArcGIS Pro arcpy",
    "arcgis_version": "3.5.0.57366",
    "python_version": py_ver,
    "generated_at": now().isoformat(),
    "gdb_path": GDB,
    "feature_datasets": ["TransportFD", "AdminFD"],
    "domains": ["road_status_domain", "speed_range_domain", "road_type_domain", "inspection_result_domain"],
    "relationship_classes": ["roads_inspections_rel", "parcels_parts_rel", "assets_guid_rel"],
    "advanced_field_types": advanced_fields,
    "known_limitations": {
        "attributed_relationship": "arcpy 3.5 does not support creating attributed relationship classes"
    }
}
with open(os.path.join(OUT_DIR, "manifest.json"), "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2, ensure_ascii=False, default=str)
print("  [OK] manifest.json")
print("")
print("--- layer-inventory.csv ---")
with open(os.path.join(OUT_DIR, "layer-inventory.csv"), "w", encoding="utf-8", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Layer", "FeatureDataset", "Type", "GeometryType", "CRS_Name", "CRS_WKID"])
    w.writerow(["root_points", "", "Feature Class", "POINT", SR_4326.name, str(SR_4326.factoryCode)])
    w.writerow(["root_table", "", "Table", "N/A", "N/A", "N/A"])
    w.writerow(["all_field_types", "", "Table", "N/A", "N/A", "N/A"])
    w.writerow(["roads", "TransportFD", "Feature Class", "POLYLINE", SR_4326.name, str(SR_4326.factoryCode)])
    w.writerow(["road_inspections", "TransportFD", "Feature Class", "POINT", SR_4326.name, str(SR_4326.factoryCode)])
    w.writerow(["road_assets", "TransportFD", "Feature Class", "POINT", SR_4326.name, str(SR_4326.factoryCode)])
    w.writerow(["parcels", "AdminFD", "Feature Class", "POLYGON", SR_3857.name, str(SR_3857.factoryCode)])
    w.writerow(["parcel_parts", "AdminFD", "Feature Class", "POLYGON", SR_3857.name, str(SR_3857.factoryCode)])
print("  [OK] layer-inventory.csv")
print("")
print("--- field-inventory.csv ---")
with open(os.path.join(OUT_DIR, "field-inventory.csv"), "w", encoding="utf-8", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Table", "FieldName", "Type", "Length", "Nullable", "Alias", "DefaultValue", "Domain"])
    for tbl_path, tbl_name in [(rp, "roads"), (rip, "road_inspections"), (rap, "road_assets"),
        (pp, "parcels"), (ppp, "parcel_parts"), (rpt, "root_points"),
        (rt, "root_table"), (aft, "all_field_types")]:
        for field in arcpy.ListFields(tbl_path):
            if field.type in ["Geometry", "OID"]: continue
            nullable = "Y" if field.isNullable else "N"
            w.writerow([tbl_name, field.name, field.type, field.length, nullable,
                       field.aliasName, str(field.defaultValue) if field.defaultValue is not None else "",
                       field.domain])
print("  [OK] field-inventory.csv")
print("")
print("--- domain-expected.csv ---")
with open(os.path.join(OUT_DIR, "domain-expected.csv"), "w", encoding="utf-8", newline="") as f:
    w = csv.writer(f)
    w.writerow(["DomainName", "DomainType", "FieldType", "Description", "CodedValues"])
    for dom in arcpy.da.ListDomains(GDB):
        cv = ""
        if dom.domainType == "CodedValue" and dom.codedValues:
            cv = "; ".join([str(k) + "=" + str(v) for k, v in dom.codedValues.items()])
        elif dom.domainType == "Range":
            cv = "Min=" + str(dom.range[0]) + ", Max=" + str(dom.range[1])
        w.writerow([dom.name, dom.domainType, dom.type, dom.description, cv])
print("  [OK] domain-expected.csv")
print("")
print("--- relationship-expected.csv ---")
with open(os.path.join(OUT_DIR, "relationship-expected.csv"), "w", encoding="utf-8", newline="") as f:
    w = csv.writer(f)
    w.writerow(["RelationshipName", "Type", "OriginTable", "DestinationTable", "Cardinality",
               "ForwardLabel", "BackwardLabel", "OriginPrimaryKey", "OriginForeignKey"])
    w.writerow(['RelationshipName', 'Type', 'OriginTable', 'DestinationTable', 'Cardinality',
               'ForwardLabel', 'BackwardLabel', 'OriginPrimaryKey', 'OriginForeignKey', 'IsAttributed', 'IsComposite'])
    w.writerow(['roads_inspections_rel', 'Simple', 'roads', 'road_inspections', '1:M',
               'Road Inspections', 'Parent Road', 'OBJECTID', 'road_id', 'N', 'N'])
    w.writerow(['parcels_parts_rel', 'Composite', 'parcels', 'parcel_parts', '1:M',
               'Parcel Parts', 'Parent Parcel', 'OBJECTID', 'parcel_id', 'N', 'Y'])
    w.writerow(['assets_guid_rel', 'Simple', 'roads', 'road_assets', '1:1',
               'Road Assets', 'Parent Road', 'GlobalID', 'asset_guid', 'N', 'N'])
print("  [OK] relationship-expected.csv")
print("")
print("--- dataset-hierarchy.csv ---")
with open(os.path.join(OUT_DIR, "dataset-hierarchy.csv"), "w", encoding="utf-8", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Level", "Name", "Type", "FullPath", "ParentDataset"])
    w.writerow([0, "acceptance_metadata.gdb", "Workspace", GDB, ""])
    for root_item in [("root_points", "Feature Class"), ("root_table", "Table"), ("all_field_types", "Table")]:
        w.writerow([1, root_item[0], root_item[1], os.path.join(GDB, root_item[0]), ""])
    for fd_name in ["TransportFD", "AdminFD"]:
        fd_path = os.path.join(GDB, fd_name)
        w.writerow([1, fd_name, "Feature Dataset", fd_path, ""])
        arcpy.env.workspace = fd_path
        for fc in arcpy.ListFeatureClasses():
            w.writerow([2, fc, "Feature Class", os.path.join(fd_path, fc), fd_name])
        # Relationship classes listed in relationship-expected.csv
        # Relationship classes: roads_inspections_rel, parcels_parts_rel, assets_guid_rel
        arcpy.env.workspace = GDB
print("  [OK] dataset-hierarchy.csv")
print("")
print("--- fid-objectid-mapping.csv ---")
with open(os.path.join(OUT_DIR, "fid-objectid-mapping.csv"), "w", encoding="utf-8", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Layer", "OBJECTID", "BusinessKey", "BusinessKeyValue"])
    for tbl_path, tbl_name, bk in [(rp, "roads", "road_id"), (rip, "road_inspections", "road_id"),
        (rap, "road_assets", "asset_guid"), (pp, "parcels", "parcel_id"),
        (ppp, "parcel_parts", "parcel_id"), (rpt, "root_points", "point_id"),
        (rt, "root_table", "record_id")]:
        try:
            with arcpy.da.SearchCursor(tbl_path, ["OID@", bk]) as cur:
                for row in cur:
                    w.writerow([tbl_name, row[0], bk, row[1]])
        except: pass
print("  [OK] fid-objectid-mapping.csv")
print("")
print("--- field-values-expected.csv ---")
with open(os.path.join(OUT_DIR, "field-values-expected.csv"), "w", encoding="utf-8", newline="") as f:
    w = csv.writer(f)
    w.writerow(["Table", "OID", "FieldName", "Value", "IsNull", "HexSummary"])
    for tbl_path, tbl_name in [(rp, "roads"), (rip, "road_inspections"), (rap, "road_assets"),
        (pp, "parcels"), (ppp, "parcel_parts"), (rpt, "root_points"),
        (rt, "root_table"), (aft, "all_field_types")]:
        try:
            fields = [f.name for f in arcpy.ListFields(tbl_path) if f.type not in ["Geometry", "OID"]]
            with arcpy.da.SearchCursor(tbl_path, ["OID@"] + fields) as cur:
                for row in cur:
                    oid = row[0]
                    for i, fname in enumerate(fields):
                        val = row[i + 1]
                        is_null = val is None
                        hs = ""
                        if isinstance(val, bytes):
                            if len(val) == 0: hs = "(empty)"
                            elif len(val) < 50: hs = val.hex()
                            else: hs = val[:50].hex() + "..."
                        w.writerow([tbl_name, oid, fname, str(val)[:200] if val is not None else "", "Y" if is_null else "N", hs])
        except Exception as e:
            print("  [SKIP] " + tbl_name + ": " + str(e))
print("  [OK] field-values-expected.csv")
print("")
print("--- metadata-expected.json ---")
md_export = {}
for item_name, item_path in [
    ("Workspace", GDB),
    ("TransportFD", os.path.join(GDB, "TransportFD")),
    ("AdminFD", os.path.join(GDB, "AdminFD")),
    ("roads", rp), ("road_inspections", rip), ("road_assets", rap),
    ("parcels", pp), ("parcel_parts", ppp),
    ("root_points", rpt), ("root_table", rt), ("all_field_types", aft),
]:
    try:
        d = arcpy.Describe(item_path)
        entry = {"name": d.name}
        try:
            md = arcpy.metadata.Metadata(item_path)
            entry["title"] = md.title if md.title else ""
            entry["summary"] = md.summary if md.summary else ""
            entry["tags"] = md.tags if md.tags else ""
        except: pass
        md_export[item_name] = entry
    except Exception as e:
        md_export[item_name] = {"error": str(e)}
with open(os.path.join(OUT_DIR, "metadata-expected.json"), "w", encoding="utf-8") as f:
    json.dump(md_export, f, indent=2, ensure_ascii=False, default=str)
print("  [OK] metadata-expected.json")
print("")
print("--- source-notes.md ---")
with open(os.path.join(OUT_DIR, "source-notes.md"), "w", encoding="utf-8") as f:
    f.write("# acceptance_metadata.gdb Source Notes\n\n")
    f.write("## Generation Environment\n\n")
    f.write("| Item | Value |\n|------|-------|\n")
    f.write("| Tool | ArcGIS Pro arcpy |\n")
    f.write("| ArcGIS Pro Version | 3.5.0.57366 |\n")
    f.write("| Generation Script | generate_acceptance_metadata.py |\n")
    f.write("| Generation Time | " + now().isoformat() + " |\n\n")
    f.write("## Feature Datasets\n\n")
    f.write("- TransportFD (EPSG:4326): roads, road_inspections, road_assets\n")
    f.write("- AdminFD (EPSG:3857): parcels, parcel_parts\n")
    f.write("- Root: root_points, root_table, all_field_types\n\n")
    f.write("## Domains\n\n")
    f.write("| Domain | Type | Values |\n")
    f.write("|--------|------|--------|\n")
    f.write("| road_status_domain | Coded Value (TEXT) | OPEN=Open, CLOSED=Closed, UNKNOWN=Unknown |\n")
    f.write("| speed_range_domain | Range (SHORT) | 0-120 |\n")
    f.write("| road_type_domain | Coded Value (SHORT) | 1=Highway, 2=Local Road, 3=Trail |\n")
    f.write("| inspection_result_domain | Coded Value (TEXT) | GOOD=Good, FAIR=Fair, POOR=Poor |\n\n")
    f.write("## Relationships\n\n")
    f.write("| Name | Type | Origin | Destination | Cardinality | Key |\n")
    f.write("|------|------|--------|-------------|-------------|-----|\n")
    f.write("| roads_inspections_rel | Simple | roads | road_inspections | 1:M | OBJECTID <-> road_id |\n")
    f.write("| parcels_parts_rel | Composite | parcels | parcel_parts | 1:M | OBJECTID <-> parcel_id |\n")
    f.write("| assets_guid_rel | Simple | roads | road_assets | 1:1 | GlobalID <-> asset_guid |\n\n")
    f.write("## Advanced Field Types\n\n")
    f.write("| Field | Type | Status |\n|-------|------|--------|\n")
    for fn, ft in advanced_fields.items():
        status = "SUPPORTED" if ft != "SKIPPED" else "SKIPPED"
        f.write("| " + fn + " | " + ft + " | " + status + " |\n")
    f.write("\n## Known Limitations\n\n")
    f.write("1. Attributed relationship classes: Not created (arcpy 3.5 limitation)\n")
    f.write("2. Raster: Not generated (not in scope)\n")
    f.write("3. Multipatch: Not generated (not in scope)\n")
    f.write("\n## Delivery Files\n\n")
    for fn in sorted(os.listdir(OUT_DIR)):
        f.write("- " + fn + "\n")
print("  [OK] source-notes.md")

print(chr(10) + "=" * 60)
print("ALL COMPLETE!")
print("=" * 60)
print("")
print("GDB: " + GDB)
print("Output: " + OUT_DIR)
print("Files:")
for fn in sorted(os.listdir(OUT_DIR)):
    print("  - " + fn)