// tests/edgar/explorgdb/test_synthetic.cpp
// 自包含合成数据测试 — 不依赖外部 .gdb 文件
//
// 使用 test_fixture_explorgdb.h 在内存/临时目录中构造最小合法 GDB 数据，
// 精确验证解析结果的每一个值。
//
// 测试覆盖：
//   1. 目录扫描 + magic 验证（磁盘临时目录）
//   2. .gdbtable 头部 + 字段解析（内存缓冲区）
//   3. .gdbtablx 偏移表 + 位图（内存缓冲区）
//   4. .gdbindexes 索引元数据（内存缓冲区）
//   5. 端到端记录解析（table + tablx 组合）
//   6. 精确值验证（score=100, fid=0/1, nullable 等）

#include "test_fixture_explorgdb.h"
#include "binary_reader.h"
#include "gdb_catalog.h"
#include "gdb_table.h"
#include "gdb_tablx.h"
#include "gdb_indexes.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

using namespace explorgdb;

// ── 目录扫描测试（使用临时目录） ──

// 测试合成的 .gdb 目录能被正确扫描
TEST(SyntheticTest, CatalogScan) {
    std::string dir = explorgdb_test::create_synthetic_gdb("test_catalog_scan.gdb");

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dir));

    // 应该找到 3 个 aXXXXXXXX.* 文件
    EXPECT_EQ(catalog.entries().size(), 3u);

    // magic 应该正确
    EXPECT_EQ(catalog.magic().version, 5u);
    EXPECT_EQ(catalog.magic().magic, 0xEFBEADDEu);  // 0xDEADBEEF LE

    // timestamps 应该加载
    EXPECT_EQ(catalog.timestamps().raw[0], 0xAB);

    explorgdb_test::cleanup_synthetic_gdb(dir);
}

// 测试按扩展名查找合成目录中的文件
TEST(SyntheticTest, CatalogFindByExtension) {
    std::string dir = explorgdb_test::create_synthetic_gdb("test_find_by_ext.gdb");

    GdbCatalog catalog;
    catalog.scan(dir);

    auto tables = catalog.find_by_extension(".gdbtable");
    EXPECT_EQ(tables.size(), 1u);
    if (!tables.empty()) {
        EXPECT_EQ(tables[0]->filename, "a00000001.gdbtable");
        EXPECT_EQ(tables[0]->numeric_id, 1u);
    }

    auto tablxes = catalog.find_by_extension(".gdbtablx");
    EXPECT_EQ(tablxes.size(), 1u);

    auto indexes = catalog.find_by_extension(".gdbindexes");
    EXPECT_EQ(indexes.size(), 1u);

    explorgdb_test::cleanup_synthetic_gdb(dir);
}

// ── 内存 .gdbtable 测试 ──

// 测试内存构造的 .gdbtable 头部解析
TEST(SyntheticTest, MemoryTableHeader) {
    auto tbl = explorgdb_test::build_minimal_table();

    GdbTableParser parser("<memory>");
    // 手动加载数据（不通过文件）
    // 这里我们用 BinaryReader 直接验证头部
    BinaryReader br(tbl.data);
    uint32_t version = br.read_u32();
    EXPECT_EQ(version, 3u);

    uint32_t nfeatures = br.read_u32();
    EXPECT_EQ(nfeatures, 2u);

    uint32_t largest = br.read_u32();
    EXPECT_EQ(largest, 32u);

    // v3 头部布局 (40 字节):
    // [0]  version(4)
    // [4]  nfeatures(4)
    // [8]  largest_size(4)
    // [12] role(4)
    // [16] unknown_16(4)
    // [20] unknown_20(4)
    // [24] file_size(8)
    // [32] field_desc_offset(8)

    uint32_t role = br.read_u32();
    EXPECT_EQ(role, 5u);

    br.read_u32();  // unknown_16
    br.read_u32();  // unknown_20

    uint64_t file_size = br.read_u64();
    EXPECT_EQ(file_size, 256u);

    uint64_t fdo = br.read_u64();
    EXPECT_EQ(fdo, 40u);
}

// 测试内存 .gdbtable 字段描述符解析
TEST(SyntheticTest, MemoryTableFields) {
    auto tbl = explorgdb_test::build_minimal_table();

    BinaryReader br(tbl.data);
    br.seek(tbl.field_desc_offset);

    // Section Header
    uint32_t section_len = br.read_u32();
    EXPECT_GT(section_len, 0u);

    uint32_t section_ver = br.read_u32();
    EXPECT_EQ(section_ver, 3u);

    uint32_t geom_type = br.read_u32();
    EXPECT_EQ(geom_type, 0u);  // 无几何

    uint16_t nfields = br.read_u16();
    EXPECT_EQ(nfields, 2u);

    // 字段 0: ObjectId
    uint8_t name_len0 = br.read_u8();
    EXPECT_EQ(name_len0, 8u);
    std::string name0 = br.read_utf16(name_len0);
    EXPECT_EQ(name0, "ObjectId");

    uint8_t alias_len0 = br.read_u8();
    EXPECT_EQ(alias_len0, 0u);
    br.read_utf16(alias_len0);  // 空别名

    uint8_t type0 = br.read_u8();
    EXPECT_EQ(type0, 6u);  // ObjectId

    uint8_t width0 = br.read_u8();
    EXPECT_EQ(width0, 4u);

    uint8_t flag0 = br.read_u8();
    EXPECT_EQ(flag0, 0u);  // not nullable

    // 字段 1: Int32 "score"
    uint8_t name_len1 = br.read_u8();
    EXPECT_EQ(name_len1, 5u);
    std::string name1 = br.read_utf16(name_len1);
    EXPECT_EQ(name1, "score");

    uint8_t alias_len1 = br.read_u8();
    br.read_utf16(alias_len1);  // 空

    uint8_t type1 = br.read_u8();
    EXPECT_EQ(type1, 1u);  // Int32

    uint8_t width1 = br.read_u8();
    EXPECT_EQ(width1, 4u);

    uint8_t flag1 = br.read_u8();
    EXPECT_EQ(flag1, 1u);  // nullable
}

// ── 内存 .gdbtablx 测试 ──

// 测试内存构造的 .gdbtablx 偏移表
TEST(SyntheticTest, MemoryTablxOffsets) {
    std::vector<uint8_t> tablx = explorgdb_test::build_minimal_tablx(48, 60);

    BinaryReader br(tablx);

    // 头部
    uint32_t version = br.read_u32();
    EXPECT_EQ(version, 3u);

    uint32_t nblocks = br.read_u32();
    EXPECT_EQ(nblocks, 1u);

    uint32_t nfeatures = br.read_u32();
    EXPECT_EQ(nfeatures, 1024u);

    uint32_t offset_size = br.read_u32();
    EXPECT_EQ(offset_size, 4u);

    // 验证前 3 个偏移
    uint32_t off0 = br.read_u32();
    EXPECT_EQ(off0, 48u);

    uint32_t off1 = br.read_u32();
    EXPECT_EQ(off1, 60u);

    uint32_t off2 = br.read_u32();
    EXPECT_EQ(off2, 0u);  // null 要素
}

// 测试 .gdbtablx 解析器完整解析
TEST(SyntheticTest, TablxParserFull) {
    std::vector<uint8_t> tablx = explorgdb_test::build_minimal_tablx(48, 60);

    // 写入临时文件
    std::string tmp_file = fs::temp_directory_path().string() + "/test_tablx.gdbtablx";
    std::ofstream ofs(tmp_file, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(tablx.data()), tablx.size());
    ofs.close();

    GdbTablxParser parser(tmp_file);
    ASSERT_TRUE(parser.parse());

    EXPECT_EQ(parser.header().version, 3u);
    EXPECT_EQ(parser.header().size_tablx_offsets, 4u);
    EXPECT_EQ(parser.feature_count(), 2u);

    EXPECT_EQ(parser.get_offset(0), 48u);
    EXPECT_EQ(parser.get_offset(1), 60u);
    EXPECT_EQ(parser.get_offset(2), 0u);
    EXPECT_EQ(parser.get_offset(9999), 0u);

    // 位图: 块 0 应该活跃
    EXPECT_TRUE(parser.is_block_active(0));

    fs::remove(tmp_file);
}

// ── 内存 .gdbindexes 测试 ──

// 测试内存构造的 .gdbindexes 解析
TEST(SyntheticTest, MemoryIndexes) {
    std::vector<uint8_t> idx = explorgdb_test::build_minimal_indexes();

    // 写入临时文件
    std::string tmp_file = fs::temp_directory_path().string() + "/test_idx.gdbindexes";
    std::ofstream ofs(tmp_file, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(idx.data()), idx.size());
    ofs.close();

    GdbIndexesParser parser(tmp_file);
    ASSERT_TRUE(parser.parse());

    EXPECT_EQ(parser.index_count(), 1u);

    const auto& entry = parser.entries()[0];
    EXPECT_EQ(entry.name, "PK_score");
    EXPECT_EQ(entry.magic2, 2);
    EXPECT_EQ(entry.magic3, 0);
    EXPECT_EQ(entry.magic4, 0);  // known magic 有 magic4
    EXPECT_EQ(entry.column_name, "score");

    fs::remove(tmp_file);
}

// ── 端到端记录解析测试 ──

// 测试合成数据的完整记录解析（table + tablx 组合）
TEST(SyntheticTest, EndToEndRecords) {
    // 1. 构造 .gdbtable 数据
    auto tbl = explorgdb_test::build_minimal_table();

    // 2. 构造对应的 .gdbtablx 数据（使用记录的偏移）
    auto tablx_data = explorgdb_test::build_minimal_tablx(
        static_cast<uint32_t>(tbl.rec0_offset),
        static_cast<uint32_t>(tbl.rec1_offset));

    // 3. 写入临时文件
    std::string tbl_path = fs::temp_directory_path().string() + "/test_e2e.gdbtable";
    std::string tablx_path = fs::temp_directory_path().string() + "/test_e2e.gdbtablx";

    {
        std::ofstream ofs(tbl_path, std::ios::binary);
        ofs.write(reinterpret_cast<char*>(tbl.data.data()), tbl.data.size());
    }
    {
        std::ofstream ofs(tablx_path, std::ios::binary);
        ofs.write(reinterpret_cast<char*>(tablx_data.data()), tablx_data.size());
    }

    // 4. 解析
    GdbTableParser parser(tbl_path);
    ASSERT_TRUE(parser.load_tablx(tablx_path));
    ASSERT_TRUE(parser.parse_fields());
    ASSERT_TRUE(parser.parse_records());

    // 5. 验证字段 schema
    const auto& fields = parser.fields();
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_EQ(fields[0].name, "ObjectId");
    EXPECT_EQ(fields[0].type, FieldType::ObjectId);
    EXPECT_EQ(fields[1].name, "score");
    EXPECT_EQ(fields[1].type, FieldType::Int32);

    // 6. 验证记录
    const auto& records = parser.records();
    ASSERT_EQ(records.size(), 2u);

    // 记录 0: FID=0, ObjectId=1, score=100
    const auto& rec0 = records[0];
    EXPECT_EQ(rec0.fid, 0u);

    // ObjectId 值 = FID + 1
    ASSERT_TRUE(std::holds_alternative<int32_t>(rec0.field_values[0]));
    EXPECT_EQ(std::get<int32_t>(rec0.field_values[0]), 1);

    // score = 100
    ASSERT_TRUE(std::holds_alternative<int32_t>(rec0.field_values[1]));
    EXPECT_EQ(std::get<int32_t>(rec0.field_values[1]), 100);

    // 记录 1: FID=1, ObjectId=2, score=null
    const auto& rec1 = records[1];
    EXPECT_EQ(rec1.fid, 1u);

    ASSERT_TRUE(std::holds_alternative<int32_t>(rec1.field_values[0]));
    EXPECT_EQ(std::get<int32_t>(rec1.field_values[0]), 2);

    // score 为 null
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(rec1.field_values[1]));

    // 清理
    fs::remove(tbl_path);
    fs::remove(tablx_path);
}

// ── 多表目录测试 ──

// 测试包含多个表的合成目录
TEST(SyntheticTest, MultiTableCatalog) {
    std::string dir = fs::temp_directory_path().string() + "/test_multi.gdb";
    fs::create_directories(dir);

    // gdb magic 文件
    {
        std::vector<uint8_t> buf;
        explorgdb_test::write_u32(buf, 5);
        explorgdb_test::write_u32(buf, 0xDEADBEEF);
        std::ofstream ofs(dir + "/gdb", std::ios::binary);
        ofs.write(reinterpret_cast<char*>(buf.data()), buf.size());
    }

    // timestamps
    {
        std::vector<uint8_t> buf(384, 0xCC);
        std::ofstream ofs(dir + "/timestamps", std::ios::binary);
        ofs.write(reinterpret_cast<char*>(buf.data()), buf.size());
    }

    // 构造 3 个不同的表 (id=1, 2, 3)
    for (uint32_t id = 1; id <= 3; ++id) {
        auto tbl = explorgdb_test::build_minimal_table();
        auto tablx = explorgdb_test::build_minimal_tablx(
            static_cast<uint32_t>(tbl.rec0_offset),
            static_cast<uint32_t>(tbl.rec1_offset));

        // 写 aXXXXXXXX.gdbtable
        char fname[64];
        snprintf(fname, sizeof(fname), "a%08x.gdbtable", id);
        std::ofstream ofs_t(dir + "/" + std::string(fname), std::ios::binary);
        ofs_t.write(reinterpret_cast<char*>(tbl.data.data()), tbl.data.size());

        // 写 aXXXXXXXX.gdbtablx
        snprintf(fname, sizeof(fname), "a%08x.gdbtablx", id);
        std::ofstream ofs_x(dir + "/" + std::string(fname), std::ios::binary);
        ofs_x.write(reinterpret_cast<char*>(tablx.data()), tablx.size());
    }

    // 扫描和验证
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dir));

    EXPECT_EQ(catalog.entries().size(), 6u);  // 3 tables + 3 tablx

    // 每个表都能被找到
    for (uint32_t id = 1; id <= 3; ++id) {
        const auto* table = catalog.find_table(id);
        ASSERT_NE(table, nullptr) << "Table " << id << " not found";
        EXPECT_EQ(table->numeric_id, id);

        const auto* tablx = catalog.find_tablx(id);
        ASSERT_NE(tablx, nullptr) << "Tablx " << id << " not found";
    }

    // id=4 不存在
    EXPECT_EQ(catalog.find_table(4), nullptr);

    explorgdb_test::cleanup_synthetic_gdb(dir);
}
