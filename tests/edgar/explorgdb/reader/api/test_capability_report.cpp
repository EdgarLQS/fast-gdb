// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>
#include "capability_report.h"

using namespace explorgdb;

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(CapabilityReportTest, SupportedLayerIsReadable) {
    CapabilityReport report;
    report.curve_geometry = {CapabilityState::Supported, "none"};
    report.multipatch = {CapabilityState::Supported, "none"};
    report.raster = {CapabilityState::Supported, "none"};
    EXPECT_TRUE(report.can_read_layer());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(CapabilityReportTest, UnsupportedGeometryBlocksLayer) {
    CapabilityReport report;
    report.curve_geometry = {CapabilityState::Supported, "none"};
    report.multipatch = {CapabilityState::Unsupported, "multipatch unsupported"};
    report.raster = {CapabilityState::Supported, "none"};
    EXPECT_FALSE(report.can_read_layer());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(CapabilityReportTest, DegradedRasterAndCurveRemainReadable) {
    CapabilityReport report;
    report.curve_geometry = {CapabilityState::Degraded, "curves are explicit"};
    report.multipatch = {CapabilityState::Supported, "not a multipatch layer"};
    report.raster = {CapabilityState::Degraded, "raster pixels are not read"};
    EXPECT_TRUE(report.can_read_layer());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(CapabilityReportTest, DegradedMultiPatchRemainsReadableWithExplicitBoundary) {
    CapabilityReport report;
    report.curve_geometry = {CapabilityState::Supported, "none"};
    report.multipatch = {
        CapabilityState::Degraded,
        "coordinates exposed; part type and surface topology are not preserved"
    };
    report.raster = {CapabilityState::Supported, "none"};

    EXPECT_TRUE(report.can_read_layer());
    EXPECT_EQ(report.multipatch.state, CapabilityState::Degraded);
    EXPECT_NE(report.multipatch.reason.find("topology"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(CapabilityReportTest, StateNamesAreStable) {
    EXPECT_STREQ(capability_state_name(CapabilityState::Supported), "supported");
    EXPECT_STREQ(capability_state_name(CapabilityState::Degraded), "degraded");
    EXPECT_STREQ(capability_state_name(CapabilityState::Unsupported), "unsupported");
}
