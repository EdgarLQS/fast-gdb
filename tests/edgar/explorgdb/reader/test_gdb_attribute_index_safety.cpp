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

} // namespace

TEST(AttributeIndexSafetyTest, ValidSingleLeafStillParses) {
    const std::string path = build_single_value_atx("valid", 0U, 1U, 1U);
    GdbAttributeIndexParser parser(path);
    ASSERT_TRUE(parser.parse());
    ASSERT_EQ(parser.all_entries().size(), 1U);
    EXPECT_EQ(parser.query_double(42.0, AttrOp::Eq),
              (std::vector<uint32_t>{0}));
    fs::remove(path);
}

TEST(AttributeIndexSafetyTest, DirectNumericQueryMatchesMaterializedQuery) {
    const std::string path =
        build_single_value_atx("direct_valid", 0U, 1U, 1U);

    GdbAttributeIndexParser materialized(path);
    ASSERT_TRUE(materialized.parse());
    const std::vector<uint32_t> expected =
        materialized.query_double(42.0, AttrOp::Eq);

    GdbAttributeIndexParser direct(path);
    AttributeIndexQueryMetrics metrics;
    std::vector<uint32_t> actual;
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

TEST(AttributeIndexSafetyTest, TrailerCountMismatchFailsClosed) {
    const std::string path =
        build_single_value_atx("count_mismatch", 0U, 1U, 2U);
    GdbAttributeIndexParser parser(path);
    EXPECT_FALSE(parser.parse());
    EXPECT_TRUE(parser.all_entries().empty());

    std::vector<uint32_t> direct_result{99U};
    EXPECT_FALSE(parser.query_double_direct(
        42.0, AttrOp::Eq, 1U, direct_result));
    EXPECT_EQ(direct_result, (std::vector<uint32_t>{99U}));
    fs::remove(path);
}

TEST(AttributeIndexSafetyTest, CyclicLeafChainFailsClosed) {
    const std::string path = build_single_value_atx("cycle", 1U, 1U, 1U);
    GdbAttributeIndexParser parser(path);
    EXPECT_FALSE(parser.parse());
    EXPECT_TRUE(parser.all_entries().empty());

    std::vector<uint32_t> direct_result{77U};
    EXPECT_FALSE(parser.query_double_direct(
        42.0, AttrOp::Eq, 1U, direct_result));
    EXPECT_EQ(direct_result, (std::vector<uint32_t>{77U}));
    fs::remove(path);
}

TEST(AttributeIndexSafetyTest, ZeroFidFailsClosed) {
    const std::string path = build_single_value_atx("zero_fid", 0U, 0U, 1U);
    GdbAttributeIndexParser parser(path);
    EXPECT_FALSE(parser.parse());
    EXPECT_TRUE(parser.all_entries().empty());

    std::vector<uint32_t> direct_result{55U};
    EXPECT_FALSE(parser.query_double_direct(
        42.0, AttrOp::Eq, 1U, direct_result));
    EXPECT_EQ(direct_result, (std::vector<uint32_t>{55U}));
    fs::remove(path);
}
