// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include "gdb_geometry.h"
#include "polygon_topology.h"
#include "spatial_predicate.h"
#include "test_fixture_explorgdb.h"
#include "wkb_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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

TEST(GeometryModelDecoder,
     GeometryReadOptionsPreserveOrExplicitlyLinearizeCurvePayload) {
    const auto curve = build_general_arc();
    const auto decoder = make_decoder();

    GeometryReadOptions preserve = GeometryReadOptions::preserve_source();
    const GeometryValue preserved = decoder.decode_value_with_options(
        curve.data(), curve.size(), preserve);
    EXPECT_EQ(preserved.status, GeometryStatus::UnsupportedCurve);
    EXPECT_TRUE(preserved.source_was_curve);
    EXPECT_FALSE(preserved.linearized);
    EXPECT_EQ(preserved.source_payload_state,
              GeometrySourcePayloadState::Preserved);
    EXPECT_EQ(preserved.source_payload, curve);
    EXPECT_TRUE(preserved.wkb.empty());

    GeometryReadOptions linearize;
    linearize.curve_policy = CurveReadPolicy::Linearize;
    const GeometryValue linearized = decoder.decode_value_with_options(
        curve.data(), curve.size(), linearize);
    ASSERT_TRUE(linearized.valid()) << linearized.diagnostic;
    EXPECT_TRUE(linearized.source_was_curve);
    EXPECT_TRUE(linearized.linearized);
    EXPECT_EQ(linearized.source_payload_state,
              GeometrySourcePayloadState::Preserved);
    EXPECT_EQ(linearized.source_payload, curve);

    preserve.max_source_payload_bytes = 1;
    const GeometryValue limited = decoder.decode_value_with_options(
        curve.data(), curve.size(), preserve);
    EXPECT_EQ(limited.status, GeometryStatus::UnsupportedCurve);
    EXPECT_TRUE(limited.source_payload.empty());
    EXPECT_EQ(limited.source_payload_state,
              GeometrySourcePayloadState::LimitExceeded);
    EXPECT_NE(limited.diagnostic.find("preservation limit"),
              std::string::npos);
}

TEST(GeometryModelDecoder,
     MultipatchPreservesStructureDimensionsAndRawPayload) {
    const auto multipatch = explorgdb_test::build_geom_multipatch();
    const auto multipatch_m = explorgdb_test::build_geom_multipatch_m();
    const auto decoder = make_decoder();
    const auto options = GeometryReadOptions::preserve_source();

    const GeometryValue value = decoder.decode_value_with_options(
        multipatch.data(), multipatch.size(), options);
    EXPECT_EQ(value.status, GeometryStatus::Valid);
    EXPECT_TRUE(value.has_z);
    EXPECT_FALSE(value.has_m);
    EXPECT_EQ(value.geometry_type, 1016U);
    ASSERT_FALSE(value.wkb.empty());
    EXPECT_EQ(read_u32_le(value.wkb, 1U), 1016U);
    ASSERT_TRUE(value.to_wkt().has_value());
    EXPECT_EQ(*value.to_wkt(), "TIN Z (((0.001 0.001 1.001, "
                              "1.001 0.001 2.001, "
                              "1.001 1.001 3.001, "
                              "0.001 0.001 1.001)))");
    EXPECT_EQ(value.source_payload_state,
              GeometrySourcePayloadState::Preserved);
    EXPECT_EQ(value.source_payload, multipatch);
    ASSERT_TRUE(value.multipatch);
    ASSERT_TRUE(value.multipatch->structure_complete);
    ASSERT_EQ(value.multipatch->parts.size(), 1U);
    EXPECT_EQ(value.multipatch->parts[0].type,
              MultiPatchPartType::TriangleStrip);
    EXPECT_EQ(value.multipatch->parts[0].triangle_count, 1U);

    const GeometryValue value_m = decoder.decode_value_with_options(
        multipatch_m.data(), multipatch_m.size(), options);
    EXPECT_EQ(value_m.status, GeometryStatus::Valid);
    EXPECT_TRUE(value_m.has_z);
    EXPECT_TRUE(value_m.has_m);
    EXPECT_EQ(value_m.geometry_type, 3006U);
    ASSERT_FALSE(value_m.wkb.empty());
    ASSERT_TRUE(value_m.to_wkt().has_value());
    EXPECT_NE(value_m.to_wkt()->find("MULTIPOLYGON ZM"),
              std::string::npos);
    EXPECT_EQ(value_m.source_payload_state,
              GeometrySourcePayloadState::Preserved);
    EXPECT_EQ(value_m.source_payload, multipatch_m);
    ASSERT_TRUE(value_m.multipatch);
    ASSERT_TRUE(value_m.multipatch->structure_complete);
    ASSERT_EQ(value_m.multipatch->parts.size(), 1U);
    EXPECT_EQ(value_m.multipatch->parts[0].type,
              MultiPatchPartType::OuterRing);
}

TEST(GeometryModelDecoder,
     MultipatchParsesAllPartTypesFacesAndRingGroups) {
    const auto multipatch = explorgdb_test::build_geom_multipatch_parts(
        {0, 1, 0x12, 3, 4, 5, 6}, {5, 5, 4, 4, 4, 4, 6});
    auto decoder = make_decoder();
    const GeometryModel model = decoder.decode_model(
        multipatch.data(), multipatch.size());

    EXPECT_EQ(model.status, GeometryStatus::Valid);
    EXPECT_FALSE(model.is_empty());
    ASSERT_TRUE(model.multipatch);
    ASSERT_TRUE(model.multipatch->structure_complete);
    ASSERT_EQ(model.multipatch->parts.size(), 7U);
    ASSERT_EQ(model.multipatch->vertices.size(), 32U);
    EXPECT_EQ(model.multipatch->parts[0].type,
              MultiPatchPartType::TriangleStrip);
    EXPECT_EQ(model.multipatch->parts[1].type,
              MultiPatchPartType::TriangleFan);
    EXPECT_EQ(model.multipatch->parts[6].type,
              MultiPatchPartType::Triangles);
    EXPECT_EQ(model.multipatch->parts[2].type,
              MultiPatchPartType::OuterRing);
    EXPECT_EQ(model.multipatch->parts[2].raw_type, 0x12U);
    EXPECT_EQ(model.multipatch->parts[0].triangle_count, 3U);
    EXPECT_EQ(model.multipatch->parts[1].triangle_count, 3U);
    EXPECT_EQ(model.multipatch->parts[6].triangle_count, 2U);
    EXPECT_DOUBLE_EQ(model.multipatch->vertices[0].x, 0.001);
    EXPECT_DOUBLE_EQ(model.multipatch->vertices[0].z, 1.001);

    std::array<size_t, 3> triangle{};
    ASSERT_TRUE(model.multipatch->parts[0].triangle_vertices(1, triangle));
    EXPECT_EQ(triangle, (std::array<size_t, 3>{2, 1, 3}));
    ASSERT_TRUE(model.multipatch->parts[1].triangle_vertices(1, triangle));
    EXPECT_EQ(triangle, (std::array<size_t, 3>{5, 7, 8}));
    ASSERT_TRUE(model.multipatch->parts[6].triangle_vertices(1, triangle));
    EXPECT_EQ(triangle, (std::array<size_t, 3>{29, 30, 31}));
    EXPECT_FALSE(model.multipatch->parts[0].triangle_vertices(3, triangle));

    ASSERT_EQ(model.multipatch->ring_groups.size(), 2U);
    EXPECT_EQ(model.multipatch->ring_groups[0].encoding,
              MultiPatchRingEncoding::OuterAndInner);
    EXPECT_EQ(model.multipatch->ring_groups[0].part_indices,
              (std::vector<size_t>{2, 3}));
    EXPECT_EQ(model.multipatch->ring_groups[1].encoding,
              MultiPatchRingEncoding::FirstAndFollowing);
    EXPECT_EQ(model.multipatch->ring_groups[1].part_indices,
              (std::vector<size_t>{4, 5}));

    const GeometryValue value = WkbWriter::write(model);
    EXPECT_EQ(value.status, GeometryStatus::Valid);
    EXPECT_EQ(value.geometry_type, 1007U);
    ASSERT_FALSE(value.wkb.empty());
    ASSERT_TRUE(value.to_wkt().has_value());
    EXPECT_EQ(value.to_wkt()->rfind("GEOMETRYCOLLECTION Z (", 0), 0U);
    EXPECT_NE(value.to_wkt()->find("TIN Z"), std::string::npos);
    EXPECT_NE(value.to_wkt()->find("MULTIPOLYGON Z"), std::string::npos);
}

TEST(GeometryModelDecoder,
     MultipatchMapsRingGroupsAndMixedPartsToStandardWkb) {
    auto decoder = make_decoder();
    const auto ring_parts = explorgdb_test::build_geom_multipatch_parts(
        {2, 3, 4, 5}, {4, 4, 4, 4}, true);
    const GeometryValue rings = decoder.decode_value(
        ring_parts.data(), ring_parts.size());
    ASSERT_EQ(rings.status, GeometryStatus::Valid) << rings.diagnostic;
    EXPECT_EQ(rings.geometry_type, 3006U);
    ASSERT_TRUE(rings.to_wkt().has_value());
    EXPECT_EQ(rings.to_wkt()->rfind("MULTIPOLYGON ZM", 0), 0U);
    EXPECT_NE(rings.to_wkt()->find(")), (("), std::string::npos);

    const auto mixed_parts = explorgdb_test::build_geom_multipatch_parts(
        {0, 2}, {4, 4});
    const GeometryValue mixed = decoder.decode_value(
        mixed_parts.data(), mixed_parts.size());
    ASSERT_EQ(mixed.status, GeometryStatus::Valid) << mixed.diagnostic;
    EXPECT_EQ(mixed.geometry_type, 1007U);
    ASSERT_TRUE(mixed.to_wkt().has_value());
    EXPECT_EQ(mixed.to_wkt()->rfind("GEOMETRYCOLLECTION Z", 0), 0U);
    EXPECT_NE(mixed.to_wkt()->find("TIN Z"), std::string::npos);
    EXPECT_NE(mixed.to_wkt()->find("MULTIPOLYGON Z"), std::string::npos);
}

TEST(GeometryModelDecoder,
     MultipatchConnectedOuterRingsMatchGdalTinHeuristic) {
    auto decoder = make_decoder();
    const auto blob =
        explorgdb_test::build_geom_multipatch_outer_ring_tin();
    const GeometryValue value = decoder.decode_value(blob.data(), blob.size());
    ASSERT_EQ(value.status, GeometryStatus::Valid) << value.diagnostic;
    EXPECT_EQ(value.geometry_type, 1016U);
    ASSERT_TRUE(value.to_wkt().has_value());
    EXPECT_EQ(value.to_wkt()->rfind("TIN Z (", 0), 0U);
    EXPECT_NE(value.to_wkt()->find(")), (("), std::string::npos);
    EXPECT_EQ(read_u32_le(value.wkb, 5U), 2U);
}

TEST(GeometryModelDecoder, MultipatchSpatialFilterChecksProjectedSurfaces) {
    auto decoder = make_decoder();
    const auto triangle_blob = explorgdb_test::build_geom_multipatch();
    const GeometryModel triangle = decoder.decode_model(
        triangle_blob.data(), triangle_blob.size());
    ASSERT_EQ(triangle.status, GeometryStatus::Valid);
    EXPECT_TRUE(SpatialPredicate::intersects_bbox(
        triangle, QueryGridBbox{0.0L, 0.0L, 2000.0L, 2000.0L}));
    EXPECT_TRUE(SpatialPredicate::intersects_bbox(
        triangle, QueryGridBbox{800.0L, 100.0L, 900.0L, 200.0L}));
    EXPECT_FALSE(SpatialPredicate::intersects_bbox(
        triangle, QueryGridBbox{100.0L, 900.0L, 200.0L, 950.0L}));

    const auto ring_blob = explorgdb_test::build_geom_multipatch_m();
    const GeometryModel ring = decoder.decode_model(
        ring_blob.data(), ring_blob.size());
    ASSERT_EQ(ring.status, GeometryStatus::Valid);
    EXPECT_TRUE(SpatialPredicate::intersects_bbox(
        ring, QueryGridBbox{800.0L, 100.0L, 900.0L, 200.0L}));
    EXPECT_FALSE(SpatialPredicate::intersects_bbox(
        ring, QueryGridBbox{100.0L, 900.0L, 200.0L, 950.0L}));
}

TEST(GeometryModelDecoder,
     MultipatchRetainsUnknownTypesAndRejectsInvalidTriangleGroups) {
    const auto unknown = explorgdb_test::build_geom_multipatch_parts(
        {7}, {3});
    const auto invalid_triangles =
        explorgdb_test::build_geom_multipatch_parts({6}, {4});
    auto decoder = make_decoder();

    const GeometryModel unknown_model = decoder.decode_model(
        unknown.data(), unknown.size());
    EXPECT_EQ(unknown_model.status, GeometryStatus::UnsupportedType);
    ASSERT_TRUE(unknown_model.multipatch);
    ASSERT_TRUE(unknown_model.multipatch->structure_complete);
    ASSERT_EQ(unknown_model.multipatch->parts.size(), 1U);
    EXPECT_EQ(unknown_model.multipatch->parts[0].type,
              MultiPatchPartType::Unknown);
    EXPECT_EQ(unknown_model.multipatch->parts[0].raw_type, 7U);
    EXPECT_NE(unknown_model.diagnostic.find("unknown part type"),
              std::string::npos);
    const GeometryValue unknown_value = WkbWriter::write(unknown_model);
    EXPECT_EQ(unknown_value.status, GeometryStatus::UnsupportedType);
    EXPECT_TRUE(unknown_value.wkb.empty());

    auto opaque_tail = explorgdb_test::build_geom_multipatch();
    opaque_tail.push_back(0x7fU);
    const GeometryValue opaque = decoder.decode_value_with_options(
        opaque_tail.data(), opaque_tail.size(),
        GeometryReadOptions::preserve_source());
    EXPECT_EQ(opaque.status, GeometryStatus::UnsupportedType);
    EXPECT_TRUE(opaque.wkb.empty());
    EXPECT_EQ(opaque.source_payload, opaque_tail);
    ASSERT_TRUE(opaque.multipatch);
    EXPECT_EQ(opaque.multipatch->opaque_tail_bytes, 1U);

    const GeometryValue invalid = decoder.decode_value_with_options(
        invalid_triangles.data(), invalid_triangles.size(),
        GeometryReadOptions::preserve_source());
    EXPECT_EQ(invalid.status, GeometryStatus::InvalidTopology);
    EXPECT_TRUE(invalid.wkb.empty());
    ASSERT_TRUE(invalid.multipatch);
    EXPECT_TRUE(invalid.multipatch->structure_complete);
    EXPECT_EQ(invalid.source_payload, invalid_triangles);
    EXPECT_EQ(invalid.source_payload_state,
              GeometrySourcePayloadState::Preserved);
}

TEST(GeometryModelDecoder, EmptyMultipatchKeepsEmptyStatusWithoutWkb) {
    std::vector<uint8_t> empty;
    explorgdb_test::write_varuint(empty, 32U);
    explorgdb_test::write_varuint(empty, 0U);
    auto decoder = make_decoder();
    const GeometryValue value = decoder.decode_value(
        empty.data(), empty.size());
    EXPECT_EQ(value.status, GeometryStatus::Empty);
    EXPECT_TRUE(value.wkb.empty());
}

TEST(GeometryModelDecoder, GeneralMultipatchUsesZAndMTypeFlags) {
    constexpr uint64_t kGeneralZFlag = 0x80000000ULL;
    constexpr uint64_t kGeneralMFlag = 0x40000000ULL;
    const auto general = explorgdb_test::build_geom_multipatch_parts(
        {0}, {3}, true,
        54ULL | kGeneralZFlag | kGeneralMFlag);
    auto decoder = make_decoder();

    const GeometryModel model = decoder.decode_model(
        general.data(), general.size());
    EXPECT_TRUE(model.has_z);
    EXPECT_TRUE(model.has_m);
    EXPECT_EQ(model.status, GeometryStatus::Valid);
    ASSERT_TRUE(model.multipatch);
    EXPECT_TRUE(model.multipatch->structure_complete);
    ASSERT_EQ(model.multipatch->vertices.size(), 3U);
    EXPECT_DOUBLE_EQ(model.multipatch->vertices[0].z, 1.001);
    EXPECT_DOUBLE_EQ(model.multipatch->vertices[0].m, 5.001);
}
