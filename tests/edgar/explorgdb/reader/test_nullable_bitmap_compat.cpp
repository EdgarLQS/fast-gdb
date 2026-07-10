#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

#define private public
#include "gdb_table.h"
#undef private

using namespace explorgdb;

namespace {

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8U));
    out.push_back(static_cast<uint8_t>(value >> 16U));
    out.push_back(static_cast<uint8_t>(value >> 24U));
}

void append_i32(std::vector<uint8_t>& out, int32_t value) {
    append_u32(out, static_cast<uint32_t>(value));
}

FieldDescriptor nullable_int_field(const std::string& name) {
    FieldDescriptor field;
    field.name = name;
    field.type = FieldType::Int32;
    field.flag = 1;
    return field;
}

FieldDescriptor object_id_field() {
    FieldDescriptor field;
    field.name = "OBJECTID";
    field.type = FieldType::ObjectId;
    return field;
}

void append_row_at_offset_one(GdbTableParser& parser,
                              const std::vector<uint8_t>& payload) {
    parser.file_data_.push_back(0xff);
    append_u32(parser.file_data_, static_cast<uint32_t>(payload.size()));
    parser.file_data_.insert(parser.file_data_.end(), payload.begin(), payload.end());
    parser.feature_offsets_ = {1};
    parser.file_size_ = parser.file_data_.size();
}

void prepare_expanded_nullable_schema_record(GdbTableParser& parser,
                                             bool write_current_bitmap) {
    parser.fields_.push_back(object_id_field());
    for (int i = 0; i < 9; ++i)
        parser.fields_.push_back(nullable_int_field("value_" + std::to_string(i)));

    std::vector<uint8_t> payload;
    payload.push_back(0x00);
    if (write_current_bitmap) payload.push_back(0x00);
    const int value_count = write_current_bitmap ? 9 : 7;
    for (int i = 0; i < value_count; ++i)
        append_i32(payload, 100 + i);

    append_row_at_offset_one(parser, payload);
}

void expect_expanded_nullable_values(const FeatureRecord& record) {
    ASSERT_EQ(record.field_values.size(), 10u);
    ASSERT_TRUE(std::holds_alternative<int32_t>(record.field_values[0]));
    EXPECT_EQ(std::get<int32_t>(record.field_values[0]), 1);
    for (int i = 0; i < 7; ++i) {
        ASSERT_TRUE(std::holds_alternative<int32_t>(record.field_values[1 + i]));
        EXPECT_EQ(std::get<int32_t>(record.field_values[1 + i]), 100 + i);
    }
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(record.field_values[8]));
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(record.field_values[9]));
}

int32_t ref_i32(const FieldRef& ref) {
    int32_t value = 0;
    std::memcpy(&value, ref.data, sizeof(value));
    return value;
}

} // namespace

TEST(NullableBitmapCompatTest, ReadRecordTreatsMissingAddedNullableFieldsAsNull) {
    GdbTableParser parser("unused");
    prepare_expanded_nullable_schema_record(parser, false);

    FeatureRecord record;
    ASSERT_TRUE(parser.read_record_by_fid(0, record));
    expect_expanded_nullable_values(record);
    ASSERT_EQ(record.nullable_flags.size(), 1u);
}

TEST(NullableBitmapCompatTest, ParseRecordsUsesSameCompatLayout) {
    GdbTableParser parser("unused");
    prepare_expanded_nullable_schema_record(parser, false);

    ASSERT_TRUE(parser.parse_records());
    ASSERT_EQ(parser.records().size(), 1u);
    expect_expanded_nullable_values(parser.records()[0]);
}

TEST(NullableBitmapCompatTest, SequentialScanKeepsOldFieldsAligned) {
    GdbTableParser parser("unused");
    prepare_expanded_nullable_schema_record(parser, false);
    parser.mapped_data_ = parser.file_data_.data();

    bool callback_called = false;
    const uint64_t scanned = parser.sequential_scan(
        [&](uint32_t fid, const FieldRef* fields, int field_count) {
            callback_called = true;
            EXPECT_EQ(fid, 0u);
            EXPECT_EQ(field_count, 10);
            EXPECT_EQ(fields[0].implicit_value, 1);
            for (int i = 0; i < 7; ++i) {
                EXPECT_FALSE(fields[1 + i].is_null);
                EXPECT_NE(fields[1 + i].data, nullptr);
                if (fields[1 + i].data != nullptr)
                    EXPECT_EQ(ref_i32(fields[1 + i]), 100 + i);
            }
            EXPECT_TRUE(fields[8].is_null);
            EXPECT_TRUE(fields[9].is_null);
            return true;
        });

    parser.mapped_data_ = nullptr;
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(scanned, 1u);
}

TEST(NullableBitmapCompatTest, CurrentTwoByteBitmapStillReadsAllFields) {
    GdbTableParser parser("unused");
    prepare_expanded_nullable_schema_record(parser, true);

    FeatureRecord record;
    ASSERT_TRUE(parser.read_record_by_fid(0, record));
    ASSERT_EQ(record.field_values.size(), 10u);
    for (int i = 0; i < 9; ++i) {
        ASSERT_TRUE(std::holds_alternative<int32_t>(record.field_values[1 + i]));
        EXPECT_EQ(std::get<int32_t>(record.field_values[1 + i]), 100 + i);
    }
    ASSERT_EQ(record.nullable_flags.size(), 2u);
}

TEST(NullableBitmapCompatTest, PeekGeometryUsesCompatBitmapWidth) {
    GdbTableParser parser("unused");
    parser.fields_.push_back(object_id_field());
    for (int i = 0; i < 7; ++i)
        parser.fields_.push_back(nullable_int_field("value_" + std::to_string(i)));

    FieldDescriptor geometry;
    geometry.name = "Shape";
    geometry.type = FieldType::Geometry;
    geometry.xyscale = 1.0;
    parser.fields_.push_back(geometry);
    parser.geometry_field_index_ = 8;

    parser.fields_.push_back(nullable_int_field("new_value_0"));
    parser.fields_.push_back(nullable_int_field("new_value_1"));

    std::vector<uint8_t> payload;
    payload.push_back(0x00);
    for (int i = 0; i < 7; ++i) append_i32(payload, 100 + i);
    payload.push_back(0x01);
    payload.push_back(0x00);
    append_row_at_offset_one(parser, payload);

    const uint8_t* blob = nullptr;
    size_t size = 0;
    ASSERT_TRUE(parser.peek_geometry_blob(0, blob, size));
    ASSERT_NE(blob, nullptr);
    ASSERT_EQ(size, 1u);
    EXPECT_EQ(blob[0], 0x00);
}
