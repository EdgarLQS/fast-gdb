#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <functional>
#include <shared_mutex>
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

void prepare_datetime_before_geometry(GdbTableParser& parser) {
    FieldDescriptor object_id;
    object_id.name = "OBJECTID";
    object_id.type = FieldType::ObjectId;

    FieldDescriptor timestamp;
    timestamp.name = "captured_at";
    timestamp.type = FieldType::DateTimeWithOffset;

    FieldDescriptor geometry;
    geometry.name = "Shape";
    geometry.type = FieldType::Geometry;
    geometry.xyscale = 1.0;

    parser.fields_ = {object_id, timestamp, geometry};
    parser.geometry_field_index_ = 2;
    parser.geometry_nullable_bit_index_ = 0;

    // Offset zero is reserved by the parser for a deleted row, so place the row at byte 1.
    parser.file_data_.push_back(0xff);
    constexpr uint32_t payload_size = 12; // datetime(8) + offset(2) + geom length(1) + geom(1)
    append_u32(parser.file_data_, payload_size);

    const double date_value = 45200.5;
    const auto* date_bytes = reinterpret_cast<const uint8_t*>(&date_value);
    parser.file_data_.insert(parser.file_data_.end(), date_bytes, date_bytes + sizeof(date_value));
    parser.file_data_.push_back(0x3c); // +60 minutes, little endian int16
    parser.file_data_.push_back(0x00);
    parser.file_data_.push_back(0x01); // geometry blob length
    parser.file_data_.push_back(0x00); // Null geometry type

    parser.feature_offsets_ = {1};
    parser.file_size_ = parser.file_data_.size();
}

} // namespace

TEST(DateTimeWithOffsetBeforeGeometry_ReadRecord, ConsumesOffsetBeforeGeometry) {
    GdbTableParser parser("unused");
    prepare_datetime_before_geometry(parser);

    FeatureRecord record;
    ASSERT_TRUE(parser.read_record_by_fid(0, record));
    ASSERT_EQ(record.field_values.size(), 3u);
    ASSERT_TRUE(std::holds_alternative<DateTimeOffsetValue>(record.field_values[1]));
    const auto value = std::get<DateTimeOffsetValue>(record.field_values[1]);
    EXPECT_DOUBLE_EQ(value.date, 45200.5);
    EXPECT_EQ(value.offset_minutes, 60);
}

TEST(DateTimeWithOffsetBeforeGeometry_PeekGeometry, UsesCanonicalTenByteSkip) {
    GdbTableParser parser("unused");
    prepare_datetime_before_geometry(parser);

    const uint8_t* blob = nullptr;
    size_t size = 0;
    ASSERT_TRUE(parser.peek_geometry_blob(0, blob, size));
    ASSERT_NE(blob, nullptr);
    ASSERT_EQ(size, 1u);
    EXPECT_EQ(blob[0], 0x00);
}

TEST(DateTimeWithOffsetBeforeGeometry_SequentialScan, KeepsGeometryFieldAligned) {
    GdbTableParser parser("unused");
    prepare_datetime_before_geometry(parser);

    parser.mapped_data_ = parser.file_data_.data();
    bool callback_called = false;
    const uint64_t scanned = parser.sequential_scan(
        [&](uint32_t fid, const FieldRef* fields, int field_count) {
            callback_called = true;
            EXPECT_EQ(fid, 0u);
            EXPECT_EQ(field_count, 3);
            EXPECT_EQ(fields[1].type, FieldType::DateTimeWithOffset);
            EXPECT_EQ(fields[1].byte_len, 10u);
            EXPECT_DOUBLE_EQ(fields[1].as_f64(), 45200.5);
            EXPECT_EQ(fields[1].as_datetime_offset_minutes(), 60);
            EXPECT_EQ(fields[2].type, FieldType::Geometry);
            EXPECT_EQ(fields[2].byte_len, 1u);
            if (fields[2].data != nullptr) EXPECT_EQ(fields[2].data[0], 0x00);
            return true;
        });

    // mapped_data_ aliases vector memory in this synthetic test; prevent destructor munmap.
    parser.mapped_data_ = nullptr;
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(scanned, 1u);
}

TEST(DateTimeWithOffsetBeforeGeometry_GeometryOnlyScan,
     ReturnsZeroWhenAnyRowIsTruncated) {
    GdbTableParser parser("unused");
    prepare_datetime_before_geometry(parser);

    parser.feature_offsets_.push_back(parser.file_size_ - 1);
    parser.mapped_data_ = parser.file_data_.data();
    const uint64_t scanned = parser.scan_geometry_blobs(
        [](uint32_t, const uint8_t*, size_t, bool) { return true; });

    parser.mapped_data_ = nullptr;
    EXPECT_EQ(scanned, 0u);
}
