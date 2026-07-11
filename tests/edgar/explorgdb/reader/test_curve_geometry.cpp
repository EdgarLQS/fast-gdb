#include "curve_geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace explorgdb;

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

CurveRequest base_request(GridPoint start, GridPoint end) {
    CurveRequest request;
    request.transform.xy_scale = 1000.0;
    request.points = {start, end};
    request.part_sizes = {2};
    request.options.max_chord_error = 0.0005;
    request.options.max_angle_step_degrees = 5.0;
    request.options.max_segments_per_curve = 2048;
    return request;
}

} // namespace

TEST(CurveGeometryContract, LinearizesThreePointCircularArc) {
    auto request = base_request({1000, 0}, {-1000, 0});
    CurveDescriptor curve;
    curve.kind = CurveSegmentKind::CircularArc;
    curve.values[0] = 0.0;
    curve.values[1] = 1.0;
    curve.flags = 0x80;
    request.curves = {curve};

    auto result = linearize_curves(request);
    ASSERT_TRUE(result.valid()) << result.diagnostic;
    ASSERT_EQ(result.parts.size(), 1u);
    EXPECT_GT(result.parts.front().size(), 3u);
    EXPECT_TRUE(std::any_of(result.parts.front().begin(),
                            result.parts.front().end(),
                            [](const GridPoint& point) {
                                return point.y >= 999;
                            }));
}

TEST(CurveGeometryContract, LinearizesFullCircleAndPreservesClosure) {
    auto request = base_request({1000, 0}, {1000, 0});
    CurveDescriptor curve;
    curve.kind = CurveSegmentKind::CircularArc;
    curve.values[0] = -1.0;
    curve.values[1] = 0.0;
    curve.flags = 0x80;
    request.curves = {curve};

    auto result = linearize_curves(request);
    ASSERT_TRUE(result.valid()) << result.diagnostic;
    ASSERT_GT(result.parts.front().size(), 8u);
    EXPECT_TRUE(same_xy(result.parts.front().front(),
                        result.parts.front().back()));
    int64_t min_y = result.parts.front().front().y;
    int64_t max_y = min_y;
    for (const auto& point : result.parts.front()) {
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }
    EXPECT_LE(min_y, -999);
    EXPECT_GE(max_y, 999);
}

TEST(CurveGeometryContract, LinearizesBezierAndInterpolatesZAndM) {
    auto request = base_request({0, 0, 10.0, 100.0},
                                {1000, 0, 20.0, 200.0});
    request.has_z = true;
    request.has_m = true;
    CurveDescriptor curve;
    curve.kind = CurveSegmentKind::CubicBezier;
    curve.values[0] = 0.0;
    curve.values[1] = 1.0;
    curve.values[2] = 1.0;
    curve.values[3] = 1.0;
    request.curves = {curve};

    auto result = linearize_curves(request);
    ASSERT_TRUE(result.valid()) << result.diagnostic;
    ASSERT_GT(result.parts.front().size(), 3u);
    const auto middle = result.parts.front()[
        result.parts.front().size() / 2];
    EXPECT_GT(middle.y, 500);
    EXPECT_GT(middle.z, 10.0);
    EXPECT_LT(middle.z, 20.0);
    EXPECT_GT(middle.m, 100.0);
    EXPECT_LT(middle.m, 200.0);
}

TEST(CurveGeometryContract, LinearizesMinorMajorAndCompleteEllipse) {
    auto request = base_request({2000, 0}, {-2000, 0});
    CurveDescriptor minor;
    minor.kind = CurveSegmentKind::EllipticArc;
    minor.values[0] = 0.0;
    minor.values[1] = 0.0;
    minor.values[2] = 0.0;
    minor.values[3] = 2.0;
    minor.values[4] = 0.5;
    minor.flags = 0x1000;
    request.curves = {minor};
    auto minor_result = linearize_curves(request);
    ASSERT_TRUE(minor_result.valid()) << minor_result.diagnostic;
    EXPECT_GT(minor_result.parts.front().size(), 3u);

    request.points[1] = request.points[0];
    request.curves[0].flags = 0x2000;
    auto complete_result = linearize_curves(request);
    ASSERT_TRUE(complete_result.valid()) << complete_result.diagnostic;
    EXPECT_GT(complete_result.parts.front().size(),
              minor_result.parts.front().size());
    EXPECT_TRUE(same_xy(complete_result.parts.front().front(),
                        complete_result.parts.front().back()));
}

TEST(CurveGeometryContract, UsesConventionalRotationAfterDescriptorNormalization) {
    // +90 degree Cartesian rotation: major-axis endpoints are (0, +/-2)
    // and the positive-parameter half passes through (-1, 0).
    auto request = base_request({0, 2000}, {0, -2000});
    CurveDescriptor ellipse;
    ellipse.kind = CurveSegmentKind::EllipticArc;
    ellipse.values[0] = 0.0;
    ellipse.values[1] = 0.0;
    ellipse.values[2] = kPi / 2.0;
    ellipse.values[3] = 2.0;
    ellipse.values[4] = 0.5;
    ellipse.flags = 0x1000;
    request.curves = {ellipse};

    auto result = linearize_curves(request);
    ASSERT_TRUE(result.valid()) << result.diagnostic;
    int64_t min_x = 0;
    for (const auto& point : result.parts.front())
        min_x = std::min(min_x, point.x);
    EXPECT_LE(min_x, -999);
}

TEST(CurveGeometryContract, ReconstructsMixedLinearAndCurveSegments) {
    CurveRequest request;
    request.transform.xy_scale = 1000.0;
    request.points = {{0, 0}, {1000, 0}, {2000, 0}, {3000, 0}};
    request.part_sizes = {4};
    CurveDescriptor curve;
    curve.start_vertex = 1;
    curve.kind = CurveSegmentKind::CubicBezier;
    curve.values[0] = 1.25;
    curve.values[1] = 1.0;
    curve.values[2] = 1.75;
    curve.values[3] = 1.0;
    request.curves = {curve};

    auto result = linearize_curves(request);
    ASSERT_TRUE(result.valid()) << result.diagnostic;
    ASSERT_GT(result.parts.front().size(), 4u);
    EXPECT_TRUE(same_xy(result.parts.front().front(),
                        request.points.front()));
    EXPECT_TRUE(same_xy(result.parts.front().back(),
                        request.points.back()));
    EXPECT_TRUE(std::any_of(result.parts.front().begin(),
                            result.parts.front().end(),
                            [](const GridPoint& point) {
                                return point.x > 1000 && point.x < 2000 &&
                                       point.y > 0;
                            }));
}

TEST(CurveGeometryContract, RejectsInvalidOptionsAndDescriptors) {
    auto request = base_request({0, 0}, {1000, 0});
    CurveDescriptor curve;
    curve.kind = CurveSegmentKind::CubicBezier;
    request.curves = {curve};
    request.options.max_segments_per_curve = 0;
    auto invalid_options = linearize_curves(request);
    EXPECT_FALSE(invalid_options.valid());
    EXPECT_EQ(invalid_options.status, GeometryStatus::InvalidEncoding);

    request.options.max_segments_per_curve = 10;
    request.curves[0].start_vertex = 2;
    auto invalid_start = linearize_curves(request);
    EXPECT_FALSE(invalid_start.valid());
    EXPECT_EQ(invalid_start.status, GeometryStatus::InvalidEncoding);

    request.curves[0].start_vertex =
        std::numeric_limits<size_t>::max();
    auto overflow_start = linearize_curves(request);
    EXPECT_FALSE(overflow_start.valid());
    EXPECT_EQ(overflow_start.status, GeometryStatus::InvalidEncoding);

    request.points = {{0, 0}, {1000, 0}, {2000, 0}, {3000, 0}};
    request.part_sizes = {2, 2};
    request.curves[0].start_vertex = 1;
    auto cross_part = linearize_curves(request);
    EXPECT_FALSE(cross_part.valid());
    EXPECT_EQ(cross_part.status, GeometryStatus::InvalidEncoding);
}
