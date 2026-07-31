// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>
#include "query_engine.h"

using namespace explorgdb;

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryEngineTest, SequentialScanRequestIsDefault) {
    QueryRequest request;
    EXPECT_EQ(request.kind, QueryKind::SequentialScan);
    EXPECT_EQ(request.attr_op, AttrOp::Eq);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryEngineTest, WhereClauseRequestCarriesText) {
    QueryRequest request;
    request.kind = QueryKind::WhereClause;
    request.where_clause = "value >= 3 AND name = 'abc'";
    EXPECT_EQ(request.kind, QueryKind::WhereClause);
    EXPECT_EQ(request.where_clause, "value >= 3 AND name = 'abc'");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryEngineTest, MissingSpatialIndexFallsBack) {
    EXPECT_TRUE(QueryEngine::should_fallback_spatial_index(false, false));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryEngineTest, UnparseableSpatialIndexFallsBack) {
    EXPECT_TRUE(QueryEngine::should_fallback_spatial_index(true, false));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(QueryEngineTest, ValidSpatialIndexDoesNotFallbackEvenForEmptyQuery) {
    EXPECT_FALSE(QueryEngine::should_fallback_spatial_index(true, true));
}
