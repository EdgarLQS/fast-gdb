// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include "gdb_spatial_index.h"
#include "test_paths.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace explorgdb;

namespace {

const std::string kLargeSpx =
    explorgdb_test_paths::test_data_path(
        "test_data/large/large_test.gdb/a00000009.spx").string();

} // namespace

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(SpatialIndexMergeTest, FullHeightMergedRangeContainsStandardCandidates) {
    GdbSpatialIndexParser parser(kLargeSpx);
    ASSERT_TRUE(parser.parse());

    const std::vector<double> grid_resolutions = {400.0};
    const auto standard = parser.query_bbox(
        50000.0, 0.0, 50100.0, 100000.0,
        -400.0, -400.0, 1e9, grid_resolutions,
        0U, false);
    const auto merged = parser.query_bbox(
        50000.0, 0.0, 50100.0, 100000.0,
        -400.0, -400.0, 1e9, grid_resolutions,
        0U, true);

    ASSERT_FALSE(standard.empty());
    ASSERT_FALSE(merged.empty());
    EXPECT_TRUE(std::is_sorted(standard.begin(), standard.end()));
    EXPECT_TRUE(std::is_sorted(merged.begin(), merged.end()));
    EXPECT_TRUE(std::includes(
        merged.begin(), merged.end(),
        standard.begin(), standard.end()));
}
