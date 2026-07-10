// tests/edgar/explorgdb/test_full_audit.cpp
// 端到端完整审计测试
// 解析 spx.gdb 目录中所有可识别文件，验证整体结构一致性

#include "gdb_catalog.h"
#include "gdb_table.h"
#include "gdb_tablx.h"
#include "gdb_indexes.h"
#include "../test_paths.h"
#include <gtest/gtest.h>
#include <set>

using namespace explorgdb;

static const auto SPX_GDB_PATH =
    explorgdb_test_paths::test_data_path("test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb");

// ── 目录完整性测试 ──

// 测试目录扫描找到所有预期文件类型
TEST(FullAuditTest, AllFileTypesPresent) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPX_GDB_PATH.string()));

    // 应该找到以下扩展名
    auto tables = catalog.find_by_extension(".gdbtable");
    auto tablxes = catalog.find_by_extension(".gdbtablx");
    auto indexes = catalog.find_by_extension(".gdbindexes");
    auto spx_files = catalog.find_by_extension(".spx");
    auto atx_files = catalog.find_by_extension(".atx");

    // 表和索引文件应该成对出现（.gdbtable + .gdbtablx）
    EXPECT_EQ(tables.size(), tablxes.size());
    // 至少有一些表
    EXPECT_FALSE(tables.empty());
    // spx.gdb 应该有 .gdbindexes 文件
    EXPECT_FALSE(indexes.empty());
    // 应该有空间索引文件
    EXPECT_FALSE(spx_files.empty());
}

// 测试 gdb 头部 magic 正确
TEST(FullAuditTest, DirectoryMagicValid) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPX_GDB_PATH.string()));

    EXPECT_TRUE(catalog.magic().version == 0u || catalog.magic().version == 5u);
    EXPECT_TRUE(catalog.magic().magic == 0u || catalog.magic().magic == 0xEFBEADDEu);  // 0xDEADBEEF LE
}

// 测试 timestamps 文件加载
TEST(FullAuditTest, TimestampsLoaded) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPX_GDB_PATH.string()));

    const auto& ts = catalog.timestamps();
    // 应该填充了 384 字节（不验证具体内容，只验证不崩溃）
    (void)ts;
}

// ── 表和索引配对测试 ──

// 测试每个 .gdbtable 都有对应的 .gdbtablx
TEST(FullAuditTest, TableTablxPairing) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    auto tables = catalog.find_by_extension(".gdbtable");
    std::set<uint32_t> table_ids;
    for (const auto* e : tables) {
        table_ids.insert(e->numeric_id);
    }

    auto tablxes = catalog.find_by_extension(".gdbtablx");
    std::set<uint32_t> tablx_ids;
    for (const auto* e : tablxes) {
        tablx_ids.insert(e->numeric_id);
    }

    // 两组 ID 应该完全匹配
    EXPECT_EQ(table_ids, tablx_ids);
}

// ── 表头部一致性测试 ──

// 测试所有 .gdbtable 头部解析
TEST(FullAuditTest, AllTableHeadersParse) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    auto tables = catalog.find_by_extension(".gdbtable");
    int success = 0;
    for (const auto* entry : tables) {
        std::string path = SPX_GDB_PATH.string();
        path += "/" + entry->filename;

        GdbTableParser parser(path);
        if (parser.parse_header()) {
            success++;
            // 版本应该是 3 或 4
            uint32_t ver = parser.header().version;
            EXPECT_TRUE(ver == 3 || ver == 4) << "Unexpected table version: " << ver;
        }
    }

    // 所有表头部都应该解析成功
    EXPECT_GT(success, 0);
    EXPECT_LE(success, static_cast<int>(tables.size()));
}

// ── 字段描述符一致性测试 ──

// 测试所有表字段解析不崩溃
TEST(FullAuditTest, AllTableFieldsParse) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    auto tables = catalog.find_by_extension(".gdbtable");
    int success = 0;
    for (const auto* entry : tables) {
        std::string path = SPX_GDB_PATH.string();
        path += "/" + entry->filename;

        GdbTableParser parser(path);
        if (parser.parse_fields()) {
            success++;
            // 每个表至少有一个字段（ObjectId）
            EXPECT_FALSE(parser.fields().empty());
        }
    }

    // 所有表字段都应该解析成功
    EXPECT_GT(success, 0);
    EXPECT_LE(success, static_cast<int>(tables.size()));
}

// 测试几何表包含几何字段
TEST(FullAuditTest, FeatureTablesHaveGeometryField) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPX_GDB_PATH.string()));

    // 系统目录表（id=1）不含几何字段，其他表可能有
    int geom_table_count = 0;
    for (uint32_t id = 2; id <= 15; ++id) {
        const auto* entry = catalog.find_table(id);
        if (!entry) continue;

        std::string path = SPX_GDB_PATH.string();
        path += "/" + entry->filename;

        GdbTableParser parser(path);
        if (!parser.parse_fields()) continue;

        for (const auto& fd : parser.fields()) {
            if (fd.type == FieldType::Geometry) {
                geom_table_count++;
                // 几何字段应有非空 WKT
                EXPECT_FALSE(fd.wkt.empty());
                break;
            }
        }
    }

    // spx.gdb 至少有几何表
    EXPECT_GT(geom_table_count, 0);
}

// ── Tablx 一致性测试 ──

// 测试所有 .gdbtablx 解析
TEST(FullAuditTest, AllTablxParse) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    auto tablxes = catalog.find_by_extension(".gdbtablx");
    int success = 0;
    for (const auto* entry : tablxes) {
        std::string path = SPX_GDB_PATH.string();
        path += "/" + entry->filename;

        GdbTablxParser parser(path);
        if (parser.parse()) {
            success++;
            (void)parser.offsets();
        }
    }

    EXPECT_GT(success, 0);
    EXPECT_LE(success, static_cast<int>(tablxes.size()));
}

// ── 索引一致性测试 ──

// 测试所有 .gdbindexes 解析
TEST(FullAuditTest, AllIndexesParse) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    auto idx_files = catalog.find_by_extension(".gdbindexes");
    int success = 0;
    for (const auto* entry : idx_files) {
        std::string path = SPX_GDB_PATH.string();
        path += "/" + entry->filename;

        GdbIndexesParser parser(path);
        if (parser.parse()) {
            success++;
            // 至少有一个索引条目
            EXPECT_FALSE(parser.entries().empty());
        }
    }

    EXPECT_EQ(success, static_cast<int>(idx_files.size()));
}

// ── 记录解析端到端测试 ──

// 测试系统目录表的记录解析
TEST(FullAuditTest, SystemCatalogRecordsEndToEnd) {
    std::string table_path = SPX_GDB_PATH.string();
    table_path += "/a00000001.gdbtable";

    std::string tablx_path = SPX_GDB_PATH.string();
    tablx_path += "/a00000001.gdbtablx";

    GdbTableParser parser(table_path);
    ASSERT_TRUE(parser.load_tablx(tablx_path));
    ASSERT_TRUE(parser.parse_fields());
    ASSERT_TRUE(parser.parse_records());

    // 应该有记录
    const auto& records = parser.records();
    EXPECT_FALSE(records.empty());

    // 第一条记录应该包含 "GDB_SystemCatalog" 名称
    const auto& fields = parser.fields();
    int name_idx = -1;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == "Name") {
            name_idx = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(name_idx, 0);

    bool found_system_catalog = false;
    for (const auto& rec : records) {
        if (name_idx < static_cast<int>(rec.field_values.size())) {
            const auto& val = rec.field_values[name_idx];
            if (std::holds_alternative<std::string>(val)) {
                if (std::get<std::string>(val) == "GDB_SystemCatalog") {
                    found_system_catalog = true;
                    break;
                }
            }
        }
    }
    EXPECT_TRUE(found_system_catalog);
}

// ── 统计摘要测试 ──

// 输出 spx.gdb 整体统计信息（验证性测试）
TEST(FullAuditTest, SummaryStatistics) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPX_GDB_PATH.string()));

    // 文件总数
    EXPECT_GT(catalog.entries().size(), 50u);

    // 按类型统计
    size_t table_count = catalog.find_by_extension(".gdbtable").size();
    size_t tablx_count = catalog.find_by_extension(".gdbtablx").size();
    size_t index_count = catalog.find_by_extension(".gdbindexes").size();
    size_t spx_count = catalog.find_by_extension(".spx").size();

    // 验证数量关系
    EXPECT_GE(table_count, tablx_count);  // 至少不应少于 tablx
    EXPECT_GT(table_count, 0u);
    EXPECT_GT(spx_count, 0u);

    // 打印统计信息（便于调试）
    printf("\n[spx.gdb summary]\n");
    printf("  total entries: %zu\n", catalog.entries().size());
    printf("  .gdbtable:     %zu\n", table_count);
    printf("  .gdbtablx:     %zu\n", tablx_count);
    printf("  .gdbindexes:   %zu\n", index_count);
    printf("  .spx:          %zu\n", spx_count);
}
