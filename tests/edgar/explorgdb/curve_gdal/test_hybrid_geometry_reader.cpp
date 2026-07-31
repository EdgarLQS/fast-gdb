// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include "gdal_curve_backend.h"
#include "hybrid_geometry_reader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

using namespace explorgdb;

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(HybridGeometryContract, MapsFastFidWithExplicitOffset) {
    int64_t mapped = -1;
    EXPECT_TRUE(HybridGeometryReader::map_gdal_fid(0, 1, mapped));
    EXPECT_EQ(mapped, 1);
    EXPECT_TRUE(HybridGeometryReader::map_gdal_fid(42, 0, mapped));
    EXPECT_EQ(mapped, 42);
    EXPECT_FALSE(HybridGeometryReader::map_gdal_fid(0, -1, mapped));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(HybridGeometryContract, RejectsInvalidDatasetWithoutGuessing) {
    GdalCurveRequest request;
    request.gdb_path = "/path/that/does/not/exist.gdb";
    request.layer_name = "missing_layer";
    request.fid = 1;

    GdalCurveBackendBridge bridge;
    const GeometryValue value = bridge.read_geometry(request);
    EXPECT_FALSE(value.valid());
    EXPECT_EQ(value.backend, GeometryBackend::Gdal);
    EXPECT_EQ(value.status, GeometryStatus::InvalidEncoding);
    EXPECT_NE(value.diagnostic.find("could not open"), std::string::npos);

    const auto spatial = bridge.intersects_bbox(
        request, 0.0, 0.0, 1.0, 1.0);
    EXPECT_FALSE(spatial.valid());
    EXPECT_EQ(spatial.backend, GeometryBackend::Gdal);
    EXPECT_EQ(spatial.status, GeometryStatus::InvalidEncoding);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(HybridGeometryContract, RejectsNegativeGdalFid) {
    GdalCurveRequest request;
    request.gdb_path = "/path/that/does/not/exist.gdb";
    request.layer_name = "missing_layer";
    request.fid = -1;

    // Dataset open fails before feature lookup, but a negative FID remains an
    // invalid request contract and is never remapped/guessed by the bridge.
    GdalCurveBackendBridge bridge;
    const auto result = bridge.read_geometry(request);
    EXPECT_FALSE(result.valid());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(HybridGeometryContract, ThreadCacheCanBeClearedRepeatedly) {
    EXPECT_NO_THROW(GdalCurveBackendBridge::clear_thread_cache());
    EXPECT_NO_THROW(GdalCurveBackendBridge::clear_thread_cache());
}
