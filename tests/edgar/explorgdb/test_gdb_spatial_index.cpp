// tests/edgar/explorgdb/test_gdb_spatial_index.cpp
// GdbSpatialIndexParser 正确性单元测试
// 验证 parse() 和 query_bbox() 的基本行为

#include <gtest/gtest.h>
#include "gdb_spatial_index.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace explorgdb;

// ── 测试数据路径 ──

static const std::string kSmallSpx =
    "test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb/a0000000c.spx";

static const std::string kLargeSpx =
    "/Users/edgarlqs/Downloads/daydaydaywork/dailyWork/convert/gdal/"
    "fast_gdb/test_data/large/large_test.gdb/a00000009.spx";

// ── parse() 测试 ──

TEST(SpatialIndexTest, ParseValidSmall) {
    GdbSpatialIndexParser parser(kSmallSpx);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.trailer().value_size, 8);
    EXPECT_EQ(parser.trailer().magic1, 1u);
    EXPECT_GE(parser.trailer().tree_depth, 1u);
    EXPECT_LE(parser.trailer().tree_depth, 4u);
    EXPECT_GT(parser.trailer().total_value_count, 0u);
}

TEST(SpatialIndexTest, ParseValidLarge) {
    GdbSpatialIndexParser parser(kLargeSpx);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.trailer().value_size, 8);
    EXPECT_EQ(parser.trailer().magic1, 1u);
    EXPECT_EQ(parser.trailer().tree_depth, 3u);
    EXPECT_GT(parser.trailer().total_value_count, 1000000u);
}

TEST(SpatialIndexTest, ParseNonexistent) {
    GdbSpatialIndexParser parser("/nonexistent/path/fake.spx");
    EXPECT_FALSE(parser.parse());
}

TEST(SpatialIndexTest, ParseTruncated) {
    // 创建一个 < 22 字节的临时文件
    std::string tmp_path = "/tmp/test_truncated.spx";
    {
        std::ofstream ofs(tmp_path, std::ios::binary);
        ofs.write("short", 5);
    }
    GdbSpatialIndexParser parser(tmp_path);
    EXPECT_FALSE(parser.parse());
    fs::remove(tmp_path);
}

// ── query_bbox() 测试 ──

TEST(SpatialIndexTest, QuerySmallBbox) {
    // 使用大数据集（1M），参数确定有效
    GdbSpatialIndexParser parser(kLargeSpx);
    ASSERT_TRUE(parser.parse());

    // 1M 数据集参数：xorig=-400, yorig=-400, xyscale=1e9, grid=[400.0]
    double xorig = -400.0, yorig = -400.0, xyscale = 1e9;
    std::vector<double> grid_resolutions = {400.0};

    // 查询一个中等范围 bbox（数据范围 [0,100000]）
    auto fids = parser.query_bbox(
        50000, 50000, 50100, 50100,
        xorig, yorig, xyscale, grid_resolutions);

    // 应该返回一些结果
    EXPECT_GT(fids.size(), 0u);
}

TEST(SpatialIndexTest, QueryEmptyBbox) {
    // 查询数据范围外的 bbox → 应返回空
    GdbSpatialIndexParser parser(kSmallSpx);
    ASSERT_TRUE(parser.parse());

    double xorig = -400.0, yorig = -400.0, xyscale = 1e9;
    std::vector<double> grid_resolutions = {400.0};

    // 数据范围大约 [0, 100000]，查询完全在范围外
    auto fids = parser.query_bbox(
        -1000000, -1000000, -999000, -999000,
        xorig, yorig, xyscale, grid_resolutions);

    EXPECT_EQ(fids.size(), 0u);
}

TEST(SpatialIndexTest, QueryLargeBbox) {
    // 查询覆盖全部数据范围 → 应返回最多结果
    GdbSpatialIndexParser parser(kSmallSpx);
    ASSERT_TRUE(parser.parse());

    double xorig = -400.0, yorig = -400.0, xyscale = 1e9;
    std::vector<double> grid_resolutions = {400.0};

    auto fids = parser.query_bbox(
        -1000, -1000, 200000, 200000,
        xorig, yorig, xyscale, grid_resolutions);

    // 应该返回大量结果
    EXPECT_GT(fids.size(), 100u);
}

TEST(SpatialIndexTest, QueryEmptyGridResolutions) {
    // grid_resolutions 为空 → 应返回空
    GdbSpatialIndexParser parser(kSmallSpx);
    ASSERT_TRUE(parser.parse());

    auto fids = parser.query_bbox(
        50000, 50000, 50100, 50100,
        0, 0, 1e9, std::vector<double>{});

    EXPECT_EQ(fids.size(), 0u);
}

// ── 结果排序验证 ──

TEST(SpatialIndexTest, ResultsAreSorted) {
    GdbSpatialIndexParser parser(kSmallSpx);
    ASSERT_TRUE(parser.parse());

    double xorig = -400.0, yorig = -400.0, xyscale = 1e9;
    std::vector<double> grid_resolutions = {400.0};

    auto fids = parser.query_bbox(
        0, 0, 100000, 100000,
        xorig, yorig, xyscale, grid_resolutions);

    // 验证 FID 列表已排序（query_bbox 内部做了 sort + unique）
    for (size_t i = 1; i < fids.size(); ++i) {
        EXPECT_GT(fids[i], fids[i - 1]) << "FIDs should be strictly ascending at index " << i;
    }
}
