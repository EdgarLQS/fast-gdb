// src/edgar/explorgdb/reader/format/gdb_table_record.cpp
// WKB-first 记录读取 — 完整物化普通字段并跳过几何 blob。

#include "gdb_table.h"

#include "field_layout.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace explorgdb {
namespace {

size_t nullable_bitmap_bytes_for(int nullable_count) {
    return static_cast<size_t>((nullable_count + 7) / 8);
}

bool nullable_bit_is_set(const std::vector<uint8_t>& bitmap,
                         int nullable_bit) {
    const int byte_index = nullable_bit / 8;
    const int bit_index = nullable_bit % 8;
    return byte_index < static_cast<int>(bitmap.size()) &&
           ((bitmap[static_cast<size_t>(byte_index)] >> bit_index) & 1U) != 0;
}

bool is_zero_padding(const uint8_t* cursor, const uint8_t* end) {
    while (cursor < end) {
        if (*cursor++ != 0) return false;
    }
    return true;
}

bool read_fixed_field_value(BinaryReader& reader,
                            FieldType type,
                            uint32_t implicit_object_id,
                            FieldValue& value) {
    if (type == FieldType::ObjectId) {
        value = static_cast<int32_t>(implicit_object_id);
        return true;
    }

    const size_t width = fixed_physical_width(type);
    if (width == 0 || !reader.can_read(width)) return false;

    switch (type) {
        case FieldType::Int16:
            value = static_cast<int32_t>(reader.read_i16());
            return true;
        case FieldType::Int32:
            value = reader.read_i32();
            return true;
        case FieldType::Int64:
            value = reader.read_i64();
            return true;
        case FieldType::Float32:
            value = static_cast<double>(reader.read_f32());
            return true;
        case FieldType::Float64:
        case FieldType::DateTime:
        case FieldType::Date:
        case FieldType::Time:
            value = reader.read_f64();
            return true;
        case FieldType::DateTimeWithOffset:
            value = DateTimeOffsetValue{reader.read_f64(), reader.read_i16()};
            return true;
        case FieldType::UUID_1:
        case FieldType::UUID_2: {
            auto bytes = reader.read_bytes(width);
            std::reverse(bytes.begin(), bytes.begin() + 4);
            std::reverse(bytes.begin() + 4, bytes.begin() + 6);
            std::reverse(bytes.begin() + 6, bytes.begin() + 8);
            char uuid[37];
            std::snprintf(
                uuid, sizeof(uuid),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                "%02x%02x%02x%02x%02x%02x",
                bytes[0], bytes[1], bytes[2], bytes[3],
                bytes[4], bytes[5], bytes[6], bytes[7],
                bytes[8], bytes[9], bytes[10], bytes[11],
                bytes[12], bytes[13], bytes[14], bytes[15]);
            value = std::string(uuid);
            return true;
        }
        default:
            return false;
    }
}

bool materialize_zero_length_record(
    uint32_t fid,
    const std::vector<FieldDescriptor>& fields,
    FeatureRecord& record) {
    record = FeatureRecord{};
    record.fid = fid;

    size_t nullable_count = 0;
    for (const FieldDescriptor& field : fields) {
        if ((field.flag & 1U) != 0) ++nullable_count;
    }
    record.nullable_flags.assign((nullable_count + 7U) / 8U, 0U);
    record.field_values.reserve(fields.size());

    size_t nullable_bit = 0;
    for (const FieldDescriptor& field : fields) {
        const bool nullable = (field.flag & 1U) != 0;
        const size_t current_nullable_bit = nullable_bit;
        if (nullable) ++nullable_bit;

        if (field.type == FieldType::ObjectId) {
            record.field_values.push_back(static_cast<int32_t>(fid + 1U));
            continue;
        }
        if (field.type == FieldType::Geometry) {
            record.field_values.push_back(std::string{});
            if (nullable) {
                record.nullable_flags[current_nullable_bit / 8U] |=
                    static_cast<uint8_t>(1U << (current_nullable_bit % 8U));
            }
            continue;
        }
        if (!nullable) return false;

        record.nullable_flags[current_nullable_bit / 8U] |=
            static_cast<uint8_t>(1U << (current_nullable_bit % 8U));
        record.field_values.push_back(nullptr);
    }
    return true;
}

bool parse_record_without_geometry(
    const uint8_t* row_data,
    size_t row_size,
    uint32_t fid,
    const std::vector<FieldDescriptor>& fields,
    int nullable_count,
    FeatureRecord& record) {
    if (row_size == 0) {
        return materialize_zero_length_record(fid, fields, record);
    }

    const size_t max_bitmap_bytes =
        nullable_bitmap_bytes_for(nullable_count);
    FeatureRecord best_record;
    size_t best_padding = row_size + 1U;
    bool found = false;

    for (size_t bitmap_bytes = max_bitmap_bytes;; --bitmap_bytes) {
        if (bitmap_bytes <= row_size) {
            const int max_present_bits = std::min(
                nullable_count, static_cast<int>(bitmap_bytes * 8U));
            for (int present_bits = max_present_bits;
                 present_bits >= 0;
                 --present_bits) {
                FeatureRecord candidate;
                candidate.fid = fid;
                candidate.blob_len = static_cast<uint32_t>(row_size);

                try {
                    BinaryReader reader(row_data, row_size);
                    if (bitmap_bytes > 0) {
                        candidate.nullable_flags =
                            reader.read_bytes(bitmap_bytes);
                    }
                    candidate.field_values.reserve(fields.size());

                    int nullable_bit = 0;
                    bool valid = true;
                    for (const FieldDescriptor& field : fields) {
                        bool is_null = false;
                        if ((field.flag & 1U) != 0) {
                            is_null = nullable_bit >= present_bits ||
                                nullable_bit_is_set(
                                    candidate.nullable_flags, nullable_bit);
                            ++nullable_bit;
                        }

                        // Geometry 槽始终为空字符串占位；NULL/Empty/错误状态由
                        // GeometryValue 独立表达，record 不尝试推断。
                        if (field.type == FieldType::Geometry) {
                            if (!is_null) {
                                const uint64_t length = reader.read_varuint();
                                if (length >
                                        std::numeric_limits<size_t>::max() ||
                                    !reader.can_read(
                                        static_cast<size_t>(length))) {
                                    valid = false;
                                    break;
                                }
                                reader.skip(static_cast<size_t>(length));
                            }
                            candidate.field_values.push_back(std::string{});
                            continue;
                        }

                        if (is_null) {
                            candidate.field_values.push_back(nullptr);
                            continue;
                        }

                        if (field.type == FieldType::ObjectId ||
                            fixed_physical_width(field.type) != 0) {
                            FieldValue value;
                            if (!read_fixed_field_value(
                                    reader, field.type, fid + 1U, value)) {
                                valid = false;
                                break;
                            }
                            candidate.field_values.push_back(std::move(value));
                            continue;
                        }

                        switch (field.type) {
                            case FieldType::String:
                            case FieldType::XML: {
                                const uint64_t length = reader.read_varuint();
                                if (length >
                                        std::numeric_limits<size_t>::max() ||
                                    !reader.can_read(
                                        static_cast<size_t>(length))) {
                                    valid = false;
                                    break;
                                }
                                const size_t size = static_cast<size_t>(length);
                                std::string text(size, '\0');
                                for (char& byte : text) {
                                    byte = static_cast<char>(reader.read_u8());
                                }
                                candidate.field_values.push_back(
                                    std::move(text));
                                break;
                            }
                            case FieldType::Binary: {
                                const uint64_t length = reader.read_varuint();
                                if (length >
                                        std::numeric_limits<size_t>::max() ||
                                    !reader.can_read(
                                        static_cast<size_t>(length))) {
                                    valid = false;
                                    break;
                                }
                                candidate.field_values.push_back(
                                    reader.read_bytes(
                                        static_cast<size_t>(length)));
                                break;
                            }
                            case FieldType::Raster: {
                                const uint64_t length = reader.read_varuint();
                                if (length >
                                        std::numeric_limits<size_t>::max() ||
                                    !reader.can_read(
                                        static_cast<size_t>(length))) {
                                    valid = false;
                                    break;
                                }
                                reader.skip(static_cast<size_t>(length));
                                candidate.field_values.push_back(nullptr);
                                break;
                            }
                            default:
                                valid = false;
                                break;
                        }
                        if (!valid) break;
                    }

                    if (valid && reader.tell() <= row_size) {
                        const size_t padding = row_size - reader.tell();
                        if (is_zero_padding(row_data + reader.tell(),
                                            row_data + row_size) &&
                            padding < best_padding) {
                            best_padding = padding;
                            best_record = std::move(candidate);
                            found = true;
                            if (padding == 0) break;
                        }
                    }
                } catch (const std::exception&) {
                    // nullable 位图长度存在历史变体；继续尝试下一个候选布局。
                }
            }
        }

        if (found && best_padding == 0) break;
        if (bitmap_bytes == 0) break;
    }

    if (!found) return false;
    record = std::move(best_record);
    return true;
}

} // namespace

bool GdbTableParser::read_record_by_fid(uint32_t fid,
                                        FeatureRecord& record) {
    if (fd_ < 0 && file_data_.empty() && !open()) return false;
    if (fields_.empty() && fd_ >= 0 && !ensure_fields_loaded()) return false;
    if (feature_offsets_.empty() || fid >= feature_offsets_.size()) return false;

    const uint64_t raw_offset = feature_offsets_[fid];
    if (raw_offset == 0 ||
        raw_offset > static_cast<uint64_t>(
                         std::numeric_limits<size_t>::max())) {
        return false;
    }
    const size_t offset = static_cast<size_t>(raw_offset);

    uint32_t blob_length = 0;
    const uint8_t* row_data = nullptr;
    if (mapped_data_ != nullptr) {
        if (offset > file_size_ ||
            file_size_ - offset < sizeof(blob_length)) {
            return false;
        }
        std::memcpy(&blob_length, mapped_data_ + offset,
                    sizeof(blob_length));
        const size_t payload_offset = offset + sizeof(blob_length);
        if (static_cast<size_t>(blob_length) >
            file_size_ - payload_offset) {
            return false;
        }
        row_data = mapped_data_ + payload_offset;
    } else if (fd_ >= 0) {
        if (offset > file_size_ ||
            file_size_ - offset < sizeof(blob_length)) {
            return false;
        }
        uint8_t length_buffer[sizeof(blob_length)];
        if (!read_at(offset, length_buffer, sizeof(length_buffer))) {
            return false;
        }
        BinaryReader length_reader(length_buffer, sizeof(length_buffer));
        blob_length = length_reader.read_u32();
        const size_t payload_offset = offset + sizeof(blob_length);
        if (static_cast<size_t>(blob_length) >
            file_size_ - payload_offset) {
            return false;
        }
        if (row_buffer_.size() < blob_length) {
            row_buffer_.resize(blob_length);
        }
        if (blob_length != 0 &&
            !read_at(payload_offset, row_buffer_.data(), blob_length)) {
            return false;
        }
        row_data = row_buffer_.data();
    } else {
        if (offset > file_data_.size() ||
            file_data_.size() - offset < sizeof(blob_length)) {
            return false;
        }
        BinaryReader length_reader(
            file_data_.data() + offset, file_data_.size() - offset);
        blob_length = length_reader.read_u32();
        const size_t payload_offset = offset + sizeof(blob_length);
        if (static_cast<size_t>(blob_length) >
            file_data_.size() - payload_offset) {
            return false;
        }
        row_data = file_data_.data() + payload_offset;
    }

    if (!parse_record_without_geometry(
            row_data, blob_length, fid, fields_,
            nullable_field_count(), record)) {
        return false;
    }
    record.blob_len = blob_length;
    return true;
}

} // namespace explorgdb
