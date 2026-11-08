// tests/edgar/explorgdb/test_gdb_spatial_index.cpp
// GdbSpatialIndexParser 正确性单元测试
// 验证 parse() 和 query_bbox() 的基本行为

#include <gtest/gtest.h>
#include "gdb_spatial_index.h"
#include "test_paths.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace fs = std::filesystem;
using namespace explorgdb;

// ── 测试数据路径 ──

static const std::string kSmallSpx =
    explorgdb_test_paths::test_data_path(
        "test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb/a0000000c.spx").string();

static const std::string kLargeSpx =
    explorgdb_test_paths::test_data_path(
        "test_data/large/large_test.gdb/a00000009.spx").string();

namespace {

void write_u32_le(std::vector<uint8_t>& bytes,
                  size_t offset,
                  uint32_t value) {
    for (size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<uint8_t>(
            (value >> (index * 8U)) & 0xffU);
    }
}

void write_u64_le(std::vector<uint8_t>& bytes,
                  size_t offset,
                  uint64_t value) {
    for (size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<uint8_t>(
            (value >> (index * 8U)) & 0xffU);
    }
}

std::string build_v2_high_object_id_spx() {
    constexpr size_t kPageSize = 65536U;
    constexpr size_t kTrailerSize = 30U;
    constexpr size_t kObjectIdSize = 8U;
    constexpr size_t kNonLeafHeaderSize = 12U;
    constexpr size_t kLeafHeaderSize = 20U;
    constexpr size_t kCapacity =
        (kPageSize - kLeafHeaderSize) / (kObjectIdSize + 8U);
    constexpr size_t kValuesOffset =
        kLeafHeaderSize + kCapacity * kObjectIdSize;
    constexpr uint64_t kHighObjectId = 0x100000005ULL;
    constexpr uint64_t kCell = 1ULL << 29U;
    constexpr uint64_t kRawKey = (kCell << 31U) | kCell;
    const std::string path =
        (fs::temp_directory_path() / "fast_gdb_spatial_index_v2_high.spx")
            .string();

    std::vector<uint8_t> root(kPageSize, 0U);
    write_u32_le(root, kObjectIdSize, 1U);
    write_u64_le(root, kNonLeafHeaderSize, 2U);
    write_u64_le(root, kNonLeafHeaderSize + kObjectIdSize, 3U);
    write_u64_le(root, kValuesOffset, kRawKey);

    std::vector<uint8_t> leaf(kPageSize, 0U);
    write_u32_le(leaf, kObjectIdSize, 1U);
    write_u64_le(leaf, kLeafHeaderSize, kHighObjectId);
    write_u64_le(leaf, kValuesOffset, kRawKey);

    std::vector<uint8_t> empty_leaf(kPageSize, 0U);
    std::vector<uint8_t> trailer(kTrailerSize, 0U);
    trailer[0] = 8U;
    write_u32_le(trailer, 2U, 1U);
    write_u32_le(trailer, 6U, 2U);
    write_u64_le(trailer, 10U, 1U);
    write_u32_le(trailer, 26U, 2U);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(root.data()),
                 static_cast<std::streamsize>(root.size()));
    output.write(reinterpret_cast<const char*>(leaf.data()),
                 static_cast<std::streamsize>(leaf.size()));
    output.write(reinterpret_cast<const char*>(empty_leaf.data()),
                 static_cast<std::streamsize>(empty_leaf.size()));
    output.write(reinterpret_cast<const char*>(trailer.data()),
                 static_cast<std::streamsize>(trailer.size()));
    return path;
}

} // namespace

// ── parse() 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(SpatialIndexTest, ParseValidSmall) {
    GdbSpatialIndexParser parser(kSmallSpx);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.trailer().value_size, 8);
    EXPECT_EQ(parser.trailer().magic1, 1u);
    EXPECT_GE(parser.trailer().tree_depth, 1u);
    EXPECT_LE(parser.trailer().tree_depth, 4u);
    EXPECT_GT(parser.trailer().total_value_count, 0u);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(SpatialIndexTest, ParseValidLarge) {
    GdbSpatialIndexParser parser(kLargeSpx);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.trailer().value_size, 8);
    EXPECT_EQ(parser.trailer().magic1, 1u);
    EXPECT_EQ(parser.trailer().tree_depth, 3u);
    EXPECT_GT(parser.trailer().total_value_count, 1000000u);
}

TEST(SpatialIndexTest, VersionTwoQueries64BitObjectIdsThroughBranchPages) {
    const std::string path = build_v2_high_object_id_spx();
    GdbSpatialIndexParser parser(path);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.trailer().version, 2U);
    EXPECT_EQ(parser.trailer().page_size, 65536U);
    EXPECT_EQ(parser.trailer().object_id_size, 8U);

    bool succeeded = false;
    const std::vector<RowSlot> row_slots = parser.query_bbox(
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, {1.0}, 0, false, &succeeded);
    EXPECT_TRUE(succeeded);
    EXPECT_TRUE(parser.query_bbox(1, 0, 0, 0, 0, 0, 1, {1}, 0, false, &succeeded).empty());
    EXPECT_FALSE(succeeded);
    EXPECT_EQ(row_slots, (std::vector<RowSlot>{0x100000004ULL}));
    fs::remove(path);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(SpatialIndexTest, ParseNonexistent) {
    GdbSpatialIndexParser parser("/nonexistent/path/fake.spx");
    EXPECT_FALSE(parser.parse());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(SpatialIndexTest, RejectsPageWithTooManyEntries) {
    const std::string path = "/tmp/test_invalid_entry_count.spx";
    std::vector<uint8_t> data(
        GdbSpatialIndexParser::kPageSize +
        GdbSpatialIndexParser::kTrailerSize, 0);
    const uint32_t invalid_count = 341;
    std::memcpy(data.data() + 4, &invalid_count,
                sizeof(invalid_count));

    const size_t trailer = GdbSpatialIndexParser::kPageSize;
    data[trailer] = 8;
    const uint32_t magic = 1;
    const uint32_t depth = 1;
    std::memcpy(data.data() + trailer + 2, &magic, sizeof(magic));
    std::memcpy(data.data() + trailer + 6, &depth, sizeof(depth));
    write_u32_le(data, trailer + 18U, 1U);
    std::ofstream(path, std::ios::binary).write(
        reinterpret_cast<const char*>(data.data()), data.size());

    GdbSpatialIndexParser parser(path);
    ASSERT_TRUE(parser.parse());
    EXPECT_TRUE(parser.query_bbox(
        0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, {1.0}).empty());
    fs::remove(path);
}

// ── query_bbox() 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(SpatialIndexTest, QueryEmptyGridResolutions) {
    // grid_resolutions 为空 → 应返回空
    GdbSpatialIndexParser parser(kSmallSpx);
    ASSERT_TRUE(parser.parse());

    auto fids = parser.query_bbox(
        50000, 50000, 50100, 50100,
        0, 0, 1e9, std::vector<double>{});

    EXPECT_EQ(fids.size(), 0u);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(SpatialIndexTest, RejectsInvalidQueryInputs) {
    GdbSpatialIndexParser parser(kSmallSpx);
    ASSERT_TRUE(parser.parse());

    EXPECT_TRUE(parser.query_bbox(
        std::numeric_limits<double>::quiet_NaN(), 0.0, 1.0, 1.0,
        0.0, 0.0, 1.0, {1.0}).empty());
    EXPECT_TRUE(parser.query_bbox(
        0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 1.0, {0.0}).empty());
}

TEST(SpatialIndexTest, RejectsLayerExtentsOutsideLevelZeroCellRange) {
    EXPECT_TRUE(GdbSpatialIndexParser::supports_layer_extent(
        -100.0, -100.0, 100.0, 100.0, {1.0, 4.0, 16.0}));
    EXPECT_FALSE(GdbSpatialIndexParser::supports_layer_extent(
        119.0, 30.0, 121.0, 31.0, {1e-10, 4e-10, 16e-10}));
    EXPECT_FALSE(GdbSpatialIndexParser::supports_layer_extent(
        -121.0, 30.0, -119.0, 31.0, {1e-10, 4e-10, 16e-10}));
    EXPECT_FALSE(GdbSpatialIndexParser::supports_layer_extent(
        0.0, 0.0, 1.0, 1.0, {0.0}));
    EXPECT_FALSE(GdbSpatialIndexParser::supports_layer_extent(
        0.0, 0.0, 1.0, 1.0, {1.0, 0.0}));
}

// ── 结果排序验证 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

TEST(SpatialIndexTest, CoarseGridOffsetAndSignedLevelsKeepPointCandidates) {
    for (uint32_t version : {1U, 2U}) for (bool branched : {false, true})
      for (double coarse : {16.0, 11.2}) for (bool merge : {false, true}) {
        const size_t page_size = version == 1 ? 4096 : 65536;
        const size_t oid = version == 1 ? 4 : 8;
        const size_t header = version == 1 ? 12 : 20;
        const size_t value_offset = header + ((page_size - header) / (oid + 8)) * oid;
        const double grids[] = {1.0, 4.0, coarse};
        for (double coordinate : {-64.0, -0.25, 0.0, 3.999999999, 4.0, 64.0}) {
            std::vector<std::pair<uint64_t, uint64_t>> keys;
            for (size_t level = 0; level < 3; ++level) {
                // Independent GDAL Writer formula, including the scaled shift.
                const auto cell = static_cast<uint64_t>(std::floor(coordinate / grids[level] +
                    double(1U << 29) / (grids[level] / grids[0])));
                keys.emplace_back((uint64_t(level) << 62) | (cell << 31) | cell, level + 1);
            }
            std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) {
                return (a.first ^ (uint64_t{1} << 63)) < (b.first ^ (uint64_t{1} << 63));
            });
            const size_t pages = branched ? 3 : 1;
            std::vector<uint8_t> bytes(pages * page_size + (version == 1 ? 22 : 30), 0);
            const auto put_oid = [&](size_t offset, uint64_t value) {
                if (oid == 4) write_u32_le(bytes, offset, static_cast<uint32_t>(value));
                else write_u64_le(bytes, offset, value);
            };
            if (branched) {
                write_u32_le(bytes, oid, 1);
                put_oid(oid + 4, 2); put_oid(oid + 4 + oid, 3);
                write_u64_le(bytes, value_offset, keys[0].first);
            }
            for (size_t i = 0; i < keys.size(); ++i) {
                const size_t leaf = branched ? (i == 0 ? 1 : 2) : 0;
                const size_t slot = branched && i != 0 ? i - 1 : i;
                write_u32_le(bytes, leaf * page_size + oid, branched ? (leaf == 1 ? 1 : 2) : 3);
                put_oid(leaf * page_size + header + slot * oid, keys[i].second);
                write_u64_le(bytes, leaf * page_size + value_offset + slot * 8, keys[i].first);
            }
            const size_t tail = pages * page_size;
            bytes[tail] = 8; write_u32_le(bytes, tail + 2, 1);
            write_u32_le(bytes, tail + 6, branched ? 2 : 1);
            if (version == 1) write_u32_le(bytes, tail + 10, 3);
            else write_u64_le(bytes, tail + 10, 3);
            write_u32_le(bytes, bytes.size() - 4, version);
            const auto path = fs::temp_directory_path() / "fast_gdb_coarse_levels.spx";
            { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }
            GdbSpatialIndexParser parser(path.string());
            ASSERT_TRUE(parser.parse());
            bool succeeded = false;
            const auto fids = parser.query_bbox(coordinate, coordinate, coordinate, coordinate,
                0, 0, 1, {1, 4, coarse}, 2, merge, &succeeded);
            EXPECT_TRUE(succeeded);
            EXPECT_EQ(fids, (std::vector<RowSlot>{0, 1, 2})) << coordinate << " version=" << version << " branch=" << branched;
            fs::remove(path);
        }
    }
}
