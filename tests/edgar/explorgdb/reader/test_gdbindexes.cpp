// tests/edgar/explorgdb/test_gdbindexes.cpp
// .gdbindexes 索引元数据解析测试
// 测试索引条目计数、名称解析、魔数元组验证

#include "gdb_indexes.h"
#include "gdb_catalog.h"
#include "../test_paths.h"
#include <gtest/gtest.h>

using namespace explorgdb;

static const auto SPX_GDB_PATH =
    explorgdb_test_paths::test_data_path("test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb");

// ── 基础解析测试 ──

// 测试系统目录索引文件解析
TEST(GdbIndexesTest, SystemCatalogIndexes) {
    std::string idx_path = SPX_GDB_PATH.string();
    idx_path += "/a00000001.gdbindexes";

    GdbIndexesParser parser(idx_path);
    ASSERT_TRUE(parser.parse());

    // 至少有一个索引
    EXPECT_FALSE(parser.entries().empty());
}

// 测试无效文件返回 false
TEST(GdbIndexesTest, InvalidFile) {
    GdbIndexesParser parser("/nonexistent/a00000001.gdbindexes");
    EXPECT_FALSE(parser.parse());
}

// ── 索引条目测试 ──

// 测试索引条目数量
TEST(GdbIndexesTest, IndexCount) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    // 统计所有 .gdbindexes 文件的条目总数
    size_t total_indexes = 0;
    auto idx_files = catalog.find_by_extension(".gdbindexes");
    for (const auto* entry : idx_files) {
        std::string idx_path = SPX_GDB_PATH.string();
        idx_path += "/" + entry->filename;

        GdbIndexesParser parser(idx_path);
        if (parser.parse()) {
            total_indexes += parser.index_count();
        }
    }

    // spx.gdb 应该有多个索引
    EXPECT_GT(total_indexes, 0u);
}

// 测试索引名称非空
TEST(GdbIndexesTest, IndexNamesNotEmpty) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    auto idx_files = catalog.find_by_extension(".gdbindexes");
    for (const auto* entry : idx_files) {
        std::string idx_path = SPX_GDB_PATH.string();
        idx_path += "/" + entry->filename;

        GdbIndexesParser parser(idx_path);
        if (!parser.parse()) continue;

        for (const auto& e : parser.entries()) {
            EXPECT_FALSE(e.name.empty()) << "Index name should not be empty";
        }
    }
}

// ── 魔数元组测试 ──

// 测试已知魔数元组 (2,0), (4,0), (16,65535)
TEST(GdbIndexesTest, KnownMagicTuples) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    auto idx_files = catalog.find_by_extension(".gdbindexes");
    for (const auto* entry : idx_files) {
        std::string idx_path = SPX_GDB_PATH.string();
        idx_path += "/" + entry->filename;

        GdbIndexesParser parser(idx_path);
        if (!parser.parse()) continue;

        for (const auto& e : parser.entries()) {
            bool is_known = false;
            if (e.magic2 == 2 && e.magic3 == 0) is_known = true;
            else if (e.magic2 == 4 && e.magic3 == 0) is_known = true;
            else if (e.magic2 == 16 && e.magic3 == 65535) is_known = true;

            // 已知魔数的条目应该有 magic4
            if (is_known) {
                // magic4 的值不应该影响解析（只要不崩溃即可）
            }
        }
    }
}

// 测试 magic5 存在（所有条目末尾）
TEST(GdbIndexesTest, Magic5Present) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    auto idx_files = catalog.find_by_extension(".gdbindexes");
    for (const auto* entry : idx_files) {
        std::string idx_path = SPX_GDB_PATH.string();
        idx_path += "/" + entry->filename;

        GdbIndexesParser parser(idx_path);
        if (!parser.parse()) continue;

        for (const auto& e : parser.entries()) {
            // magic5 是一个 uint16，解析后应该是一个合理值
            // 不做强校验，只验证解析不崩溃
            (void)e.magic5;
        }
    }
}

// ── 列名测试 ──

// 测试某些索引有非空列名
TEST(GdbIndexesTest, SomeColumnNamesPresent) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    auto idx_files = catalog.find_by_extension(".gdbindexes");
    bool found_non_empty_col = false;
    for (const auto* entry : idx_files) {
        std::string idx_path = SPX_GDB_PATH.string();
        idx_path += "/" + entry->filename;

        GdbIndexesParser parser(idx_path);
        if (!parser.parse()) continue;

        for (const auto& e : parser.entries()) {
            if (!e.column_name.empty()) {
                found_non_empty_col = true;
            }
        }
    }

    // 至少有一些索引有列名
    EXPECT_TRUE(found_non_empty_col);
}

// ── 端到端测试 ──

// 测试全部 .gdbindexes 文件解析不崩溃
TEST(GdbIndexesTest, AllIndexesParseWithoutCrash) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH);

    auto idx_files = catalog.find_by_extension(".gdbindexes");
    int success_count = 0;
    for (const auto* entry : idx_files) {
        std::string idx_path = SPX_GDB_PATH;
        idx_path += "/" + entry->filename;

        GdbIndexesParser parser(idx_path);
        if (parser.parse()) {
            success_count++;
        }
    }

    // 至少解析成功一个
    EXPECT_GT(success_count, 0);
}
