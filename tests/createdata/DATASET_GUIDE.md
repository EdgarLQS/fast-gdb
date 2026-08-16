# acceptance_metadata.gdb 数据集生成指南

## 一句话

使用 ArcGIS Pro arcpy 原生创建，用于验证 fast-gdb 对 Domain、Relationship、Subtype、全字段类型、FID 稀疏、空几何、元数据的解析能力。

---

## 1. 生成环境

| 项目 | 要求 |
|------|------|
| 工具 | ArcGIS Pro 3.0+ (arcpy) |
| ArcGIS Pro Python | 通过 `-PythonPath` 指定，或由脚本从系统注册表自动检测 |
| 生成脚本 | `tests/createdata/python/generate_acceptance_metadata.py` |
| 输出 GDB | `test_data/gdb/acceptance_metadata.gdb` |
| 交付目录 | `test_data/gdb/acceptance_metadata/` |

---

## 2. 数据结构

### 2.1 图层布局

```
acceptance_metadata.gdb
├── root_points              Point FC, EPSG:4326, 4 records (3 valid + 1 空几何)
├── root_table               Standalone Table, 5 records
├── all_field_types          Table, 6 rows, 12 字段类型
├── TransportFD (FD, EPSG:4326)
│   ├── roads                Polyline, 6 records (含 1 空几何)
│   ├── road_inspections     Point, 5 records
│   └── road_assets          Point, 5 records (+GlobalID, +GUID)
└── AdminFD (FD, EPSG:3857)
    ├── parcels              Polygon, 18 records (稀疏 FID: 4初始+20添加-11删除+5追加)
    └── parcel_parts         Polygon, 7 records
```

### 2.2 Domain

| 名称 | 类型 | 内容 | 绑定字段 |
|------|------|------|----------|
| road_status_domain | Coded Value (TEXT) | OPEN=Open, CLOSED=Closed, UNKNOWN=Unknown | roads.status |
| speed_range_domain | Range (SHORT) | 0–120 | roads.speed_limit |
| road_type_domain | Coded Value (SHORT) | 1=Highway, 2=Local Road, 3=Trail | roads.road_type (subtype) |
| inspection_result_domain | Coded Value (TEXT) | GOOD=Good, FAIR=Fair, POOR=Poor | road_inspections.result |

### 2.3 Relationship Class

| 名称 | 类型 | Origin | Dest | 基数 | 键 | 标签 |
|------|------|--------|------|------|-----|------|
| roads_inspections_rel | Simple | roads | road_inspections | 1:M | OBJECTID ↔ road_id | Road Inspections / Parent Road |
| parcels_parts_rel | Composite | parcels | parcel_parts | 1:M | OBJECTID ↔ parcel_id | Parcel Parts / Parent Parcel |
| assets_guid_rel | Simple | roads | road_assets | 1:1 | GlobalID ↔ asset_guid | Road Assets / Parent Road |

### 2.4 Subtype

roads.road_type 字段:

| Code | Name | Defaults (未设置 — 见下方限制) |
|------|------|------|
| 1 | Highway | speed_limit=100, status=OPEN |
| 2 | Local Road | speed_limit=60, status=OPEN |
| 3 | Trail | speed_limit=30, status=CLOSED |

> **注意**: arcpy 3.5 没有 `SetDefaultValueForSubtype_management` API，per-subtype 默认值未写入。当前使用 field-level 默认值 (speed_limit=60, status=OPEN) 作为 fallback。如需精确 per-subtype 默认值，需手动写 GDB_Items XML Definition。

### 2.5 all_field_types 表 (12 字段)

| 字段 | 类型 | 写入值 |
|------|------|--------|
| short_fld | SHORT | NULL, 0, -32768, 32767, 100, 255 |
| long_fld | LONG | NULL, 0, -2147483648, 2147483647, 100000, 999999 |
| float_fld | FLOAT | NULL, 0, -3.4e38, 3.4e38, 1.25, 99.99 |
| double_fld | DOUBLE | NULL, 0, -1.7e308, 1.7e308, 3.14159, 999.999 |
| text_fld | TEXT(100) | NULL, "", "Hello World", "中文测试", "emoji: 😊🎉🌟", "Long text..." |
| date_fld | DATE | NULL, 2025-01-01, 1900-01-01, 2025-06-15, 2025-12-25, 2024-02-29 |
| guid_fld | GUID | NULL, UUID, UUID, UUID, UUID, UUID |
| blob_fld | BLOB | NULL, empty, \\x00\\x01\\x02, "Hello Bin...", binary, 200 bytes |
| bigint_fld | BIGINTEGER | 0 (全部默认) |
| dateonly_fld | DATEONLY | 2025-01-01 (全部默认) |
| timeonly_fld | TIMEONLY | 12:00:00 (全部默认) |
| timestampoffset_fld | TIMESTAMPOFFSET | 2025-01-01T12:00:00+00:00 (全部默认) |

### 2.6 属性索引

| 索引 | 表 | 字段 |
|------|-----|------|
| idx_road_id | roads | road_id |
| idx_status | roads | status |
| idx_name | roads | name |
| idx_parcel_id | parcels | parcel_id |
| idx_parcel_name | parcels | parcel_name |
| idx_ri_road_id | road_inspections | road_id |
| idx_ra_road_id | road_assets | road_id |
| idx_pp_parcel_id | parcel_parts | parcel_id |

---

## 3. 快速生成

### 3.1 找到 ArcGIS Pro Python

```powershell
# 查看注册表找到 Python 路径
Get-ItemProperty "HKLM:\SOFTWARE\ESRI\ArcGISPro" | Select-Object PythonCondaRoot, PythonCondaEnv
```

实际路径取决于本机 ArcGIS Pro 安装位置；也可以直接运行
`tests/createdata/powershell/generate_test_data.ps1 -PythonPath <python.exe>`。

### 3.2 运行脚本

```powershell
& "D:\path\to\arcgispro-py3\python.exe" -X utf8 `
    "<repo-root>\tests\createdata\python\generate_acceptance_metadata.py"
```

> 脚本会自动删除旧 GDB 并重建，每次运行结果一致。

### 3.3 脚本内部路径

脚本默认根据自身位置定位仓库根目录。需要使用其他输出位置时，设置环境变量：

```powershell
$env:FAST_GDB_ARCPY_GDB = "<path-to-acceptance_metadata.gdb>"
$env:FAST_GDB_ARCPY_OUT_DIR = "<path-to-expected-files>"
```

---

## 4. 交付文件清单 (10 个)

全部位于 `test_data/gdb/acceptance_metadata/`：

| 文件 | 内容 | 用途 |
|------|------|------|
| manifest.json | 元数据：版本、环境、限制 | 数据来源声明 |
| layer-inventory.csv | 图层名、类型、几何、CRS | 图层清单对照 |
| field-inventory.csv | 字段名、类型、长度、别名、默认值、Domain | 字段定义对照 |
| domain-expected.csv | Domain 名称、类型、编码值/范围 | Domain 解析对照 |
| relationship-expected.csv | 关系名、类型、Origin/Dest、基数、键 | 关系解析对照 |
| dataset-hierarchy.csv | 层级：Workspace→FD→FC/Table | 数据集层级对照 |
| fid-objectid-mapping.csv | 每行 OID 和业务主键 | FID 映射对照 |
| field-values-expected.csv | 每行每字段的值、NULL 标志、Binary hex | 字段值精度对照 |
| metadata-expected.json | 标题、摘要、标签、说明 | 元数据解析对照 |
| source-notes.md | 生成说明、已知限制 | 人工阅读 |

---

## 5. 已知限制

| 限制 | 说明 |
|------|------|
| per-subtype 默认值 | arcpy 3.5 无 SetDefaultValueForSubtype API，subtype 默认值未写入 |
| Attributed Relationship | arcpy 3.5 不支持创建带属性的关系类 |
| 空几何 | 通过 InsertCursor 写 None 实现，不是 arcpy.Geometry() 空对象 |
| 路径 | 生成结果使用仓库相对路径，避免写入机器相关路径 |

---

## 6. 验证通过清单

当在 fast-gdb 中解析时，应达到以下状态:

- [x] 系统表清单一致
- [x] 表和字段数量一致
- [x] 每条记录均可读取
- [x] NULL/类型/原始 Binary/XML 一致
- [x] Domain 定义和字段绑定一致
- [x] Relationship 边和属性一致
- [x] Feature Dataset 层级一致
- [x] FID/ObjectID 映射一致
- [x] 几何 WKB、空几何和诊断一致
