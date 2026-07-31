// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>
#include "field_layout.h"

using namespace explorgdb;

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(FieldLayoutTest, DateTimeWithOffsetUsesTenPhysicalBytes) {
    EXPECT_EQ(fixed_physical_width(FieldType::DateTimeWithOffset), 10u);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(FieldLayoutTest, SkipDateTimeWithOffsetKeepsFollowingFieldAligned) {
    const uint8_t row[] = {
        0,0,0,0,0,0,0,0, // double payload
        0x3c,0x00,         // +60 minute offset
        0x78,0x56,0x34,0x12
    };
    BinaryReader reader(row, sizeof(row));
    ASSERT_TRUE(skip_field_value(reader, FieldType::DateTimeWithOffset));
    EXPECT_EQ(reader.tell(), 10u);
    EXPECT_EQ(reader.read_u32(), 0x12345678u);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(FieldLayoutTest, ObjectIdConsumesNoBytes) {
    const uint8_t row[] = {0x2a};
    BinaryReader reader(row, sizeof(row));
    ASSERT_TRUE(skip_field_value(reader, FieldType::ObjectId));
    EXPECT_EQ(reader.tell(), 0u);
    EXPECT_EQ(reader.read_u8(), 0x2a);
}
