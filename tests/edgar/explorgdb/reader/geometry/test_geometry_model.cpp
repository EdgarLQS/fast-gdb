// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include "gdb_geometry.h"
#include "polygon_topology.h"
#include "spatial_predicate.h"
#include "wkb_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace explorgdb;

namespace {

PointSequence square(int64_t xmin, int64_t ymin,
                     int64_t xmax, int64_t ymax,
                     bool reverse = false) {
    PointSequence ring{{xmin, ymin}, {xmax, ymin},
                       {xmax, ymax}, {xmin, ymax},
                       {xmin, ymin}};
    if (reverse) std::reverse(ring.begin(), ring.end());
    return ring;
}

uint32_t read_u32_le(const std::vector<uint8_t>& bytes,
                     size_t offset) {
    return static_cast<uint32_t>(bytes.at(offset)) |
           (static_cast<uint32_t>(bytes.at(offset + 1)) << 8) |
           (static_cast<uint32_t>(bytes.at(offset + 2)) << 16) |
           (static_cast<uint32_t>(bytes.at(offset + 3)) << 24);
}

void write_varuint(std::vector<uint8_t>& out, uint64_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        if (value) byte |= 0x80;
        out.push_back(byte);
    } while (value);
}

void write_varint(std::vector<uint8_t>& out, int64_t value) {
    uint64_t magnitude = value < 0
        ? static_cast<uint64_t>(-value)
        : static_cast<uint64_t>(value);
    uint8_t first = static_cast<uint8_t>(magnitude & 0x3f);
    magnitude >>= 6;
    if (value < 0) first |= 0x40;
    if (magnitude) first |= 0x80;
    out.push_back(first);
    while (magnitude) {
        uint8_t byte = static_cast<uint8_t>(magnitude & 0x7f);
        magnitude >>= 7;
        if (magnitude) byte |= 0x80;
        out.push_back(byte);
    }
}

void write_u32_le(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

void write_double_le(std::vector<uint8_t>& out, double value) {
    uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof(value));
    for (unsigned i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((raw >> (8 * i)) & 0xff));
}

void write_bbox(std::vector<uint8_t>& out,
                uint64_t xmin, uint64_t ymin,
                uint64_t dx, uint64_t dy) {
    write_varuint(out, xmin);
    write_varuint(out, ymin);
    write_varuint(out, dx);
    write_varuint(out, dy);
}

std::vector<uint8_t> build_point() {
    std::vector<uint8_t> out;
    write_varuint(out, 1);
    write_varuint(out, 1001);
    write_varuint(out, 2001);
    return out;
}

std::vector<uint8_t> build_polygon_with_hole_and_island() {
    std::vector<uint8_t> out;
    write_varuint(out, 5);
    write_varuint(out, 12);
    write_varuint(out, 3);
    write_bbox(out, 0, 0, 10000, 10000);
    write_varuint(out, 4);
    write_varuint(out, 4);

    // Deliberately shuffled: hole, outer, island. Directions are mixed.
    const std::vector<std::pair<int64_t, int64_t>> points{
        {3000, 3000}, {7000, 3000},
        {7000, 7000}, {3000, 7000},
        {0, 0}, {10000, 0},
        {10000, 10000}, {0, 10000},
        {4000, 4000}, {6000, 4000},
        {6000, 6000}, {4000, 6000}};
    int64_t x = 0, y = 0;
    for (const auto& point : points) {
        write_varint(out, point.first - x);
        write_varint(out, point.second - y);
        x = point.first;
        y = point.second;
    }
    return out;
}

std::vector<uint8_t> build_general_arc() {
    std::vector<uint8_t> out;
    write_varuint(out, 50 | 0x20000000ULL);
    write_varuint(out, 2); // nPoints
    write_varuint(out, 1); // nParts
    write_varuint(out, 1); // nCurves
    write_bbox(out, 0, 0, 2000, 1000);
    write_varint(out, 1000);
    write_varint(out, 0);
    write_varint(out, -2000);
    write_varint(out, 0);
    write_varuint(out, 0); // start vertex
    write_varuint(out, 1); // circular arc
    write_double_le(out, 0.0); // intermediate X
    write_double_le(out, 1.0); // intermediate Y
    write_u32_le(out, 0x80);   // intermediate-point form
    return out;
}

GdbGeomDecoder make_decoder() {
    return GdbGeomDecoder(0.0, 0.0, 1000.0,
                          0.0, 1000.0,
                          0.0, 1000.0,
                          false, false);
}

} // namespace

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(PolygonTopologyContract,
     OrganizesHoleAndIslandIndependentOfOrderAndDirection) {
    PolygonTopologyBuilder builder;
    auto result = builder.build({
        square(3000, 3000, 7000, 7000, true),
        square(0, 0, 10000, 10000, true),
        square(4000, 4000, 6000, 6000)});
    ASSERT_TRUE(result.valid()) << result.diagnostic;
    EXPECT_EQ(result.model.polygons.size(), 2u);
    size_t holes = 0;
    for (const auto& polygon : result.model.polygons)
        holes += polygon.interior_rings.size();
    EXPECT_EQ(holes, 1u);

    for (const auto& polygon : result.model.polygons) {
        EXPECT_GT(result.model.rings[
            polygon.exterior_ring].signed_area2, 0.0L);
        for (size_t hole : polygon.interior_rings)
            EXPECT_LT(result.model.rings[hole].signed_area2, 0.0L);
    }
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(PolygonTopologyContract,
     RejectsSelfIntersectionDuplicateAndDegenerateRings) {
    PolygonTopologyBuilder builder;
    auto bow_tie = builder.build(
        {{{0, 0}, {10, 10}, {0, 10}, {10, 0}}});
    EXPECT_FALSE(bow_tie.valid());
    EXPECT_EQ(bow_tie.status, TopologyStatus::SelfIntersection);

    auto duplicate = builder.build({
        square(0, 0, 10, 10),
        square(0, 0, 10, 10, true)});
    EXPECT_FALSE(duplicate.valid());
    EXPECT_EQ(duplicate.status, TopologyStatus::DuplicateRing);

    auto line = builder.build(
        {{{0, 0}, {1, 0}, {2, 0}}});
    EXPECT_FALSE(line.valid());
    EXPECT_EQ(line.status, TopologyStatus::DegenerateRing);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(PolygonTopologyContract,
     KeepsZAndMAttachedWhenOrientationIsReversed) {
    PointSequence clockwise{{0, 0, 10.0, 100.0},
                            {0, 10, 20.0, 200.0},
                            {10, 10, 30.0, 300.0},
                            {10, 0, 40.0, 400.0}};
    auto result = PolygonTopologyBuilder().build({clockwise});
    ASSERT_TRUE(result.valid());
    const auto& points = result.model.rings.front().points;
    for (const auto& point : points) {
        if (point.x == 0 && point.y == 0) {
            EXPECT_DOUBLE_EQ(point.z, 10.0);
            EXPECT_DOUBLE_EQ(point.m, 100.0);
        }
        if (point.x == 0 && point.y == 10) {
            EXPECT_DOUBLE_EQ(point.z, 20.0);
            EXPECT_DOUBLE_EQ(point.m, 200.0);
        }
        if (point.x == 10 && point.y == 10) {
            EXPECT_DOUBLE_EQ(point.z, 30.0);
            EXPECT_DOUBLE_EQ(point.m, 300.0);
        }
        if (point.x == 10 && point.y == 0) {
            EXPECT_DOUBLE_EQ(point.z, 40.0);
            EXPECT_DOUBLE_EQ(point.m, 400.0);
        }
    }
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(WkbContract, WritesValidStableMultiPolygonStructure) {
    auto topology = PolygonTopologyBuilder().build({
        square(3000, 3000, 7000, 7000),
        square(0, 0, 10000, 10000, true),
        square(4000, 4000, 6000, 6000)});
    ASSERT_TRUE(topology.valid());
    GeometryModel model;
    model.kind = GeometryKind::MultiPolygon;
    model.multipolygon = std::move(topology.model);
    auto value = WkbWriter::write(model);
    ASSERT_TRUE(value.valid()) << value.diagnostic;
    ASSERT_GE(value.wkb.size(), 9u);
    EXPECT_EQ(value.wkb[0], 1u);
    EXPECT_EQ(read_u32_le(value.wkb, 1), 6u);
    EXPECT_EQ(read_u32_le(value.wkb, 5), 2u);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryModelDecoder,
     DecodesPointAndProducesWkbWithoutWktRoundTrip) {
    auto blob = build_point();
    auto decoder = make_decoder();
    auto model = decoder.decode_model(blob.data(), blob.size());
    ASSERT_TRUE(model.valid()) << model.diagnostic;
    EXPECT_EQ(model.kind, GeometryKind::Point);
    EXPECT_EQ(model.point.x, 1000);
    EXPECT_EQ(model.point.y, 2000);

    auto value = decoder.decode_value(blob.data(), blob.size());
    ASSERT_TRUE(value.valid()) << value.diagnostic;
    EXPECT_EQ(value.geometry_type, 1u);
    EXPECT_EQ(value.wkb.size(), 21u);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryModelDecoder,
     PolygonWkbAndSpatialFilterShareTopology) {
    auto blob = build_polygon_with_hole_and_island();
    auto decoder = make_decoder();
    auto model = decoder.decode_model(blob.data(), blob.size());
    ASSERT_TRUE(model.valid()) << model.diagnostic;
    ASSERT_EQ(model.multipolygon.polygons.size(), 2u);

    EXPECT_TRUE(decoder.model_intersects_bbox(
        blob.data(), blob.size(), 1.0, 1.0, 2.0, 2.0));
    EXPECT_FALSE(decoder.model_intersects_bbox(
        blob.data(), blob.size(), 3.5, 3.5, 3.6, 3.6));
    EXPECT_TRUE(decoder.model_intersects_bbox(
        blob.data(), blob.size(), 4.5, 4.5, 4.6, 4.6));

    auto value = decoder.decode_value(blob.data(), blob.size());
    ASSERT_TRUE(value.valid()) << value.diagnostic;
    EXPECT_EQ(value.geometry_type, 6u);
    EXPECT_EQ(read_u32_le(value.wkb, 5), 2u);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryModelDecoder,
     ReportsTruncationAndSupportsExplicitCurveModes) {
    auto polygon = build_polygon_with_hole_and_island();
    auto decoder = make_decoder();
    auto truncated = decoder.decode_model(
        polygon.data(), polygon.size() - 1);
    EXPECT_FALSE(truncated.valid());
    EXPECT_EQ(truncated.status, GeometryStatus::InvalidEncoding);

    auto curve = build_general_arc();
    auto builtin = decoder.decode_model(curve.data(), curve.size());
    ASSERT_TRUE(builtin.valid()) << builtin.diagnostic;
    EXPECT_TRUE(builtin.source_was_curve);
    EXPECT_TRUE(builtin.linearized);
    EXPECT_EQ(builtin.backend, GeometryBackend::BuiltinCurve);
    ASSERT_EQ(builtin.lines.size(), 1u);
    EXPECT_GT(builtin.lines.front().size(), 3u);
    EXPECT_TRUE(decoder.model_intersects_bbox(
        curve.data(), curve.size(), -0.1, 0.9, 0.1, 1.1));

    decoder.set_curve_backend(CurveBackendMode::Reject);
    auto rejected = decoder.decode_model(curve.data(), curve.size());
    EXPECT_FALSE(rejected.valid());
    EXPECT_TRUE(rejected.source_was_curve);
    EXPECT_EQ(rejected.backend, GeometryBackend::Reject);
    EXPECT_EQ(rejected.status, GeometryStatus::UnsupportedCurve);
}
