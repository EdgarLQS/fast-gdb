#include <gtest/gtest.h>
#include "capability_report.h"

using namespace explorgdb;

TEST(CapabilityReportTest, SupportedLayerIsReadable) {
    CapabilityReport report;
    report.curve_geometry = {CapabilityState::Supported, "none"};
    report.multipatch = {CapabilityState::Supported, "none"};
    report.raster = {CapabilityState::Supported, "none"};
    EXPECT_TRUE(report.can_read_layer());
}

TEST(CapabilityReportTest, UnsupportedGeometryBlocksLayer) {
    CapabilityReport report;
    report.curve_geometry = {CapabilityState::Supported, "none"};
    report.multipatch = {CapabilityState::Unsupported, "multipatch unsupported"};
    report.raster = {CapabilityState::Supported, "none"};
    EXPECT_FALSE(report.can_read_layer());
}

TEST(CapabilityReportTest, DegradedRasterAndCurveRemainReadable) {
    CapabilityReport report;
    report.curve_geometry = {CapabilityState::Degraded, "curves are explicit"};
    report.multipatch = {CapabilityState::Supported, "standard WKT"};
    report.raster = {CapabilityState::Degraded, "raster pixels are not read"};
    EXPECT_TRUE(report.can_read_layer());
}

TEST(CapabilityReportTest, StateNamesAreStable) {
    EXPECT_STREQ(capability_state_name(CapabilityState::Supported), "supported");
    EXPECT_STREQ(capability_state_name(CapabilityState::Degraded), "degraded");
    EXPECT_STREQ(capability_state_name(CapabilityState::Unsupported), "unsupported");
}
