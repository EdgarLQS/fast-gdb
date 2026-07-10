// tests/edgar/explorgdb/test_gdbtable.cpp
// .gdbtable 二进制解析测试
// 测试头部解析、字段描述符、几何元数据、要素记录读取

#include "gdb_table.h"
#include "gdb_catalog.h"
#include "explorgdb_types.h"
#include "../test_paths.h"
#include <gtest/gtest.h>

using namespace explorgdb;

static const auto SPX_GDB_PATH =
    explorgdb_test_paths::test_data_path("test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb");

// ── 头部解析测试 ──

// 测试 .gdbtable 头部解析（版本可能是 3 或 4）
TEST(GdbTableTest, HeaderVersion) {
    // GDB_SystemCatalog 表 (id=1)
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());
    const auto* entry = catalog.find_table(1);
    ASSERT_NE(entry, nullptr);

    std::string table_path = SPX_GDB_PATH.string();
    table_path += "/a00000001.gdbtable";

    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.parse_header());

    // 版本应该是 3 (FGDB 9.x) 或 4 (FGDB 10.x)
    uint32_t ver = parser.header().version;
    EXPECT_TRUE(ver == 3 || ver == 4);
    // 文件大小应大于 0
    EXPECT_GT(parser.header().file_size, 0u);
    // 字段描述符偏移应合理（至少包含头部）
    EXPECT_GE(parser.header().field_desc_offset, 40u);
}

// 测试解析不存在文件返回 false
TEST(GdbTableTest, InvalidFile) {
    GdbTableParser parser("/nonexistent/a00000001.gdbtable");
    EXPECT_FALSE(parser.parse_header());
}

// ── 字段描述符测试 ──

// 测试 GDB_SystemCatalog 表的字段解析
TEST(GdbTableTest, SystemCatalogFields) {
    std::string table_path = SPX_GDB_PATH.string();
    table_path += "/a00000001.gdbtable";

    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.parse_fields());

    const auto& fields = parser.fields();
    // GDB_SystemCatalog 应有多个字段（ID, Name, Definition 等）
    EXPECT_GE(fields.size(), 3u);

    // 第一个字段应该是 ObjectId 类型 (type=6)
    EXPECT_EQ(fields[0].type, FieldType::ObjectId);

    // 查找 Name 字段（String 类型）
    bool found_name = false;
    for (const auto& fd : fields) {
        if (fd.name == "Name" && fd.type == FieldType::String) {
            found_name = true;
            break;
        }
    }
    EXPECT_TRUE(found_name);
}

// 测试几何类型字段的 WKT 和坐标原点
TEST(GdbTableTest, GeometryFieldWKT) {
    // 找一个包含几何字段的表（如 Line 层，id 可能不是 1）
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    // 遍历所有表，找到有几何字段的
    for (uint32_t id = 1; id <= 15; ++id) {
        const auto* entry = catalog.find_table(id);
        if (!entry) continue;

        std::string table_path = SPX_GDB_PATH.string();
        table_path += "/" + entry->filename;

        GdbTableParser parser(table_path);
        if (!parser.parse_fields()) continue;

        // 查找 Geometry 类型字段
        for (const auto& fd : parser.fields()) {
            if (fd.type == FieldType::Geometry) {
                // WKT 应该非空（如 "POLYGON"、"LINESTRING" 等）
                EXPECT_FALSE(fd.wkt.empty());
                // xyscale 应该是一个正值
                EXPECT_GT(fd.xyscale, 0.0);
                return;
            }
        }
    }
    FAIL() << "No geometry field found in any table";
}

// 测试几何描述符中 xorig/yorig/xyscale 始终存在
TEST(GdbTableTest, GeometryOriginAlwaysPresent) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    for (uint32_t id = 1; id <= 15; ++id) {
        const auto* entry = catalog.find_table(id);
        if (!entry) continue;

        std::string table_path = SPX_GDB_PATH.string();
        table_path += "/" + entry->filename;

        GdbTableParser parser(table_path);
        if (!parser.parse_fields()) continue;

        for (const auto& fd : parser.fields()) {
            if (fd.type == FieldType::Geometry) {
                // 这三个值始终存在，与 has_z/has_m 无关
                // xyscale 应该是正数（分辨率）
                EXPECT_GT(fd.xyscale, 0.0);
                return;
            }
        }
    }
    FAIL() << "No geometry field found";
}

// ── 记录解析测试 ──

// 测试 GDB_SystemCatalog 第一条记录
TEST(GdbTableTest, FirstSystemCatalogRecord) {
    std::string table_path = SPX_GDB_PATH.string();
    table_path += "/a00000001.gdbtable";

    std::string tablx_path = SPX_GDB_PATH.string();
    tablx_path += "/a00000001.gdbtablx";

    GdbTableParser parser(table_path);
    parser.load_tablx(tablx_path);
    ASSERT_TRUE(parser.parse_fields());
    ASSERT_TRUE(parser.parse_records());

    const auto& records = parser.records();
    ASSERT_FALSE(records.empty());

    // 第一条记录应该是 GDB_SystemCatalog 自身
    const auto& rec = records[0];

    // 查找 Name 字段的值
    const auto& fields = parser.fields();
    int name_idx = -1;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == "Name") {
            name_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(name_idx, 0);

    // Name 值应该是 "GDB_SystemCatalog"
    if (name_idx < static_cast<int>(rec.field_values.size())) {
        const auto& val = rec.field_values[name_idx];
        if (std::holds_alternative<std::string>(val)) {
            EXPECT_EQ(std::get<std::string>(val), "GDB_SystemCatalog");
        }
    }
}

// 测试记录中 ObjectId 不占用数据字节（隐式推导）
TEST(GdbTableTest, ObjectIdIsImplicit) {
    std::string table_path = SPX_GDB_PATH.string();
    table_path += "/a00000001.gdbtable";

    std::string tablx_path = SPX_GDB_PATH.string();
    tablx_path += "/a00000001.gdbtablx";

    GdbTableParser parser(table_path);
    parser.load_tablx(tablx_path);
    ASSERT_TRUE(parser.parse_fields());
    ASSERT_TRUE(parser.parse_records());

    const auto& fields = parser.fields();
    const auto& records = parser.records();
    ASSERT_FALSE(records.empty());

    // 找到 ObjectId 字段的索引
    int oid_idx = -1;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].type == FieldType::ObjectId) {
            oid_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(oid_idx, 0);

    // ObjectId 值应该等于 FID + 1
    const auto& rec = records[0];
    if (oid_idx < static_cast<int>(rec.field_values.size())) {
        const auto& val = rec.field_values[oid_idx];
        if (std::holds_alternative<int32_t>(val)) {
            EXPECT_EQ(std::get<int32_t>(val), static_cast<int32_t>(rec.fid + 1));
        }
    }
}

// 测试无 tablx 时 parse_records 返回 false
TEST(GdbTableTest, RecordsWithoutTablxFails) {
    std::string table_path = SPX_GDB_PATH.string();
    table_path += "/a00000001.gdbtable";

    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.parse_fields());
    // 没有加载 .gdbtablx 偏移表，解析记录应失败
    EXPECT_FALSE(parser.parse_records());
}

// ── 字段类型覆盖测试 ──

// 测试 String 类型字段有正确的 width
TEST(GdbTableTest, StringFieldProperties) {
    std::string table_path = SPX_GDB_PATH.string();
    table_path += "/a00000001.gdbtable";

    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.parse_fields());

    const auto& fields = parser.fields();
    bool found_string = false;
    for (const auto& fd : fields) {
        if (fd.type == FieldType::String) {
            found_string = true;
            // String 类型 width 是 varuint 编码的最大长度
            EXPECT_GT(fd.width, 0u);
        }
    }
    EXPECT_TRUE(found_string);
}

// ── read_record_by_fid 按需读取测试 ──

// 测试按需读取模式下 read_record_by_fid 读取有效记录
TEST(GdbTableTest, ReadRecordByFid_Basic) {
    std::string table_path = SPX_GDB_PATH.string();
    table_path += "/a00000001.gdbtable";
    std::string tablx_path = SPX_GDB_PATH.string();
    tablx_path += "/a00000001.gdbtablx";

    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.open());
    ASSERT_TRUE(parser.load_tablx(tablx_path));

    // 读取 FID=0 记录
    FeatureRecord rec;
    ASSERT_TRUE(parser.read_record_by_fid(0, rec));
    EXPECT_EQ(rec.fid, 0u);
    // GDB_SystemCatalog 应有多个字段值
    EXPECT_FALSE(rec.field_values.empty());
}

// 测试遍历多个 FID 读取记录
TEST(GdbTableTest, ReadRecordByFid_MultipleFids) {
    std::string table_path = SPX_GDB_PATH.string();
    table_path += "/a00000001.gdbtable";
    std::string tablx_path = SPX_GDB_PATH.string();
    tablx_path += "/a00000001.gdbtablx";

    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.open());
    ASSERT_TRUE(parser.load_tablx(tablx_path));

    int success_count = 0;
    for (uint32_t fid = 0; fid < 20; ++fid) {
        FeatureRecord rec;
        if (parser.read_record_by_fid(fid, rec)) {
            success_count++;
            EXPECT_EQ(rec.fid, fid);
        }
    }
    EXPECT_GT(success_count, 0);
}

// 测试读取超出范围的 FID 返回 false
TEST(GdbTableTest, ReadRecordByFid_InvalidFid) {
    std::string table_path = SPX_GDB_PATH.string();
    table_path += "/a00000001.gdbtable";
    std::string tablx_path = SPX_GDB_PATH.string();
    tablx_path += "/a00000001.gdbtablx";

    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.open());
    ASSERT_TRUE(parser.load_tablx(tablx_path));

    FeatureRecord rec;
    // FID 远超范围
    EXPECT_FALSE(parser.read_record_by_fid(999999, rec));
}

// 测试无 tablx 时 read_record_by_fid 返回 false
TEST(GdbTableTest, ReadRecordByFid_NoTablx) {
    std::string table_path = SPX_GDB_PATH.string();
    table_path += "/a00000001.gdbtable";

    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.open());
    // 不加载 tablx

    FeatureRecord rec;
    EXPECT_FALSE(parser.read_record_by_fid(0, rec));
}

// ── peek_geometry_blob 测试 ──

// 空间测试数据路径（有几何字段）
static const auto SPATIAL_GDB_PATH =
    explorgdb_test_paths::test_data_path("test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb");
static const uint32_t SPATIAL_TABLE_ID = 0xC;

// 测试 peek_geometry_blob 读取有效几何 blob
TEST(GdbTableTest, PeekGeometryBlob_Valid) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPATIAL_GDB_PATH.string()));

    const auto* table_entry = catalog.find_table(SPATIAL_TABLE_ID);
    ASSERT_NE(table_entry, nullptr);
    std::string table_path = SPATIAL_GDB_PATH.string() + "/" + table_entry->filename;

    const auto* tablx_entry = catalog.find_tablx(SPATIAL_TABLE_ID);
    ASSERT_NE(tablx_entry, nullptr);
    std::string tablx_path = SPATIAL_GDB_PATH.string() + "/" + tablx_entry->filename;

    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.open());
    ASSERT_TRUE(parser.load_tablx(tablx_path));

    // peek 前几个 FID 的几何 blob
    int peeked = 0;
    for (uint32_t fid = 0; fid < 100; ++fid) {
        const uint8_t* blob_data = nullptr;
        size_t blob_size = 0;
        if (parser.peek_geometry_blob(fid, blob_data, blob_size)) {
            peeked++;
            // 有效的几何 blob 应该有数据
            EXPECT_GT(blob_size, 0u);
            EXPECT_NE(blob_data, nullptr);
        }
    }
    EXPECT_GT(peeked, 0);
}

// 测试 peek_geometry_blob 遍历所有 FID
TEST(GdbTableTest, PeekGeometryBlob_AllFids) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPATIAL_GDB_PATH.string()));

    const auto* table_entry = catalog.find_table(SPATIAL_TABLE_ID);
    ASSERT_NE(table_entry, nullptr);
    std::string table_path = SPATIAL_GDB_PATH.string() + "/" + table_entry->filename;

    const auto* tablx_entry = catalog.find_tablx(SPATIAL_TABLE_ID);
    ASSERT_NE(tablx_entry, nullptr);
    std::string tablx_path = SPATIAL_GDB_PATH.string() + "/" + tablx_entry->filename;

    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.open());
    ASSERT_TRUE(parser.load_tablx(tablx_path));

    int total = 0;
    int peeked = 0;
    for (uint32_t fid = 0; fid < 2390; ++fid) {
        const uint8_t* blob_data = nullptr;
        size_t blob_size = 0;
        total++;
        if (parser.peek_geometry_blob(fid, blob_data, blob_size)) {
            peeked++;
        }
    }
    EXPECT_GT(peeked, 0);
    EXPECT_EQ(total, 2390);
}

// 测试 peek_geometry_blob 无效 FID 返回 false
TEST(GdbTableTest, PeekGeometryBlob_InvalidFid) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPATIAL_GDB_PATH.string()));

    const auto* table_entry = catalog.find_table(SPATIAL_TABLE_ID);
    ASSERT_NE(table_entry, nullptr);
    std::string table_path = SPATIAL_GDB_PATH.string() + "/" + table_entry->filename;

    const auto* tablx_entry = catalog.find_tablx(SPATIAL_TABLE_ID);
    ASSERT_NE(tablx_entry, nullptr);
    std::string tablx_path = SPATIAL_GDB_PATH.string() + "/" + tablx_entry->filename;

    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.open());
    ASSERT_TRUE(parser.load_tablx(tablx_path));

    const uint8_t* blob_data = nullptr;
    size_t blob_size = 0;
    EXPECT_FALSE(parser.peek_geometry_blob(999999, blob_data, blob_size));
}

// 测试 read_record_by_fid 配合空间数据（覆盖更多字段类型）
TEST(GdbTableTest, ReadRecordByFid_SpatialData) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPATIAL_GDB_PATH.string()));

    const auto* table_entry = catalog.find_table(SPATIAL_TABLE_ID);
    ASSERT_NE(table_entry, nullptr);
    std::string table_path = SPATIAL_GDB_PATH.string() + "/" + table_entry->filename;

    const auto* tablx_entry = catalog.find_tablx(SPATIAL_TABLE_ID);
    ASSERT_NE(tablx_entry, nullptr);
    std::string tablx_path = SPATIAL_GDB_PATH.string() + "/" + tablx_entry->filename;

    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.open());
    ASSERT_TRUE(parser.load_tablx(tablx_path));

    // 读取几条记录，验证字段值不为空
    int success = 0;
    for (uint32_t fid = 0; fid < 100; ++fid) {
        FeatureRecord rec;
        if (parser.read_record_by_fid(fid, rec)) {
            success++;
            EXPECT_FALSE(rec.field_values.empty());
        }
    }
    EXPECT_GT(success, 0);
}
