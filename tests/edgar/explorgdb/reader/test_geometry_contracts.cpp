#include "gdb_geometry.h"
#include "polygon_topology.h"
#include "wkb_writer.h"
#include "wkt_writer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
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

GdbGeomDecoder decoder() {
    return GdbGeomDecoder(0.0, 0.0, 1000.0,
                          0.0, 1000.0,
                          0.0, 1000.0,
                          false, false);
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

TEST(GeometryDecoderSafety, EveryNonEmptyTruncatedPrefixFailsWithoutThrowing) {
    // General polyline with a declared curve but no complete descriptor.
    const std::vector<uint8_t> blob{
        0xb2, 0x80, 0x80, 0x80, 0x02,
        0x02, 0x01, 0x01,
        0x00, 0x00, 0x01, 0x01,
        0x00, 0x00, 0x02, 0x00};
    auto d = decoder();
    for (size_t size = 1; size < blob.size(); ++size) {
        EXPECT_NO_THROW({
            const auto model = d.decode_model(blob.data(), size);
            EXPECT_FALSE(model.valid()) << "prefix=" << size;
        });
    }
}

TEST(GeometryDecoderSafety, DeterministicGarbageNeverThrows) {
    auto d = decoder();
    std::mt19937_64 random(0xF17E6DBULL);
    for (size_t case_index = 0; case_index < 1000; ++case_index) {
        const size_t size = static_cast<size_t>(random() % 96);
        std::vector<uint8_t> bytes(size);
        for (auto& byte : bytes)
            byte = static_cast<uint8_t>(random() & 0xff);
        EXPECT_NO_THROW({
            const auto model = d.decode_model(bytes.data(), bytes.size());
            const auto value = WkbWriter::write(model);
            (void)value;
        });
    }
}
