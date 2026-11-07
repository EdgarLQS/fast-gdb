// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include "gdb_attribute_index.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#include <utility>

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

TEST(AttributeIndexSafetyTest, SmallIntegerKeysKeepTheirSignedOrder) {
    const std::string path = safety_path("signed_int16");
    std::vector<uint8_t> bytes(GdbAttributeIndexParser::kPageSize +
                               GdbAttributeIndexParser::kTrailerSize, 0);
    constexpr size_t capacity = (GdbAttributeIndexParser::kPageSize - 12) / 6;
    constexpr size_t values_offset = 12 + capacity * 4;
    const int16_t keys[] = {-32768, -13, 0, 32767};
    write_u32_le(bytes, 4, 4);
    for (size_t i = 0; i < 4; ++i) {
        write_u32_le(bytes, 12 + 4 * i, static_cast<uint32_t>(i + 1));
        const uint16_t raw = static_cast<uint16_t>(keys[i]);
        bytes[values_offset + 2 * i] = static_cast<uint8_t>(raw);
        bytes[values_offset + 2 * i + 1] = static_cast<uint8_t>(raw >> 8);
    }
    const size_t trailer = GdbAttributeIndexParser::kPageSize;
    bytes[trailer] = 2;
    bytes[trailer + 1] = 0x40;
    write_u32_le(bytes, trailer + 2, 1);
    write_u32_le(bytes, trailer + 6, 1);
    write_u32_le(bytes, trailer + 10, 4);
    write_u32_le(bytes, trailer + 18, 1);
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        ASSERT_TRUE(output.good());
    }
    GdbAttributeIndexParser parser(path);
    ASSERT_TRUE(parser.parse());
    const std::vector<std::pair<AttrOp, std::vector<RowSlot>>> expected = {
        {AttrOp::Eq, {1}}, {AttrOp::Ne, {0, 2, 3}},
        {AttrOp::Lt, {0}}, {AttrOp::Le, {0, 1}},
        {AttrOp::Gt, {2, 3}}, {AttrOp::Ge, {1, 2, 3}}};
    GdbAttributeIndexParser direct(path);
    for (const auto& item : expected) {
        EXPECT_EQ(parser.query_double(-13, item.first), item.second);
        std::vector<RowSlot> result;
        ASSERT_TRUE(direct.query_double_direct(-13, item.first, 4, result));
        EXPECT_EQ(result, item.second);
    }
    std::vector<RowSlot> boundary;
    ASSERT_TRUE(parser.query_double_direct(-32768, AttrOp::Eq, 4, boundary));
    EXPECT_EQ(boundary, (std::vector<RowSlot>{0}));
    ASSERT_TRUE(parser.query_double_direct(32767, AttrOp::Eq, 4, boundary));
    EXPECT_EQ(boundary, (std::vector<RowSlot>{3}));
    fs::remove(path);
}

TEST(AttributeIndexSafetyTest, Float32SchemaSelectsFloatingPointKeys) {
    const std::string path = safety_path("float32_schema");
    std::vector<uint8_t> bytes(GdbAttributeIndexParser::kPageSize +
                               GdbAttributeIndexParser::kTrailerSize, 0);
    constexpr size_t capacity = (GdbAttributeIndexParser::kPageSize - 12) / 8;
    constexpr size_t values_offset = 12 + capacity * 4;
    const float keys[] = {-1.5f, 0.125f, 1.5f, 100.25f};
    write_u32_le(bytes, 4, 4);
    for (size_t i = 0; i < 4; ++i) {
        write_u32_le(bytes, 12 + 4 * i, static_cast<uint32_t>(i + 1));
        std::memcpy(bytes.data() + values_offset + 4 * i, &keys[i], 4);
    }
    const size_t trailer = GdbAttributeIndexParser::kPageSize;
    bytes[trailer] = 4;
    bytes[trailer + 1] = 0x40;
    write_u32_le(bytes, trailer + 2, 1);
    write_u32_le(bytes, trailer + 6, 1);
    write_u32_le(bytes, trailer + 10, 4);
    write_u32_le(bytes, trailer + 18, 1);
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        ASSERT_TRUE(output.good());
    }
    GdbAttributeIndexParser materialized(path, FieldType::Float32);
    GdbAttributeIndexParser direct(path, FieldType::Float32);
    ASSERT_TRUE(materialized.parse());
    const std::vector<std::pair<AttrOp, std::vector<RowSlot>>> expected = {
        {AttrOp::Eq, {2}}, {AttrOp::Ne, {0, 1, 3}},
        {AttrOp::Lt, {0, 1}}, {AttrOp::Le, {0, 1, 2}},
        {AttrOp::Gt, {3}}, {AttrOp::Ge, {2, 3}}};
    for (const auto& item : expected) {
        EXPECT_EQ(materialized.query_double(1.5, item.first), item.second);
        std::vector<RowSlot> result;
        ASSERT_TRUE(direct.query_double_direct(1.5, item.first, 4, result));
        EXPECT_EQ(result, item.second);
    }
    fs::remove(path);
}

TEST(AttributeIndexSafetyTest, TemporalMillisecondsKeepEqualKeysAcrossLeafBoundaries) {
    constexpr size_t page_size = GdbAttributeIndexParser::kPageSize;
    constexpr size_t capacity = (page_size - 12U) / 12U;
    constexpr size_t values_offset = 12U + capacity * 4U;
    for (double base : {45306.0, -0.5}) {
        const auto path = build_two_leaf_numeric_atx("temporal_millisecond_plateau");
        const double keys[] = {base + 122.0 / 86400000.0, base + 122.6 / 86400000.0,
                               base + 123.0 / 86400000.0, base + 123.4 / 86400000.0,
                               base + 124.0 / 86400000.0, 2958465.0};
        {
            std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
            const double separators[] = {keys[2], keys[5]};
            file.seekp(values_offset);
            file.write(reinterpret_cast<const char*>(separators), sizeof(separators));
            file.seekp(page_size + values_offset);
            file.write(reinterpret_cast<const char*>(keys), 3 * sizeof(double));
            file.seekp(2 * page_size + values_offset);
            file.write(reinterpret_cast<const char*>(keys + 3), 3 * sizeof(double));
            ASSERT_TRUE(file.good());
        }
        const int64_t target = base == 45306.0 ? 1705276800123LL : -2209204799877LL;
        const std::vector<std::pair<AttrOp, std::vector<RowSlot>>> expected = {
            {AttrOp::Eq, {1, 2, 3}}, {AttrOp::Ne, {0, 4, 5}},
            {AttrOp::Lt, {0}}, {AttrOp::Le, {0, 1, 2, 3}},
            {AttrOp::Gt, {4, 5}}, {AttrOp::Ge, {1, 2, 3, 4, 5}}};
        GdbAttributeIndexParser index(path, FieldType::DateTime);
        for (const auto& item : expected) {
            std::vector<RowSlot> result;
            ASSERT_TRUE(index.query_temporal_direct(target, item.first, 6, result));
            EXPECT_EQ(result, item.second) << "base=" << base;
        }
        std::vector<RowSlot> raw;
        ASSERT_TRUE(index.query_double_direct(keys[2], AttrOp::Eq, 6, raw));
        EXPECT_EQ(raw, (std::vector<RowSlot>{2}));
        {
            std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
            const double invalid = std::numeric_limits<double>::quiet_NaN();
            file.seekp(page_size + values_offset);
            file.write(reinterpret_cast<const char*>(&invalid), sizeof(invalid));
            ASSERT_TRUE(file.good());
        }
        std::vector<RowSlot> unchanged{99};
        EXPECT_FALSE(index.query_temporal_direct(target, AttrOp::Eq, 6, unchanged));
        EXPECT_EQ(unchanged, (std::vector<RowSlot>{99}));
        GdbAttributeIndexParser untyped(path);
        EXPECT_FALSE(untyped.query_temporal_direct(target, AttrOp::Eq, 6, unchanged));
        fs::remove(path);
    }
}

TEST(AttributeIndexSafetyTest, UuidAsciiKeysUseTypedCanonicalOrderAcrossPages) {
    for (uint32_t version : {1U, 2U}) {
        SCOPED_TRACE(version);
        const auto path = safety_path("uuid_ascii_two_leaf_" + std::to_string(version));
        const size_t page_size = version == 1 ? GdbAttributeIndexParser::kPageSize
                                              : GdbAttributeIndexParser::kV2PageSize;
        const size_t trailer_size = version == 1 ? GdbAttributeIndexParser::kTrailerSize
                                                 : GdbAttributeIndexParser::kV2TrailerSize;
        const size_t oid_size = version == 1 ? 4 : 8;
        const size_t leaf_header = version == 1 ? 12 : 20;
        const size_t branch_header = version == 1 ? 8 : 12;
        const size_t capacity = (page_size - leaf_header) / (oid_size + 38);
        const size_t values_offset = leaf_header + capacity * oid_size;
        const RowSlot fid_base = version == 1 ? 0 : RowSlot{1} << 40;
        const std::string keys[] = {
            "{00000000-0000-0000-0000-000000000000}",
            "{01234567-89AB-CDEF-0123-456789ABCDEF}",
            "{12345678-9ABC-DEF0-1234-567890ABCDEF}",
            "{12345678-9ABC-DEF0-1234-567890ABCDEF}",
            "{ABCDEF01-2345-6789-ABCD-EF0123456789}",
            "{FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF}"};
        std::vector<uint8_t> bytes(3 * page_size + trailer_size, 0);
        const auto write_oid = [&](size_t offset, uint64_t value) {
            if (version == 1) write_u32_le(bytes, offset, static_cast<uint32_t>(value));
            else write_u64_le(bytes, offset, value);
        };
        write_u32_le(bytes, oid_size, 2);
        write_oid(branch_header, 2);
        write_oid(branch_header + oid_size, 3);
        std::memcpy(bytes.data() + values_offset, keys[2].data(), 38);
        std::memcpy(bytes.data() + values_offset + 38, keys[5].data(), 38);
        write_oid(page_size, 3);
        for (size_t leaf = 0; leaf < 2; ++leaf) {
            const size_t offset = (leaf + 1) * page_size;
            write_u32_le(bytes, offset + oid_size, 3);
            for (size_t i = 0; i < 3; ++i) {
                write_oid(offset + leaf_header + oid_size * i, fid_base + leaf * 3 + i + 1);
                std::memcpy(bytes.data() + offset + values_offset + i * 38,
                            keys[leaf * 3 + i].data(), 38);
            }
        }
        const size_t trailer = 3 * page_size;
        bytes[trailer] = 38; // GUID keys use ASCII and neither string nor numeric flags.
        write_u32_le(bytes, trailer + 2, 1);
        write_u32_le(bytes, trailer + 6, 2);
        write_oid(trailer + 10, 6);
        write_u32_le(bytes, trailer + trailer_size - 4, version);
        const auto save = [&]() {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return file.good();
        };
        ASSERT_TRUE(save());
        std::vector<std::pair<AttrOp, std::vector<RowSlot>>> expected = {
            {AttrOp::Eq, {2, 3}}, {AttrOp::Ne, {0, 1, 4, 5}},
            {AttrOp::Lt, {0, 1}}, {AttrOp::Le, {0, 1, 2, 3}},
            {AttrOp::Gt, {4, 5}}, {AttrOp::Ge, {2, 3, 4, 5}}};
        for (auto& comparison : expected)
            for (auto& fid : comparison.second) fid += fid_base;
        for (FieldType type : {FieldType::UUID_1, FieldType::UUID_2}) {
            GdbAttributeIndexParser materialized(path, type), direct(path, type);
            ASSERT_TRUE(materialized.parse());
            EXPECT_TRUE(materialized.query_double(0.0, AttrOp::Eq).empty());
            for (const auto& literal : {keys[2], std::string("12345678-9abc-def0-1234-567890abcdef")}) {
                for (const auto& item : expected) {
                    EXPECT_EQ(materialized.query_string(literal, item.first), item.second);
                    std::vector<RowSlot> result;
                    AttributeIndexQueryMetrics metrics;
                    ASSERT_TRUE(direct.query_string_direct(literal, item.first, fid_base + 6, result, &metrics));
                    EXPECT_EQ(result, item.second);
                    EXPECT_EQ(metrics.page_count, 3U);
                }
            }
            std::vector<RowSlot> unchanged{99};
            EXPECT_FALSE(direct.query_string_direct("bad-uuid", AttrOp::Eq, 6, unchanged));
            EXPECT_EQ(unchanged, (std::vector<RowSlot>{99}));
        }
        // An unknown physical representation must not be published as numeric zero.
        GdbAttributeIndexParser untyped(path);
        EXPECT_FALSE(untyped.parse());
        bytes[page_size + values_offset] = '[';
        ASSERT_TRUE(save());
        GdbAttributeIndexParser damaged(path, FieldType::UUID_1);
        EXPECT_FALSE(damaged.parse());
        std::vector<RowSlot> unchanged{99};
        EXPECT_FALSE(damaged.query_string_direct(keys[2], AttrOp::Eq, fid_base + 6, unchanged));
        EXPECT_EQ(unchanged, (std::vector<RowSlot>{99}));
        bytes.assign(trailer_size, 0);
        bytes[0] = 38;
        write_u32_le(bytes, 2, 1);
        write_u32_le(bytes, trailer_size - 4, version);
        ASSERT_TRUE(save());
        GdbAttributeIndexParser empty(path, FieldType::UUID_2);
        ASSERT_TRUE(empty.parse());
        ASSERT_TRUE(empty.query_string_direct(keys[2], AttrOp::Eq, 6, unchanged));
        EXPECT_TRUE(unchanged.empty());
        unchanged = {99};
        EXPECT_FALSE(empty.query_string_direct("bad-uuid", AttrOp::Eq, 6, unchanged));
        EXPECT_EQ(unchanged, (std::vector<RowSlot>{99}));
        fs::remove(path);
    }
}
