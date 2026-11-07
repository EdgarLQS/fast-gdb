// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <utility>
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
    parser.feature_offsets_ = std::make_shared<const std::vector<uint64_t>>(
        std::vector<uint64_t>{1});
    parser.logical_row_count_ = 1;
    parser.active_feature_fids_ = {0};
    parser.active_feature_count_ = 1;
    parser.active_feature_count_known_ = true;
    parser.file_size_ = parser.file_data_.size();
}

void prepare_zero_length_row(GdbTableParser& parser,
                             std::vector<FieldDescriptor> fields) {
    parser.fields_ = std::move(fields);
    parser.geometry_field_index_ = -1;
    for (size_t index = 0; index < parser.fields_.size(); ++index) {
        if (parser.fields_[index].type == FieldType::Geometry) {
            parser.geometry_field_index_ = static_cast<int>(index);
            break;
        }
    }
    append_row_at_offset_one(parser, {});
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

int32_t stored_value(RowSlot row, size_t field_index) {
    const uint32_t byte1 = static_cast<uint32_t>(row % 254U) + 1U;
    const uint32_t byte2 = static_cast<uint32_t>(field_index % 254U) + 1U;
    const uint32_t byte3 =
        static_cast<uint32_t>((row + field_index) % 254U) + 1U;
    return static_cast<int32_t>(0x11000000U | (byte1 << 16U) |
                                (byte2 << 8U) | byte3);
}

void prepare_nullable_table(GdbTableParser& parser,
                            size_t current_nullable_count,
                            size_t stored_nullable_count,
                            size_t row_count) {
    parser.fields_.clear();
    parser.fields_.push_back(object_id_field());
    for (size_t index = 0; index < current_nullable_count; ++index) {
        parser.fields_.push_back(nullable_int_field(
            "value_" + std::to_string(index)));
    }

    parser.file_data_.clear();
    parser.file_data_.push_back(0xff);
    parser.active_feature_fids_.clear();
    parser.feature_block_bitmap_.clear();
    parser.block_rank_prefix_.clear();
    parser.active_feature_fids_.reserve(row_count);
    std::vector<uint64_t> feature_offsets;
    feature_offsets.reserve(row_count);

    for (size_t row = 0; row < row_count; ++row) {
        std::vector<uint8_t> payload(
            (current_nullable_count + 7U) / 8U, 0U);
        for (size_t field = stored_nullable_count;
             field < payload.size() * 8U;
             ++field) {
            payload[field / 8U] |=
                static_cast<uint8_t>(1U << (field % 8U));
        }
        for (size_t field = 0; field < stored_nullable_count; ++field) {
            append_i32(payload, stored_value(row, field));
        }
        feature_offsets.push_back(parser.file_data_.size());
        append_u32(parser.file_data_, static_cast<uint32_t>(payload.size()));
        parser.file_data_.insert(
            parser.file_data_.end(), payload.begin(), payload.end());
        parser.active_feature_fids_.push_back(static_cast<RowSlot>(row));
    }

    parser.logical_row_count_ = row_count;
    parser.feature_offsets_ = std::make_shared<const std::vector<uint64_t>>(
        std::move(feature_offsets));
    parser.active_feature_count_ = row_count;
    parser.active_feature_count_known_ = true;
    parser.file_size_ = parser.file_data_.size();
}

bool record_matches_expanded_schema(const FeatureRecord& record,
                                    RowSlot row,
                                    size_t current_nullable_count,
                                    size_t stored_nullable_count) {
    if (record.field_values.size() != current_nullable_count + 1U ||
        record.nullable_flags.size() !=
            (current_nullable_count + 7U) / 8U) {
        return false;
    }
    for (size_t field = 0; field < stored_nullable_count; ++field) {
        const auto* value = std::get_if<int32_t>(
            &record.field_values[field + 1U]);
        if (value == nullptr || *value != stored_value(row, field))
            return false;
    }
    for (size_t field = stored_nullable_count;
         field < current_nullable_count;
         ++field) {
        if (!std::holds_alternative<std::nullptr_t>(
                record.field_values[field + 1U])) {
            return false;
        }
    }
    return true;
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

struct MappedDataReset {
    GdbTableParser& parser;

    ~MappedDataReset() {
        parser.mapped_data_ = nullptr;
    }
};

} // namespace

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(NullableBitmapCompatTest, ReadRecordTreatsMissingAddedNullableFieldsAsNull) {
    GdbTableParser parser("unused");
    prepare_expanded_nullable_schema_record(parser, false);

    FeatureRecord record;
    ASSERT_TRUE(parser.read_record_by_fid(0, record));
    expect_expanded_nullable_values(record);
    ASSERT_EQ(record.nullable_flags.size(), 1u);
}

TEST(NullableBitmapCompatTest,
     ZeroLengthRowHasConsistentObjectIdNullAndGeometryAcrossReadPaths) {
    GdbTableParser parser("unused");
    FieldDescriptor integer = nullable_int_field("value");
    FieldDescriptor binary;
    binary.name = "payload";
    binary.type = FieldType::Binary;
    binary.flag = 1;
    FieldDescriptor geometry;
    geometry.name = "Shape";
    geometry.type = FieldType::Geometry;
    geometry.flag = 1;
    geometry.xyscale = 1.0;
    prepare_zero_length_row(
        parser, {object_id_field(), integer, binary, geometry});

    FeatureRecord record;
    ASSERT_TRUE(parser.read_record_by_fid(0, record));
    ASSERT_EQ(record.field_values.size(), 4u);
    ASSERT_TRUE(std::holds_alternative<int32_t>(record.field_values[0]));
    EXPECT_EQ(std::get<int32_t>(record.field_values[0]), 1);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(record.field_values[1]));
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(record.field_values[2]));
    ASSERT_TRUE(std::holds_alternative<std::string>(record.field_values[3]));
    EXPECT_TRUE(std::get<std::string>(record.field_values[3]).empty());
    ASSERT_EQ(record.nullable_flags.size(), 1u);
    EXPECT_EQ(record.nullable_flags[0], 0x07U);

    parser.mapped_data_ = parser.file_data_.data();
    std::vector<uint8_t> binary_value;
    size_t binary_size = 0;
    bool binary_is_null = false;
    bool binary_limit_exceeded = false;
    ASSERT_TRUE(parser.read_binary_field_by_fid(
        0, 2, 16, binary_value, binary_size,
        binary_is_null, binary_limit_exceeded));
    EXPECT_TRUE(binary_is_null);
    EXPECT_FALSE(binary_limit_exceeded);
    EXPECT_EQ(binary_size, 0u);
    EXPECT_TRUE(binary_value.empty());

    FeatureRecord one_pass_record;
    GeometryValue one_pass_geometry;
    ASSERT_TRUE(parser.read_feature_by_fid(
        0, one_pass_record, one_pass_geometry, nullptr, nullptr,
        GeometryReadOptions::preserve_source(), true));
    ASSERT_EQ(one_pass_record.field_values.size(), 4u);
    EXPECT_TRUE(std::holds_alternative<int32_t>(
        one_pass_record.field_values[0]));
    EXPECT_EQ(std::get<int32_t>(one_pass_record.field_values[0]), 1);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
        one_pass_record.field_values[1]));
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
        one_pass_record.field_values[2]));
    ASSERT_TRUE(std::holds_alternative<std::string>(
        one_pass_record.field_values[3]));
    EXPECT_TRUE(std::get<std::string>(
        one_pass_record.field_values[3]).empty());
    EXPECT_EQ(one_pass_geometry.status, GeometryStatus::Null);
    EXPECT_TRUE(one_pass_geometry.wkb.empty());

    GeometryValue geometry_value;
    ASSERT_TRUE(parser.read_geometry_value(0, geometry_value));
    EXPECT_EQ(geometry_value.status, GeometryStatus::Null);
    EXPECT_TRUE(geometry_value.wkb.empty());

    const uint8_t* geometry_blob = nullptr;
    size_t geometry_blob_size = 0;
    bool geometry_is_null = false;
    ASSERT_TRUE(parser.peek_geometry_blob(
        0, geometry_blob, geometry_blob_size, &geometry_is_null));
    EXPECT_TRUE(geometry_is_null);
    EXPECT_EQ(geometry_blob, nullptr);
    EXPECT_EQ(geometry_blob_size, 0u);

    auto expect_scan_refs = [](uint32_t fid,
                               const FieldRef* refs,
                               int field_count) {
        EXPECT_EQ(fid, 0u);
        EXPECT_EQ(field_count, 4);
        if (field_count != 4) return false;
        EXPECT_EQ(refs[0].type, FieldType::ObjectId);
        EXPECT_FALSE(refs[0].is_null);
        EXPECT_EQ(refs[0].data, nullptr);
        EXPECT_EQ(refs[0].implicit_value, 1);
        for (int index = 1; index < field_count; ++index) {
            EXPECT_TRUE(refs[index].is_null);
            EXPECT_EQ(refs[index].data, nullptr);
        }
        return true;
    };
    EXPECT_EQ(parser.sequential_scan(expect_scan_refs), 1u);
    EXPECT_EQ(parser.scan_field_candidates(
                  parser.active_feature_fids_, expect_scan_refs), 1u);
    parser.mapped_data_ = nullptr;
}

TEST(NullableBitmapCompatTest,
     ZeroLengthRowWithRequiredAttributeFailsClosedAcrossScans) {
    GdbTableParser parser("unused");
    FieldDescriptor required;
    required.name = "required_value";
    required.type = FieldType::Int32;
    prepare_zero_length_row(parser, {object_id_field(), required});

    FeatureRecord record;
    EXPECT_FALSE(parser.read_record_by_fid(0, record));
    FeatureRecord one_pass_record;
    GeometryValue geometry;
    EXPECT_FALSE(parser.read_feature_by_fid(
        0, one_pass_record, geometry, nullptr, nullptr,
        GeometryReadOptions::preserve_source(), false));

    parser.mapped_data_ = parser.file_data_.data();
    bool callback_called = false;
    auto callback = [&](uint32_t, const FieldRef*, int) {
        callback_called = true;
        return true;
    };
    EXPECT_EQ(parser.sequential_scan(callback), 0u);
    EXPECT_EQ(parser.scan_field_candidates(
                  parser.active_feature_fids_, callback), 0u);
    EXPECT_FALSE(callback_called);
    parser.mapped_data_ = nullptr;
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(NullableBitmapCompatTest, ParseRecordsUsesSameCompatLayout) {
    GdbTableParser parser("unused");
    prepare_expanded_nullable_schema_record(parser, false);

    ASSERT_TRUE(parser.parse_records());
    ASSERT_EQ(parser.records().size(), 1u);
    expect_expanded_nullable_values(parser.records()[0]);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

TEST(NullableBitmapCompatTest,
     WideCurrentBitmapsKeepOriginalValuesAndNullNewFields) {
    constexpr size_t kStoredNullableCount = 7;
    constexpr size_t kRowCount = 3;
    const size_t current_counts[] = {17, 33, 65, 129};

    for (size_t current_count : current_counts) {
        GdbTableParser parser("unused");
        prepare_nullable_table(parser, current_count,
                              kStoredNullableCount, kRowCount);

        for (RowSlot row = 0; row < kRowCount; ++row) {
            FeatureRecord record;
            ASSERT_TRUE(parser.read_record_by_fid(row, record))
                << "current nullable count=" << current_count
                << ", row=" << row;
            EXPECT_TRUE(record_matches_expanded_schema(
                record, row, current_count, kStoredNullableCount))
                << "current nullable count=" << current_count
                << ", row=" << row;

            FeatureRecord one_pass_record;
            GeometryValue geometry;
            ASSERT_TRUE(parser.read_feature_by_fid(
                row, one_pass_record, geometry, nullptr, nullptr,
                GeometryReadOptions::preserve_source(), false));
            EXPECT_TRUE(record_matches_expanded_schema(
                one_pass_record, row, current_count,
                kStoredNullableCount));
        }

        parser.mapped_data_ = parser.file_data_.data();
        auto validate_refs = [&](uint32_t fid,
                                 const FieldRef* refs,
                                 int field_count) {
            if (field_count != static_cast<int>(current_count + 1U) ||
                fid >= kRowCount) {
                return false;
            }
            for (size_t field = 0;
                 field < kStoredNullableCount;
                 ++field) {
                const FieldRef& ref = refs[field + 1U];
                int32_t value = 0;
                if (ref.is_null || ref.data == nullptr ||
                    ref.byte_len != sizeof(value)) {
                    return false;
                }
                std::memcpy(&value, ref.data, sizeof(value));
                if (value != stored_value(fid, field)) return false;
            }
            for (size_t field = kStoredNullableCount;
                 field < current_count;
                 ++field) {
                if (!refs[field + 1U].is_null ||
                    refs[field + 1U].data != nullptr) {
                    return false;
                }
            }
            return true;
        };

        uint64_t scanned = parser.sequential_scan(validate_refs);
        EXPECT_EQ(scanned, kRowCount)
            << "sequential scan, nullable count=" << current_count;

        scanned = parser.scan_field_candidates(
            parser.active_feature_fids_, validate_refs);
        parser.mapped_data_ = nullptr;
        EXPECT_EQ(scanned, kRowCount)
            << "candidate scan, nullable count=" << current_count;
    }
}

TEST(NullableBitmapCompatTest, LargeCurrentBitmapReadBenchmark) {
    const char* benchmark_enabled =
        std::getenv("FAST_GDB_RUN_NULLABLE_BITMAP_BENCHMARK");
    if (benchmark_enabled == nullptr ||
        std::string(benchmark_enabled) != "1") {
        GTEST_SKIP() << "Set FAST_GDB_RUN_NULLABLE_BITMAP_BENCHMARK=1 to run";
    }

    constexpr size_t kRowCount = 100000;
    constexpr size_t kStoredNullableCount = 7;
    constexpr int kMeasuredRounds = 5;
    const size_t current_counts[] = {33, 129};

    for (size_t current_count : current_counts) {
        GdbTableParser parser("unused");
        prepare_nullable_table(parser, current_count,
                              kStoredNullableCount, kRowCount);
        parser.mapped_data_ = parser.file_data_.data();
        const MappedDataReset mapped_data_reset{parser};

        auto direct_pass = [&]() -> uint64_t {
            uint64_t checksum = 0;
            for (RowSlot row = 0; row < kRowCount; ++row) {
                FeatureRecord record;
                if (!parser.read_record_by_fid(row, record) ||
                    record.field_values.size() != current_count + 1U ||
                    !std::holds_alternative<int32_t>(
                        record.field_values[1]) ||
                    !std::holds_alternative<std::nullptr_t>(
                        record.field_values.back())) {
                    return 0;
                }
                checksum += static_cast<uint32_t>(
                    std::get<int32_t>(record.field_values[1]));
            }
            return checksum;
        };

        auto refs_pass = [&](bool candidate_scan) -> uint64_t {
            uint64_t checksum = 0;
            auto validate = [&](uint32_t fid,
                                const FieldRef* refs,
                                int field_count) {
                if (fid >= kRowCount ||
                    field_count != static_cast<int>(current_count + 1U) ||
                    refs[1].is_null || refs[1].data == nullptr ||
                    refs[1].byte_len != sizeof(int32_t) ||
                    !refs[field_count - 1].is_null ||
                    refs[field_count - 1].data != nullptr) {
                    return false;
                }
                int32_t value = 0;
                std::memcpy(&value, refs[1].data, sizeof(value));
                if (value != stored_value(fid, 0)) return false;
                checksum += static_cast<uint32_t>(value);
                return true;
            };

            const uint64_t scanned = candidate_scan
                ? parser.scan_field_candidates(
                    parser.active_feature_fids_, validate)
                : parser.sequential_scan(validate);
            return scanned == kRowCount ? checksum : 0;
        };

        const uint64_t expected_checksum =
            direct_pass();
        ASSERT_NE(expected_checksum, 0u)
            << "direct read warm-up failed at nullable count="
            << current_count;
        ASSERT_EQ(refs_pass(false), expected_checksum)
            << "sequential scan warm-up failed at nullable count="
            << current_count;
        ASSERT_EQ(refs_pass(true), expected_checksum)
            << "candidate scan warm-up failed at nullable count="
            << current_count;

        auto median_ms = [](std::vector<double> samples) {
            std::sort(samples.begin(), samples.end());
            return samples[samples.size() / 2U];
        };
        auto measure = [&](auto&& pass) {
            std::vector<double> samples;
            samples.reserve(kMeasuredRounds);
            for (int round = 0; round < kMeasuredRounds; ++round) {
                const auto start = std::chrono::steady_clock::now();
                const uint64_t checksum = pass();
                const auto end = std::chrono::steady_clock::now();
                EXPECT_EQ(checksum, expected_checksum)
                    << "read checksum changed at nullable count="
                    << current_count;
                samples.push_back(
                    std::chrono::duration<double, std::milli>(end - start)
                        .count());
            }
            return median_ms(std::move(samples));
        };

        const double direct_ms = measure(direct_pass);
        const double sequential_ms = measure(
            [&]() { return refs_pass(false); });
        const double candidate_ms = measure(
            [&]() { return refs_pass(true); });
        std::printf(
            "nullable_bitmap_benchmark rows=%zu fields=%zu rounds=%d "
            "direct_median_ms=%.3f sequential_median_ms=%.3f "
            "candidate_median_ms=%.3f\n",
            kRowCount, current_count, kMeasuredRounds,
            direct_ms, sequential_ms, candidate_ms);
    }
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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
