#include <gtest/gtest.h>
#include "field_layout.h"

using namespace explorgdb;

TEST(FieldLayoutTest, DateTimeWithOffsetUsesTenPhysicalBytes) {
    EXPECT_EQ(fixed_physical_width(FieldType::DateTimeWithOffset), 10u);
}

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

TEST(FieldLayoutTest, ObjectIdConsumesNoBytes) {
    const uint8_t row[] = {0x2a};
    BinaryReader reader(row, sizeof(row));
    ASSERT_TRUE(skip_field_value(reader, FieldType::ObjectId));
    EXPECT_EQ(reader.tell(), 0u);
    EXPECT_EQ(reader.read_u8(), 0x2a);
}
