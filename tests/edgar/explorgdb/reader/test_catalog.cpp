// tests/edgar/explorgdb/test_catalog.cpp
// GDB 目录扫描和 magic 验证测试
// 测试目录文件枚举、gdb 头部 magic 校验、文件分类

#include "gdb_catalog.h"
#include "../test_paths.h"
#include <gtest/gtest.h>

using namespace explorgdb;

static const auto SPX_GDB_PATH =
    explorgdb_test_paths::test_data_path("test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb");

// ── 目录扫描测试 ──

// 测试成功扫描 spx.gdb 目录
TEST(GdbCatalogTest, ScanSpxGdb) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPX_GDB_PATH.string()));

    EXPECT_GT(catalog.entries().size(), 20u);
}

// 测试无效路径返回 false
TEST(GdbCatalogTest, InvalidPath) {
    GdbCatalog catalog;
    EXPECT_FALSE(catalog.scan("/nonexistent/path"));
}

// ── Magic 文件测试 ──

// 测试 magic 文件版本和魔数
TEST(GdbCatalogTest, MagicValid) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPX_GDB_PATH.string()));

    EXPECT_TRUE(catalog.magic().version == 0u || catalog.magic().version == 5u);
    EXPECT_TRUE(catalog.magic().magic == 0u || catalog.magic().magic == 0xEFBEADDEu);
}

// ── 文件枚举测试 ──

// 测试按扩展名查找文件
TEST(GdbCatalogTest, FindByExtension) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPX_GDB_PATH.string()));

    auto tables = catalog.find_by_extension(".gdbtable");
    EXPECT_GT(tables.size(), 0u);

    auto tablxes = catalog.find_by_extension(".gdbtablx");
    EXPECT_GT(tablxes.size(), 0u);

    auto indexes = catalog.find_by_extension(".gdbindexes");
    EXPECT_GT(indexes.size(), 0u);

    auto spx_files = catalog.find_by_extension(".spx");
    EXPECT_GE(spx_files.size(), 1u);
}

// 测试按 ID 查找特定文件
TEST(GdbCatalogTest, FindById) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    // GDB_SystemCatalog 表 (id=1)
    const auto* table = catalog.find_table(1);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->filename, "a00000001.gdbtable");

    const auto* tablx = catalog.find_tablx(1);
    ASSERT_NE(tablx, nullptr);
    EXPECT_EQ(tablx->filename, "a00000001.gdbtablx");
}

// 测试不存在的 ID 返回 nullptr
TEST(GdbCatalogTest, FindNonExistent) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH);

    EXPECT_EQ(catalog.find_table(999), nullptr);
    EXPECT_EQ(catalog.find_tablx(999), nullptr);
}

// 测试 find_spx 按 ID 查找空间索引
TEST(GdbCatalogTest, FindSpx) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPX_GDB_PATH.string()));

    // spx.gdb 有 .spx 文件，尝试查找
    auto spx_entries = catalog.find_by_extension(".spx");
    ASSERT_FALSE(spx_entries.empty());

    // 用已知 ID 查找
    uint32_t spx_id = spx_entries[0]->numeric_id;
    const auto* spx = catalog.find_spx(spx_id);
    ASSERT_NE(spx, nullptr);
    EXPECT_EQ(spx->extension, ".spx");

    // 不存在的 ID
    EXPECT_EQ(catalog.find_spx(999), nullptr);
}

// 测试 find_atx 按 ID + 索引名查找属性索引
TEST(GdbCatalogTest, FindAtx) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPX_GDB_PATH.string()));

    // spx.gdb 已知有 .atx 文件: a00000001.TablesByName.atx
    // 直接测试 find_atx
    const auto* found = catalog.find_atx(1, "TablesByName");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->numeric_id, 1u);

    // 不存在的索引名
    EXPECT_EQ(catalog.find_atx(1, "NonExistentIndex"), nullptr);
    // 不存在的 ID
    EXPECT_EQ(catalog.find_atx(999, "TablesByName"), nullptr);
}

// 测试 find_all_atx 查找某个表的所有属性索引
TEST(GdbCatalogTest, FindAllAtx) {
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(SPX_GDB_PATH.string()));

    // 表 ID=4 有多个 .atx: CatItemsByPhysicalName, CatItemsByType, FDO_UUID
    auto all_atx = catalog.find_all_atx(4);
    EXPECT_GE(all_atx.size(), 3u);
    for (const auto* e : all_atx) {
        EXPECT_EQ(e->numeric_id, 4u);
    }

    // 没有 .atx 的表应返回空
    auto empty = catalog.find_all_atx(999);
    EXPECT_TRUE(empty.empty());
}

// ── 文件条目信息测试 ──

// 测试文件条目的 numeric_id 和 file_size
TEST(GdbCatalogTest, EntryInfo) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    auto tables = catalog.find_by_extension(".gdbtable");
    // 第一个表 id 应该是 1
    EXPECT_EQ(tables[0]->numeric_id, 1u);
    // 文件大小应大于 0
    EXPECT_GT(tables[0]->file_size, 0u);
}
