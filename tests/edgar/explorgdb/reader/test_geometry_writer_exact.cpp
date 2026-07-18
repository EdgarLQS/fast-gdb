#include <gtest/gtest.h>

#include "wkb_writer.h"
#include "wkt_writer.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace explorgdb;

namespace {

uint32_t read_u32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes.at(offset)) |
           (static_cast<uint32_t>(bytes.at(offset + 1U)) << 8U) |
           (static_cast<uint32_t>(bytes.at(offset + 2U)) << 16U) |
           (static_cast<uint32_t>(bytes.at(offset + 3U)) << 24U);
}

} // namespace

TEST(GeometryWriterExactTest, PointZmOutputRemainsByteAndTextCompatible) {
    GeometryModel point;
    point.kind = GeometryKind::Point;
    point.has_z = true;
    point.has_m = true;
    point.transform.xy_scale = 1000.0;
    point.point = {1250, 2500, 3.5, 7.25};

    const std::string expected_wkt = "POINT ZM (1.25 2.5 3.5 7.25)";
    EXPECT_EQ(WktWriter::write(point), expected_wkt);

    const GeometryValue value = WkbWriter::write(point);
    ASSERT_TRUE(value.valid()) << value.diagnostic;
    ASSERT_EQ(value.wkb.size(), 37U);
    EXPECT_EQ(value.wkb.front(), 1U);
    EXPECT_EQ(read_u32(value.wkb, 1U), 3001U);

    const auto on_demand_wkt = value.to_wkt();
    ASSERT_TRUE(on_demand_wkt.has_value());
    EXPECT_EQ(*on_demand_wkt, expected_wkt);
}

TEST(GeometryWriterExactTest, InvalidWkbFailsClosedForOnDemandWkt) {
    GeometryValue truncated;
    truncated.wkb = {1U, 1U, 0U, 0U, 0U, 0U};

    EXPECT_FALSE(truncated.to_wkt().has_value());
}
