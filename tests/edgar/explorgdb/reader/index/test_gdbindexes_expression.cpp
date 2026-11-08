// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include "gdb_indexes.h"
#include <filesystem>
#include <fstream>
#include <vector>

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

TEST(GdbIndexesExpressionTest, FailedReadCannotExposeStaleOrPartialMetadata) {
    const auto path = std::filesystem::temp_directory_path() / "fast_gdb_indexes_transaction.gdbindexes";
    const auto put32 = [](std::vector<uint8_t>& data, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) data.push_back(static_cast<uint8_t>(value >> (8 * i)));
    };
    std::vector<uint8_t> valid;
    put32(valid, 1);
    put32(valid, 1); valid.insert(valid.end(), {'I', 0});
    valid.insert(valid.end(), {0, 0}); put32(valid, 2);
    valid.insert(valid.end(), {0, 0}); put32(valid, 0);
    put32(valid, 1); valid.insert(valid.end(), {'A', 0, 0, 0});
    const auto write = [&](const auto& data) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
    };
    GdbIndexesParser parser(path.string());
    for (int damage : {0, 1, 2}) {
        write(valid);
        ASSERT_TRUE(parser.parse());
        ASSERT_EQ(parser.entries().size(), 1U);
        EXPECT_EQ(parser.entries()[0].column_name, "A");
        if (damage == 0) std::filesystem::remove(path);
        else {
            auto bad = valid;
            bad[0] = 2;
            if (damage == 1) put32(bad, 0xffffffffU);
            else { put32(bad, 1); bad.push_back('B'); }
            write(bad);
        }
        EXPECT_FALSE(parser.parse());
        EXPECT_TRUE(parser.entries().empty());
    }
    std::filesystem::remove(path);
}
