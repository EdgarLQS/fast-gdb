#include "gdb_geometry.h"
#include "polygon_topology.h"
#include "spatial_predicate.h"
#include "wkb_writer.h"
#include "wkt_writer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

using namespace explorgdb;

namespace {

uint32_t read_u32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes.at(offset)) |
           (static_cast<uint32_t>(bytes.at(offset + 1)) << 8) |
           (static_cast<uint32_t>(bytes.at(offset + 2)) << 16) |
           (static_cast<uint32_t>(bytes.at(offset + 3)) << 24);
}

void append_varuint(std::vector<uint8_t>& bytes, uint64_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        if (value != 0) byte |= 0x80;
        bytes.push_back(byte);
    } while (value != 0);
}

void append_varint(std::vector<uint8_t>& bytes, int64_t value) {
    const bool negative = value < 0;
    uint64_t magnitude = negative
        ? static_cast<uint64_t>(-(value + 1)) + 1
        : static_cast<uint64_t>(value);

    uint8_t first = static_cast<uint8_t>(magnitude & 0x3f);
    magnitude >>= 6;
    if (negative) first |= 0x40;
    if (magnitude != 0) first |= 0x80;
    bytes.push_back(first);

    while (magnitude != 0) {
        uint8_t byte = static_cast<uint8_t>(magnitude & 0x7f);
        magnitude >>= 7;
        if (magnitude != 0) byte |= 0x80;
        bytes.push_back(byte);
    }
}

void append_le_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
}

void append_le_double(std::vector<uint8_t>& bytes, double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (unsigned shift = 0; shift < 64; shift += 8)
        bytes.push_back(static_cast<uint8_t>((bits >> shift) & 0xff));
}

std::vector<uint8_t> m_curve_blob(bool missing_m_array) {
    constexpr uint64_t kGeneralPolylineMWithCurve = 0x60000032ULL;
    constexpr uint32_t kArcIntermediatePoint = 0x80;

    std::vector<uint8_t> blob;
    append_varuint(blob, kGeneralPolylineMWithCurve);
    append_varuint(blob, 2);  // point count
    append_varuint(blob, 1);  // part count
    append_varuint(blob, 1);  // curve count
    append_varuint(blob, 0);  // bbox xmin
    append_varuint(blob, 0);  // bbox ymin
    append_varuint(blob, 10000);  // bbox dx
    append_varuint(blob, 5000);   // bbox dy

    // Single part: no explicit part-size entries. XY values are cumulative
    // signed deltas: (0, 0) -> (10, 0) at scale 1000.
    append_varint(blob, 0);
    append_varint(blob, 0);
    append_varint(blob, 10000);
    append_varint(blob, 0);

    if (missing_m_array) {
        // FileGDB marker used when an M-enabled class stores a 2D geometry.
        blob.push_back(0x42);
    } else {
        append_varint(blob, 0);      // first M = 0
        append_varint(blob, 10000);  // second M = 10
    }

    append_varuint(blob, 0);  // descriptor start vertex
    append_varuint(blob, 1);  // CircularArc
    append_le_double(blob, 5.0);
    append_le_double(blob, 5.0);
    append_le_u32(blob, kArcIntermediatePoint);
    return blob;
}

GdbGeomDecoder decoder() {
    GdbGeomDecoder result(0.0, 0.0, 1000.0,
                          0.0, 1000.0,
                          0.0, 1000.0,
                          false, false);
    result.set_curve_backend(CurveBackendMode::Builtin);
    return result;
}

} // namespace

TEST(GeometryOutputContract, WritesIsoZmTypeCodesAndCoordinates) {
    GeometryModel point;
    point.kind = GeometryKind::Point;
    point.has_z = true;
    point.has_m = true;
    point.transform.xy_scale = 1000.0;
    point.point = {1250, 2500, 3.5, 7.25};

    auto value = WkbWriter::write(point);
    ASSERT_TRUE(value.valid()) << value.diagnostic;
    ASSERT_EQ(value.wkb.size(), 37u);
    EXPECT_EQ(read_u32(value.wkb, 1), 3001u);
    EXPECT_TRUE(value.has_z);
    EXPECT_TRUE(value.has_m);

    const std::string wkt = WktWriter::write(point);
    EXPECT_NE(wkt.find("POINT ZM"), std::string::npos);
    EXPECT_NE(wkt.find("1.25 2.5 3.5 7.25"),
              std::string::npos);
}

TEST(GeometryOutputContract, WktUsesOrganizedPolygonTopology) {
    PointSequence outer{{0, 0}, {10000, 0},
                        {10000, 10000}, {0, 10000}};
    PointSequence hole{{3000, 3000}, {3000, 7000},
                       {7000, 7000}, {7000, 3000}};
    auto topology = PolygonTopologyBuilder().build({hole, outer});
    ASSERT_TRUE(topology.valid()) << topology.diagnostic;

    GeometryModel model;
    model.kind = GeometryKind::MultiPolygon;
    model.transform.xy_scale = 1000.0;
    model.multipolygon = std::move(topology.model);
    const std::string wkt = WktWriter::write(model);
    EXPECT_NE(wkt.find("MULTIPOLYGON"), std::string::npos);
    EXPECT_NE(wkt.find("3 7"), std::string::npos);
    EXPECT_EQ(model.multipolygon.polygons.size(), 1u);
    EXPECT_EQ(model.multipolygon.polygons.front().interior_rings.size(),
              1u);
}

TEST(GeometryTopologySafety, OrientationHandlesFullInt64Range) {
    const GridPoint minimum{
        std::numeric_limits<int64_t>::min(),
        std::numeric_limits<int64_t>::min()};
    const GridPoint east{
        std::numeric_limits<int64_t>::max(),
        std::numeric_limits<int64_t>::min()};
    const GridPoint north_east{
        std::numeric_limits<int64_t>::max(),
        std::numeric_limits<int64_t>::max()};

    EXPECT_GT(orientation(minimum, east, north_east), 0);
    EXPECT_LT(orientation(minimum, north_east, east), 0);
}

TEST(GeometryTopologySafety, OrientationHandlesNegativeExtremeProducts) {
    const GridPoint a{std::numeric_limits<int64_t>::min(), 7};
    const GridPoint b{std::numeric_limits<int64_t>::max(), 7};
    const GridPoint c{std::numeric_limits<int64_t>::min(), -9};

    EXPECT_LT(orientation(a, b, c), 0);
    EXPECT_GT(orientation(a, c, b), 0);
}

TEST(GeometryTopologySafety, SegmentRelationsRemainStableAtInt64Bounds) {
    const int64_t minimum = std::numeric_limits<int64_t>::min();
    const int64_t maximum = std::numeric_limits<int64_t>::max();
    const GridPoint left{minimum, 0};
    const GridPoint right{maximum, 0};
    const GridPoint lower{0, minimum};
    const GridPoint upper{0, maximum};

    EXPECT_EQ(segment_relation(left, right, lower, upper),
              SegmentRelation::Cross);
    EXPECT_EQ(segment_relation(left, right,
                               {minimum, 0}, {maximum, 0}),
              SegmentRelation::Overlap);
}

TEST(GeometrySpatialSafety, ContinuousGridBboxCrossesSegment) {
    GeometryModel line;
    line.kind = GeometryKind::LineString;
    line.transform.xy_scale = 1.0;
    line.lines = {{{0, 0}, {10, 10}}};

    // The query contains no integer-grid vertex. Intersection must be based on
    // continuous segment geometry rather than rounded bbox coordinates.
    const QueryGridBbox query{4.25L, 4.25L, 4.75L, 4.75L};
    EXPECT_TRUE(SpatialPredicate::intersects_bbox(line, query));

    const QueryGridBbox disjoint{4.25L, 5.25L, 4.75L, 5.75L};
    EXPECT_FALSE(SpatialPredicate::intersects_bbox(line, disjoint));
}

TEST(GeometryCurveDecoder,
     ParsesArcgisMissingMMarkerBeforeNativeCurveDescriptors) {
    const auto blob = m_curve_blob(true);
    auto geometry_decoder = decoder();

    const auto model = geometry_decoder.decode_model(blob.data(), blob.size());
    ASSERT_TRUE(model.valid()) << model.diagnostic;
    EXPECT_TRUE(model.has_m);
    EXPECT_TRUE(model.source_was_curve);
    EXPECT_TRUE(model.linearized);
    EXPECT_EQ(model.backend, GeometryBackend::BuiltinCurve);
    ASSERT_EQ(model.lines.size(), 1u);
    ASSERT_GT(model.lines.front().size(), 2u);
    for (const auto& point : model.lines.front())
        EXPECT_TRUE(std::isnan(point.m));

    const auto value = geometry_decoder.decode_value(blob.data(), blob.size());
    ASSERT_TRUE(value.valid()) << value.diagnostic;
    EXPECT_TRUE(value.has_m);
    EXPECT_TRUE(value.source_was_curve);
    EXPECT_TRUE(value.linearized);
    EXPECT_FALSE(value.wkb.empty());
}

TEST(GeometryCurveDecoder, PreservesCanonicalMArrayBeforeDescriptors) {
    const auto blob = m_curve_blob(false);
    auto geometry_decoder = decoder();

    const auto model = geometry_decoder.decode_model(blob.data(), blob.size());
    ASSERT_TRUE(model.valid()) << model.diagnostic;
    ASSERT_EQ(model.lines.size(), 1u);
    ASSERT_GT(model.lines.front().size(), 2u);
    EXPECT_DOUBLE_EQ(model.lines.front().front().m, 0.0);
    EXPECT_DOUBLE_EQ(model.lines.front().back().m, 10.0);
    for (const auto& point : model.lines.front())
        EXPECT_TRUE(std::isfinite(point.m));
}

TEST(GeometryCurveDecoder, MissingMMarkerStillFailsClosedOnTruncation) {
    auto blob = m_curve_blob(true);
    ASSERT_FALSE(blob.empty());
    blob.pop_back();

    auto geometry_decoder = decoder();
    const auto model = geometry_decoder.decode_model(blob.data(), blob.size());
    EXPECT_FALSE(model.valid());
    EXPECT_EQ(model.status, GeometryStatus::InvalidEncoding);
    EXPECT_NE(model.diagnostic.find("curve descriptors"), std::string::npos);
}

TEST(GeometryDecoderSafety,
     EveryNonEmptyTruncatedPrefixFailsWithoutThrowing) {
    // General polyline with a declared curve but no complete descriptor.
    const std::vector<uint8_t> blob{
        0xb2, 0x80, 0x80, 0x80, 0x02,
        0x02, 0x01, 0x01,
        0x00, 0x00, 0x01, 0x01,
        0x00, 0x00, 0x02, 0x00};
    auto geometry_decoder = decoder();
    for (size_t size = 1; size < blob.size(); ++size) {
        EXPECT_NO_THROW({
            const auto model = geometry_decoder.decode_model(
                blob.data(), size);
            EXPECT_FALSE(model.valid()) << "prefix=" << size;
        });
    }
}

TEST(GeometryDecoderSafety, DeterministicGarbageNeverThrows) {
    auto geometry_decoder = decoder();
    std::mt19937_64 random(0xF17E6DBULL);
    for (size_t case_index = 0; case_index < 1000; ++case_index) {
        const size_t size = static_cast<size_t>(random() % 96);
        std::vector<uint8_t> bytes(size);
        for (auto& byte : bytes)
            byte = static_cast<uint8_t>(random() & 0xff);
        EXPECT_NO_THROW({
            const auto model = geometry_decoder.decode_model(
                bytes.data(), bytes.size());
            const auto value = WkbWriter::write(model);
            (void)value;
        });
    }
}
