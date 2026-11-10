// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

namespace {
void expect_conservative_bounds_contain_linearization(
    const CurveRequest& request) {
    const auto bounds = conservative_curve_bounds(request);
    ASSERT_TRUE(bounds.has_value());
    const auto linearized = linearize_curves(request);
    ASSERT_TRUE(linearized.valid()) << linearized.diagnostic;
    for (const auto& part : linearized.parts) {
        for (const auto& point : part) {
            const double x = request.transform.decode_x(point.x);
            const double y = request.transform.decode_y(point.y);
            EXPECT_GE(x, bounds->xmin - 1e-9);
            EXPECT_LE(x, bounds->xmax + 1e-9);
            EXPECT_GE(y, bounds->ymin - 1e-9);
            EXPECT_LE(y, bounds->ymax + 1e-9);
        }
    }
}
} // namespace

TEST(CurveGeometryContract, ConservativeBoundsContainAllLinearizedSamples) {
    {
        auto request = base_request({1000, 0}, {-1000, 0});
        CurveDescriptor curve;
        curve.kind = CurveSegmentKind::CircularArc;
        curve.values[0] = 0.0;
        curve.values[1] = 1.0;
        curve.flags = 0x80;
        request.curves = {curve};
        expect_conservative_bounds_contain_linearization(request);
    }
    {
        auto request = base_request({1000, 0}, {1000, 0});
        CurveDescriptor curve;
        curve.kind = CurveSegmentKind::CircularArc;
        curve.values[0] = -1.0;
        curve.values[1] = 0.0;
        curve.flags = 0x80;
        request.curves = {curve};
        expect_conservative_bounds_contain_linearization(request);
    }
    {
        // Center-form CCW arc crossing the +Y quadrant.
        auto request = base_request({1000, 0}, {0, 1000});
        CurveDescriptor curve;
        curve.kind = CurveSegmentKind::CircularArc;
        curve.values[0] = 0.0;
        curve.values[1] = 0.0;
        curve.flags = 0x8;
        request.curves = {curve};
        expect_conservative_bounds_contain_linearization(request);
    }
    {
        auto request = base_request({0, 0}, {1000, 0});
        CurveDescriptor curve;
        curve.kind = CurveSegmentKind::CubicBezier;
        curve.values[0] = 0.0;
        curve.values[1] = 1.0;
        curve.values[2] = 1.0;
        curve.values[3] = 1.0;
        request.curves = {curve};
        expect_conservative_bounds_contain_linearization(request);
    }
    {
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
        expect_conservative_bounds_contain_linearization(request);
        request.points[1] = request.points[0];
        request.curves[0].flags = 0x2000;
        expect_conservative_bounds_contain_linearization(request);
    }
    {
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
        expect_conservative_bounds_contain_linearization(request);
    }
    {
        // Line-form descriptors contribute their endpoints and no arc sweep.
        auto request = base_request({0, 0}, {1000, 1000});
        CurveDescriptor arc;
        arc.kind = CurveSegmentKind::CircularArc;
        arc.flags = 0x20;
        request.curves = {arc};
        expect_conservative_bounds_contain_linearization(request);
        request.curves[0].kind = CurveSegmentKind::EllipticArc;
        request.curves[0].flags = 0x40;
        expect_conservative_bounds_contain_linearization(request);
    }
    {
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
        expect_conservative_bounds_contain_linearization(request);
    }
}

TEST(CurveGeometryContract, ConservativeBoundsRejectInvalidOrUnsupportedDescriptors) {
    auto request = base_request({1000, 0}, {-1000, 0});
    CurveDescriptor curve;
    curve.kind = CurveSegmentKind::CircularArc;
    curve.flags = 0x1; // empty/point arc
    request.curves = {curve};
    EXPECT_FALSE(conservative_curve_bounds(request).has_value());
    request.curves[0].flags = 0x40; // point arc
    EXPECT_FALSE(conservative_curve_bounds(request).has_value());

    request.curves[0].flags = 0x200; // endpoint-vector ellipse
    request.curves[0].kind = CurveSegmentKind::EllipticArc;
    EXPECT_FALSE(conservative_curve_bounds(request).has_value());
    request.curves[0].flags = 0x80; // point ellipse
    EXPECT_FALSE(conservative_curve_bounds(request).has_value());

    request.curves[0].kind = CurveSegmentKind::CircularArc;
    request.curves[0].flags = 0; // center equals endpoint
    request.points = {{0, 0}, {0, 0}};
    request.curves[0].values[0] = 0.0;
    request.curves[0].values[1] = 0.0;
    EXPECT_FALSE(conservative_curve_bounds(request).has_value());

    auto valid = base_request({0, 0}, {1000, 0});
    CurveDescriptor duplicate;
    duplicate.kind = CurveSegmentKind::CubicBezier;
    duplicate.values[0] = 0.0;
    duplicate.values[1] = 1.0;
    duplicate.values[2] = 1.0;
    duplicate.values[3] = 1.0;
    valid.curves = {duplicate, duplicate};
    EXPECT_FALSE(conservative_curve_bounds(valid).has_value());

    valid.curves = {duplicate};
    valid.points.push_back({2000, 0});
    valid.part_sizes = {2, 2};
    valid.curves[0].start_vertex = 1; // crosses a part boundary
    EXPECT_FALSE(conservative_curve_bounds(valid).has_value());

    valid.points = {{0, 0}, {1000, 0}};
    valid.part_sizes = {3};
    valid.curves[0].start_vertex = 0;
    EXPECT_FALSE(conservative_curve_bounds(valid).has_value());

    auto nonfinite = base_request({0, 0}, {1000, 0});
    CurveDescriptor bezier;
    bezier.kind = CurveSegmentKind::CubicBezier;
    bezier.values[0] = std::numeric_limits<double>::quiet_NaN();
    bezier.values[1] = 1.0;
    bezier.values[2] = 1.0;
    bezier.values[3] = 1.0;
    nonfinite.curves = {bezier};
    EXPECT_FALSE(conservative_curve_bounds(nonfinite).has_value());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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
