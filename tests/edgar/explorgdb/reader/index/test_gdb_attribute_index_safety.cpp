// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include "gdb_attribute_index.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace explorgdb;
namespace fs = std::filesystem;

namespace {

std::string safety_path(const std::string& suffix) {
    return (fs::temp_directory_path() /
            ("fast_gdb_attribute_index_safety_" + suffix + ".atx"))
        .string();
}

void write_u32_le(std::vector<uint8_t>& bytes,
                  size_t offset,
                  uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<uint8_t>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<uint8_t>((value >> 24U) & 0xffU);
}

void write_u64_le(std::vector<uint8_t>& bytes,
                  size_t offset,
                  uint64_t value) {
    for (size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<uint8_t>(
            (value >> (index * 8U)) & 0xffU);
    }
}

void write_i64_le(std::vector<uint8_t>& bytes,
                  size_t offset,
                  int64_t value) {
    const uint64_t raw = static_cast<uint64_t>(value);
    for (size_t index = 0; index < sizeof(raw); ++index) {
        bytes[offset + index] = static_cast<uint8_t>(
            (raw >> (index * 8U)) & 0xffU);
    }
}

std::string build_single_value_atx(const std::string& suffix,
                                   uint32_t next_page,
                                   uint32_t fid,
                                   uint32_t trailer_count) {
    const std::string path = safety_path(suffix);
    std::vector<uint8_t> page(GdbAttributeIndexParser::kPageSize, 0);
    write_u32_le(page, 0, next_page);
    write_u32_le(page, 4, 1U);
    write_u32_le(page, 12, fid);

    constexpr size_t kNumericCapacity =
        (GdbAttributeIndexParser::kPageSize - 12U) / (4U + 8U);
    const size_t value_offset = 12U + kNumericCapacity * 4U;
    const double value = 42.0;
    std::memcpy(page.data() + value_offset, &value, sizeof(value));

    std::vector<uint8_t> trailer(GdbAttributeIndexParser::kTrailerSize, 0);
    trailer[0] = 8U;
    trailer[1] = 0x40U;
    write_u32_le(trailer, 18U, 1U);
    write_u32_le(trailer, 2, 1U);
    write_u32_le(trailer, 6, 1U);
    write_u32_le(trailer, 10, trailer_count);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(page.data()),
                 static_cast<std::streamsize>(page.size()));
    output.write(reinterpret_cast<const char*>(trailer.data()),
                 static_cast<std::streamsize>(trailer.size()));
    return path;
}

std::string build_int64_values_atx(const std::string& suffix) {
    const std::string path = safety_path(suffix);
    std::vector<uint8_t> page(GdbAttributeIndexParser::kPageSize, 0);
    write_u32_le(page, 0, 0U);
    write_u32_le(page, 4, 4U);
    write_u32_le(page, 12, 1U);
    write_u32_le(page, 16, 2U);
    write_u32_le(page, 20, 3U);
    write_u32_le(page, 24, 4U);

    constexpr size_t kInt64Capacity =
        (GdbAttributeIndexParser::kPageSize - 12U) / (4U + 8U);
    const size_t value_offset = 12U + kInt64Capacity * 4U;
    write_i64_le(page, value_offset, -9007199254740993LL);
    write_i64_le(page, value_offset + 8U, -9007199254740992LL);
    write_i64_le(page, value_offset + 16U, 9007199254740992LL);
    write_i64_le(page, value_offset + 24U, 9007199254740993LL);

    std::vector<uint8_t> trailer(GdbAttributeIndexParser::kTrailerSize, 0);
    trailer[0] = 8U;
    trailer[1] = 0x40U;
    write_u32_le(trailer, 18U, 1U);
    write_u32_le(trailer, 2U, 1U);
    write_u32_le(trailer, 6U, 1U);
    write_u32_le(trailer, 10U, 4U);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(page.data()),
                 static_cast<std::streamsize>(page.size()));
    output.write(reinterpret_cast<const char*>(trailer.data()),
                 static_cast<std::streamsize>(trailer.size()));
    return path;
}

std::string build_two_leaf_numeric_atx(const std::string& suffix,
                                       bool corrupt_child = false) {
    const std::string path = safety_path(suffix);
    constexpr size_t kPageSize = GdbAttributeIndexParser::kPageSize;
    constexpr size_t kNumericCapacity = (kPageSize - 12U) / 12U;
    constexpr size_t kValuesOffset = 12U + kNumericCapacity * 4U;

    std::vector<uint8_t> root(kPageSize, 0U);
    write_u32_le(root, 4U, 2U);
    write_u32_le(root, 8U, 2U);
    write_u32_le(root, 12U, corrupt_child ? 99U : 3U);
    const double separators[] = {3.0, 6.0};
    std::memcpy(root.data() + kValuesOffset, separators, sizeof(separators));

    std::vector<uint8_t> first_leaf(kPageSize, 0U);
    write_u32_le(first_leaf, 0U, 3U);
    write_u32_le(first_leaf, 4U, 3U);
    for (uint32_t index = 0U; index < 3U; ++index) {
        write_u32_le(first_leaf, 12U + index * 4U, index + 1U);
        const double value = static_cast<double>(index + 1U);
        std::memcpy(first_leaf.data() + kValuesOffset + index * 8U,
                    &value, sizeof(value));
    }

    std::vector<uint8_t> second_leaf(kPageSize, 0U);
    write_u32_le(second_leaf, 4U, 3U);
    for (uint32_t index = 0U; index < 3U; ++index) {
        write_u32_le(second_leaf, 12U + index * 4U, index + 4U);
        const double value = static_cast<double>(index + 4U);
        std::memcpy(second_leaf.data() + kValuesOffset + index * 8U,
                    &value, sizeof(value));
    }

    std::vector<uint8_t> trailer(GdbAttributeIndexParser::kTrailerSize, 0U);
    trailer[0] = 8U;
    trailer[1] = 0x40U;
    write_u32_le(trailer, 18U, 1U);
    write_u32_le(trailer, 2U, 1U);
    write_u32_le(trailer, 6U, 2U);
    write_u32_le(trailer, 10U, 6U);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(root.data()),
                 static_cast<std::streamsize>(root.size()));
    output.write(reinterpret_cast<const char*>(first_leaf.data()),
                 static_cast<std::streamsize>(first_leaf.size()));
    output.write(reinterpret_cast<const char*>(second_leaf.data()),
                 static_cast<std::streamsize>(second_leaf.size()));
    output.write(reinterpret_cast<const char*>(trailer.data()),
                 static_cast<std::streamsize>(trailer.size()));
    return path;
}

std::string build_v2_high_object_id_atx(const std::string& suffix) {
    constexpr size_t kPageSize = 65536U;
    constexpr size_t kTrailerSize = 30U;
    constexpr size_t kObjectIdSize = 8U;
    constexpr size_t kLeafHeaderSize = 20U;
    constexpr size_t kNonLeafHeaderSize = 12U;
    constexpr size_t kCapacity =
        (kPageSize - kLeafHeaderSize) / (kObjectIdSize + 8U);
    constexpr size_t kValuesOffset =
        kLeafHeaderSize + kCapacity * kObjectIdSize;
    constexpr uint64_t kHighObjectId = 0x100000005ULL;
    const std::string path = safety_path(suffix);

    std::vector<uint8_t> root(kPageSize, 0U);
    write_u32_le(root, kObjectIdSize, 1U);
    write_u64_le(root, kNonLeafHeaderSize, 2U);
    const double separator = 42.0;
    std::memcpy(root.data() + kValuesOffset, &separator, sizeof(separator));

    std::vector<uint8_t> leaf(kPageSize, 0U);
    write_u32_le(leaf, kObjectIdSize, 1U);
    write_u64_le(leaf, kLeafHeaderSize, kHighObjectId);
    const double indexed_value = 42.0;
    std::memcpy(leaf.data() + kValuesOffset, &indexed_value,
                sizeof(indexed_value));

    std::vector<uint8_t> trailer(kTrailerSize, 0U);
    trailer[0] = 8U;
    trailer[1] = 0x40U;
    write_u32_le(trailer, 2U, 1U);
    write_u32_le(trailer, 6U, 2U);
    write_u64_le(trailer, 10U, 1U);
    write_u32_le(trailer, 26U, 2U);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(root.data()),
                 static_cast<std::streamsize>(root.size()));
    output.write(reinterpret_cast<const char*>(leaf.data()),
                 static_cast<std::streamsize>(leaf.size()));
    output.write(reinterpret_cast<const char*>(trailer.data()),
                 static_cast<std::streamsize>(trailer.size()));
    return path;
}

} // namespace

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AttributeIndexSafetyTest, ValidSingleLeafStillParses) {
    const std::string path = build_single_value_atx("valid", 0U, 1U, 1U);
    GdbAttributeIndexParser parser(path);
    ASSERT_TRUE(parser.parse());
    ASSERT_EQ(parser.all_entries().size(), 1U);
    EXPECT_EQ(parser.query_double(42.0, AttrOp::Eq),
              (std::vector<RowSlot>{0}));
    fs::remove(path);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AttributeIndexSafetyTest, DirectNumericQueryMatchesMaterializedQuery) {
    const std::string path =
        build_single_value_atx("direct_valid", 0U, 1U, 1U);

    GdbAttributeIndexParser materialized(path);
    ASSERT_TRUE(materialized.parse());
    const std::vector<RowSlot> expected =
        materialized.query_double(42.0, AttrOp::Eq);

    GdbAttributeIndexParser direct(path);
    AttributeIndexQueryMetrics metrics;
    std::vector<RowSlot> actual;
    ASSERT_TRUE(direct.query_double_direct(
        42.0, AttrOp::Eq, 1U, actual, &metrics));
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(metrics.page_count, 1U);
    EXPECT_EQ(metrics.pages_visited, 1U);
    EXPECT_EQ(metrics.entries_scanned, 1U);
    EXPECT_EQ(metrics.candidate_count, 1U);
    EXPECT_GE(metrics.file_load_ms, 0.0);
    EXPECT_GE(metrics.leaf_scan_ms, 0.0);
    EXPECT_GE(metrics.total_ms, 0.0);
    fs::remove(path);
}

TEST(AttributeIndexSafetyTest, DirectInt64QueryPreservesAdjacentLargeValues) {
    const std::string path = build_int64_values_atx("int64_adjacent");
    GdbAttributeIndexParser parser(path);
    AttributeIndexQueryMetrics metrics;
    std::vector<RowSlot> actual;

    const bool found_positive = parser.query_int64_direct(
        9007199254740993LL, AttrOp::Eq, 4U, actual, &metrics);
    ASSERT_TRUE(found_positive) << "pages=" << metrics.page_count
                                << " visited=" << metrics.pages_visited
                                << " entries=" << metrics.entries_scanned;
    EXPECT_EQ(actual, (std::vector<RowSlot>{3U}));
    EXPECT_EQ(metrics.entries_scanned, 4U);

    ASSERT_TRUE(parser.query_int64_direct(
        9007199254740993LL, AttrOp::Lt, 4U, actual));
    EXPECT_EQ(actual, (std::vector<RowSlot>{0U, 1U, 2U}));

    ASSERT_TRUE(parser.query_int64_direct(
        -9007199254740993LL, AttrOp::Eq, 4U, actual));
    EXPECT_EQ(actual, (std::vector<RowSlot>{0U}));
    fs::remove(path);
}

TEST(AttributeIndexSafetyTest, VersionTwoParsesAndQueries64BitObjectIds) {
    constexpr RowSlot kExpectedRowSlot = 0x100000004ULL;
    const std::string path = build_v2_high_object_id_atx("v2_high_oid");

    GdbAttributeIndexParser materialized(path);
    ASSERT_TRUE(materialized.parse());
    EXPECT_EQ(materialized.trailer().version, 2U);
    EXPECT_EQ(materialized.trailer().page_size, 65536U);
    EXPECT_EQ(materialized.trailer().object_id_size, 8U);
    ASSERT_EQ(materialized.all_entries().size(), 1U);
    EXPECT_EQ(materialized.all_entries()[0].fid, 0x100000005ULL);
    EXPECT_EQ(materialized.query_double(42.0, AttrOp::Eq),
              (std::vector<RowSlot>{kExpectedRowSlot}));

    GdbAttributeIndexParser direct(path);
    std::vector<RowSlot> actual;
    ASSERT_TRUE(direct.query_double_direct(
        42.0, AttrOp::Eq, 0U, actual));
    EXPECT_EQ(actual, (std::vector<RowSlot>{kExpectedRowSlot}));
    fs::remove(path);
}

TEST(AttributeIndexSafetyTest, DirectQueryNavigatesAndStopsAtRange) {
    const std::string path = build_two_leaf_numeric_atx("range_direct");
    GdbAttributeIndexParser materialized(path);
    ASSERT_TRUE(materialized.parse());

    GdbAttributeIndexParser direct(path);
    AttributeIndexQueryMetrics metrics;
    std::vector<RowSlot> actual;
    const bool found_five = direct.query_double_direct(
        5.0, AttrOp::Eq, 6U, actual, &metrics);
    ASSERT_TRUE(found_five) << "pages=" << metrics.page_count
                            << " visited=" << metrics.pages_visited
                            << " entries=" << metrics.entries_scanned;
    EXPECT_EQ(actual, (std::vector<RowSlot>{4U}));
    EXPECT_EQ(metrics.page_count, 3U);
    EXPECT_EQ(metrics.pages_visited, 2U);
    EXPECT_EQ(metrics.entries_scanned, 3U);
    EXPECT_EQ(actual, materialized.query_double(5.0, AttrOp::Eq));

    ASSERT_TRUE(direct.query_double_direct(
        3.0, AttrOp::Lt, 6U, actual, &metrics));
    EXPECT_EQ(actual, (std::vector<RowSlot>{0U, 1U}));
    EXPECT_EQ(metrics.pages_visited, 2U);
    EXPECT_EQ(metrics.entries_scanned, 3U);

    ASSERT_TRUE(direct.query_double_direct(
        3.0, AttrOp::Gt, 6U, actual, &metrics));
    EXPECT_EQ(actual, (std::vector<RowSlot>{3U, 4U, 5U}));
    EXPECT_EQ(metrics.pages_visited, 2U);
    EXPECT_EQ(metrics.entries_scanned, 3U);

    ASSERT_TRUE(direct.query_double_direct(
        3.0, AttrOp::Ge, 6U, actual, &metrics));
    EXPECT_EQ(actual, (std::vector<RowSlot>{2U, 3U, 4U, 5U}));
    EXPECT_EQ(metrics.entries_scanned, 6U);

    ASSERT_TRUE(direct.query_double_direct(
        3.0, AttrOp::Le, 6U, actual, &metrics));
    EXPECT_EQ(actual, (std::vector<RowSlot>{0U, 1U, 2U}));
    EXPECT_EQ(metrics.pages_visited, 3U);
    EXPECT_EQ(metrics.entries_scanned, 4U);
    EXPECT_EQ(actual, materialized.query_double(3.0, AttrOp::Le));

    ASSERT_TRUE(direct.query_double_direct(
        3.0, AttrOp::Ne, 6U, actual, &metrics));
    EXPECT_EQ(actual, (std::vector<RowSlot>{0U, 1U, 3U, 4U, 5U}));
    EXPECT_EQ(metrics.pages_visited, 3U);
    EXPECT_EQ(metrics.entries_scanned, 6U);
    EXPECT_EQ(actual, materialized.query_double(3.0, AttrOp::Ne));
    fs::remove(path);
}

TEST(AttributeIndexSafetyTest, InvalidUnselectedBranchFailsClosed) {
    const std::string path = build_two_leaf_numeric_atx(
        "invalid_branch", true);
    GdbAttributeIndexParser parser(path);
    std::vector<RowSlot> result{99U};
    EXPECT_FALSE(parser.query_double_direct(
        5.0, AttrOp::Eq, 6U, result));
    EXPECT_EQ(result, (std::vector<RowSlot>{99U}));
    fs::remove(path);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AttributeIndexSafetyTest, TrailerCountMismatchFailsClosed) {
    const std::string path =
        build_single_value_atx("count_mismatch", 0U, 1U, 2U);
    GdbAttributeIndexParser parser(path);
    EXPECT_FALSE(parser.parse());
    EXPECT_TRUE(parser.all_entries().empty());

    std::vector<RowSlot> direct_result{99U};
    EXPECT_FALSE(parser.query_double_direct(
        42.0, AttrOp::Eq, 1U, direct_result));
    EXPECT_EQ(direct_result, (std::vector<RowSlot>{99U}));
    fs::remove(path);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AttributeIndexSafetyTest, CyclicLeafChainFailsClosed) {
    const std::string path = build_single_value_atx("cycle", 1U, 1U, 1U);
    GdbAttributeIndexParser parser(path);
    EXPECT_FALSE(parser.parse());
    EXPECT_TRUE(parser.all_entries().empty());

    std::vector<RowSlot> direct_result{77U};
    EXPECT_FALSE(parser.query_double_direct(
        42.0, AttrOp::Eq, 1U, direct_result));
    EXPECT_EQ(direct_result, (std::vector<RowSlot>{77U}));
    fs::remove(path);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AttributeIndexSafetyTest, ZeroFidFailsClosed) {
    const std::string path = build_single_value_atx("zero_fid", 0U, 0U, 1U);
    GdbAttributeIndexParser parser(path);
    EXPECT_FALSE(parser.parse());
    EXPECT_TRUE(parser.all_entries().empty());

    std::vector<RowSlot> direct_result{55U};
    EXPECT_FALSE(parser.query_double_direct(
        42.0, AttrOp::Eq, 1U, direct_result));
    EXPECT_EQ(direct_result, (std::vector<RowSlot>{55U}));
    fs::remove(path);
}
