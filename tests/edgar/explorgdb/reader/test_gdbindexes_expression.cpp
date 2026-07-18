#include <gtest/gtest.h>

#include "gdb_indexes.h"

using namespace explorgdb;

TEST(GdbIndexesExpressionTest, DirectFieldRemainsDirect) {
    EXPECT_EQ(GdbIndexesParser::field_name_from_expression("Name"),
              "Name");
    EXPECT_TRUE(GdbIndexesParser::is_direct_field_expression("Name"));
}

TEST(GdbIndexesExpressionTest, LowerExpressionMapsFieldButIsNotDirect) {
    EXPECT_EQ(GdbIndexesParser::field_name_from_expression("LOWER(Name)"),
              "Name");
    EXPECT_EQ(GdbIndexesParser::field_name_from_expression("lower(Name)"),
              "Name");
    EXPECT_FALSE(
        GdbIndexesParser::is_direct_field_expression("LOWER(Name)"));
}

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
