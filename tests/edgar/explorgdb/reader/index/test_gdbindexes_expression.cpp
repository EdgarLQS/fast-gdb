// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include "gdb_indexes.h"

using namespace explorgdb;

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GdbIndexesExpressionTest, DirectFieldRemainsDirect) {
    EXPECT_EQ(GdbIndexesParser::field_name_from_expression("Name"),
              "Name");
    EXPECT_TRUE(GdbIndexesParser::is_direct_field_expression("Name"));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GdbIndexesExpressionTest, LowerExpressionMapsFieldButIsNotDirect) {
    EXPECT_EQ(GdbIndexesParser::field_name_from_expression("LOWER(Name)"),
              "Name");
    EXPECT_EQ(GdbIndexesParser::field_name_from_expression("lower(Name)"),
              "Name");
    EXPECT_FALSE(
        GdbIndexesParser::is_direct_field_expression("LOWER(Name)"));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GdbIndexesExpressionTest, UnknownFunctionsAreNotDirect) {
    EXPECT_EQ(GdbIndexesParser::field_name_from_expression("UPPER(Name)"),
              "UPPER(Name)");
    EXPECT_FALSE(
        GdbIndexesParser::is_direct_field_expression("UPPER(Name)"));
    EXPECT_EQ(GdbIndexesParser::field_name_from_expression("LOWER(Name"),
              "LOWER(Name");
    EXPECT_FALSE(
        GdbIndexesParser::is_direct_field_expression("LOWER(Name"));
}
