// tests/edgar/explorgdb/reader/geometry/test_wkb_to_wkt.cpp
// GeometryValue::to_wkt() 的纯 C++ 正确性与损坏输入测试。

#include <gtest/gtest.h>

#include "geometry_model.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace explorgdb;

namespace {

class WkbBuilder {
public:
    explicit WkbBuilder(bool little_endian = true)
        : little_endian_(little_endian) {}

    WkbBuilder& header(uint32_t type) {
        bytes_.push_back(little_endian_ ? 1U : 0U);
        u32(type);
        return *this;
    }

    WkbBuilder& u32(uint32_t value) {
        if (little_endian_) {
            for (int shift = 0; shift < 32; shift += 8) {
                bytes_.push_back(
                    static_cast<uint8_t>((value >> shift) & 0xffU));
            }
        } else {
            for (int shift = 24; shift >= 0; shift -= 8) {
                bytes_.push_back(
                    static_cast<uint8_t>((value >> shift) & 0xffU));
            }
        }
        return *this;
    }

    WkbBuilder& f64(double value) {
        uint64_t raw = 0;
        std::memcpy(&raw, &value, sizeof(value));
        if (little_endian_) {
            for (int shift = 0; shift < 64; shift += 8) {
                bytes_.push_back(
                    static_cast<uint8_t>((raw >> shift) & 0xffU));
            }
        } else {
            for (int shift = 56; shift >= 0; shift -= 8) {
                bytes_.push_back(
                    static_cast<uint8_t>((raw >> shift) & 0xffU));
            }
        }
        return *this;
    }

    WkbBuilder& append(const std::vector<uint8_t>& nested) {
        bytes_.insert(bytes_.end(), nested.begin(), nested.end());
        return *this;
    }

    std::vector<uint8_t> take() { return std::move(bytes_); }

private:
    bool little_endian_ = true;
    std::vector<uint8_t> bytes_;
};

GeometryValue value(std::vector<uint8_t> bytes) {
    GeometryValue result;
    result.wkb = std::move(bytes);
    return result;
}

std::vector<uint8_t> point(double x, double y,
                           bool little_endian = true) {
    return WkbBuilder(little_endian)
        .header(1U).f64(x).f64(y).take();
}

std::vector<uint8_t> point_zm(double x, double y, double z, double m) {
    return WkbBuilder().header(3001U)
        .f64(x).f64(y).f64(z).f64(m).take();
}

std::vector<uint8_t> line(const std::vector<double>& xy) {
    WkbBuilder builder;
    builder.header(2U).u32(static_cast<uint32_t>(xy.size() / 2U));
    for (double ordinate : xy) builder.f64(ordinate);
    return builder.take();
}

std::vector<uint8_t> polygon() {
    return WkbBuilder().header(3U).u32(2U)
        .u32(5U)
        .f64(0).f64(0)
        .f64(10).f64(0)
        .f64(10).f64(10)
        .f64(0).f64(10)
        .f64(0).f64(0)
        .u32(5U)
        .f64(2).f64(2)
        .f64(2).f64(4)
        .f64(4).f64(4)
        .f64(4).f64(2)
        .f64(2).f64(2)
        .take();
}

} // namespace

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryValueToWkt, ConvertsPointAndBigEndianPoint) {
    const auto little = value(point(1.25, -2.5)).to_wkt();
    ASSERT_TRUE(little.has_value());
    EXPECT_EQ(*little, "POINT (1.25 -2.5)");

    const auto big = value(point(3.5, 4.25, false)).to_wkt();
    ASSERT_TRUE(big.has_value());
    EXPECT_EQ(*big, "POINT (3.5 4.25)");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryValueToWkt, ConvertsDimensionsAndEmptyPoint) {
    const auto zm = value(point_zm(1, 2, 3, 4)).to_wkt();
    ASSERT_TRUE(zm.has_value());
    EXPECT_EQ(*zm, "POINT ZM (1 2 3 4)");

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto empty = value(WkbBuilder().header(1001U)
                                 .f64(nan).f64(nan).f64(nan).take())
                           .to_wkt();
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(*empty, "POINT Z EMPTY");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryValueToWkt, ConvertsLinePolygonAndCollections) {
    const std::vector<uint8_t> line_wkb = line({0, 0, 1, 2, 3, 4});
    const auto line_wkt = value(line_wkb).to_wkt();
    ASSERT_TRUE(line_wkt.has_value());
    EXPECT_EQ(*line_wkt, "LINESTRING (0 0, 1 2, 3 4)");

    const std::vector<uint8_t> polygon_wkb = polygon();
    const auto polygon_wkt = value(polygon_wkb).to_wkt();
    ASSERT_TRUE(polygon_wkt.has_value());
    EXPECT_EQ(*polygon_wkt,
              "POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), "
              "(2 2, 2 4, 4 4, 4 2, 2 2))");

    const auto multipoint = value(
        WkbBuilder().header(4U).u32(2U)
            .append(point(1, 2)).append(point(3, 4)).take())
        .to_wkt();
    ASSERT_TRUE(multipoint.has_value());
    EXPECT_EQ(*multipoint, "MULTIPOINT ((1 2), (3 4))");

    const auto multiline = value(
        WkbBuilder().header(5U).u32(2U)
            .append(line_wkb)
            .append(line({5, 6, 7, 8}))
            .take())
        .to_wkt();
    ASSERT_TRUE(multiline.has_value());
    EXPECT_EQ(*multiline,
              "MULTILINESTRING ((0 0, 1 2, 3 4), (5 6, 7 8))");

    const auto multipolygon = value(
        WkbBuilder().header(6U).u32(1U).append(polygon_wkb).take())
        .to_wkt();
    ASSERT_TRUE(multipolygon.has_value());
    EXPECT_EQ(*multipolygon,
              "MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0), "
              "(2 2, 2 4, 4 4, 4 2, 2 2)))");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryValueToWkt, ConvertsEmptyNonPointTypes) {
    EXPECT_EQ(*value(WkbBuilder().header(2U).u32(0U).take()).to_wkt(),
              "LINESTRING EMPTY");
    EXPECT_EQ(*value(WkbBuilder().header(3U).u32(0U).take()).to_wkt(),
              "POLYGON EMPTY");
    EXPECT_EQ(*value(WkbBuilder().header(4U).u32(0U).take()).to_wkt(),
              "MULTIPOINT EMPTY");
    EXPECT_EQ(*value(WkbBuilder().header(5U).u32(0U).take()).to_wkt(),
              "MULTILINESTRING EMPTY");
    EXPECT_EQ(*value(WkbBuilder().header(6U).u32(0U).take()).to_wkt(),
              "MULTIPOLYGON EMPTY");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryValueToWkt, ConvertsEmptyMembersInCollections) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto empty_point =
        WkbBuilder().header(1U).f64(nan).f64(nan).take();
    const auto multipoint = value(
        WkbBuilder().header(4U).u32(2U)
            .append(empty_point).append(point(1, 2)).take())
        .to_wkt();
    ASSERT_TRUE(multipoint.has_value());
    EXPECT_EQ(*multipoint, "MULTIPOINT (EMPTY, (1 2))");

    const auto multiline = value(
        WkbBuilder().header(5U).u32(2U)
            .append(WkbBuilder().header(2U).u32(0U).take())
            .append(line({0, 0, 1, 1})).take())
        .to_wkt();
    ASSERT_TRUE(multiline.has_value());
    EXPECT_EQ(*multiline, "MULTILINESTRING (EMPTY, (0 0, 1 1))");

    const auto multipolygon = value(
        WkbBuilder().header(6U).u32(2U)
            .append(WkbBuilder().header(3U).u32(0U).take())
            .append(polygon()).take())
        .to_wkt();
    ASSERT_TRUE(multipolygon.has_value());
    EXPECT_EQ(*multipolygon,
              "MULTIPOLYGON (EMPTY, ((0 0, 10 0, 10 10, 0 10, 0 0), "
              "(2 2, 2 4, 4 4, 4 2, 2 2)))");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryValueToWkt, RejectsMissingTruncatedAndUnsupportedWkb) {
    EXPECT_FALSE(GeometryValue{}.to_wkt().has_value());
    EXPECT_FALSE(value({1U, 1U, 0U}).to_wkt().has_value());
    EXPECT_FALSE(value(WkbBuilder().header(7U).take()).to_wkt().has_value());

    auto truncated = point(1, 2);
    truncated.pop_back();
    EXPECT_FALSE(value(std::move(truncated)).to_wkt().has_value());

    auto trailing = point(1, 2);
    trailing.push_back(0U);
    EXPECT_FALSE(value(std::move(trailing)).to_wkt().has_value());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryValueToWkt, RejectsInvalidNestedTypeAndDimensions) {
    const auto wrong_type = value(
        WkbBuilder().header(4U).u32(1U)
            .append(line({0, 0, 1, 1})).take())
        .to_wkt();
    EXPECT_FALSE(wrong_type.has_value());

    const auto wrong_dimension = value(
        WkbBuilder().header(1004U).u32(1U)
            .append(point(1, 2)).take())
        .to_wkt();
    EXPECT_FALSE(wrong_dimension.has_value());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryValueToWkt, RejectsInvalidCountsAndUnclosedRings) {
    EXPECT_FALSE(value(WkbBuilder().header(2U)
                           .u32(std::numeric_limits<uint32_t>::max())
                           .take())
                     .to_wkt()
                     .has_value());

    const auto unclosed = value(
        WkbBuilder().header(3U).u32(1U).u32(4U)
            .f64(0).f64(0)
            .f64(1).f64(0)
            .f64(1).f64(1)
            .f64(0).f64(1)
            .take())
        .to_wkt();
    EXPECT_FALSE(unclosed.has_value());
}
