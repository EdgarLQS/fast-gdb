#include "gdb_table.h"

#include "field_layout.h"
#include "wkb_writer.h"
#include "wkt_writer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace explorgdb {
namespace {

using FeatureReadClock = std::chrono::steady_clock;

struct GeometrySlice {
    bool present = false;
    bool is_null = false;
    size_t field_index = 0;
    size_t offset = 0;
    size_t size = 0;
};

double elapsed_ms(FeatureReadClock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        FeatureReadClock::now() - start).count();
}

FeatureReadClock::time_point metric_start(FeatureReadMetrics* metrics) {
    return metrics == nullptr ? FeatureReadClock::time_point{}
                              : FeatureReadClock::now();
}

void metric_add(FeatureReadMetrics* metrics,
                double FeatureReadMetrics::*member,
                FeatureReadClock::time_point start) {
    if (metrics != nullptr) metrics->*member += elapsed_ms(start);
}

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
    FeatureRecord& record,
    const std::vector<FieldDescriptor>& fields) {
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
            record.field_values.push_back(
                static_cast<int32_t>(record.fid + 1U));
            continue;
        }
        if (!nullable) return false;

        record.nullable_flags[current_nullable_bit / 8U] |=
            static_cast<uint8_t>(1U << (current_nullable_bit % 8U));
        record.field_values.push_back(nullptr);
    }
    return true;
}

} // namespace

bool GdbTableParser::read_feature_by_fid(
    uint32_t fid,
    FeatureRecord& record,
    GeometryValue& geometry,
    FeatureReadMetrics* metrics) {
    if (metrics != nullptr) *metrics = FeatureReadMetrics{};

    const auto lookup_start = metric_start(metrics);
    if (fd_ < 0 && file_data_.empty() && !open()) return false;
    if (fields_.empty() && fd_ >= 0 && !ensure_fields_loaded()) return false;
    if (feature_offsets_.empty() || fid >= feature_offsets_.size()) return false;

    const uint64_t raw_offset = feature_offsets_[fid];
    if (raw_offset == 0 ||
        raw_offset > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    const size_t offset = static_cast<size_t>(raw_offset);

    uint32_t blob_length = 0;
    const uint8_t* row_data = nullptr;
    if (mapped_data_ != nullptr) {
        if (offset > file_size_ || file_size_ - offset < sizeof(blob_length))
            return false;
        std::memcpy(&blob_length, mapped_data_ + offset, sizeof(blob_length));
        if (static_cast<size_t>(blob_length) >
            file_size_ - offset - sizeof(blob_length)) {
            return false;
        }
        row_data = mapped_data_ + offset + sizeof(blob_length);
    } else if (fd_ >= 0) {
        uint8_t length_buffer[sizeof(blob_length)];
        if (!read_at(raw_offset, length_buffer, sizeof(length_buffer))) return false;
        BinaryReader length_reader(length_buffer, sizeof(length_buffer));
        blob_length = length_reader.read_u32();
        if (raw_offset + sizeof(blob_length) > file_size_ ||
            static_cast<uint64_t>(blob_length) >
                file_size_ - static_cast<size_t>(raw_offset + sizeof(blob_length))) {
            return false;
        }
        if (row_buffer_.size() < blob_length) row_buffer_.resize(blob_length);
        if (blob_length != 0 &&
            !read_at(raw_offset + sizeof(blob_length),
                     row_buffer_.data(), blob_length)) {
            return false;
        }
        row_data = row_buffer_.data();
    } else {
        if (offset > file_data_.size() ||
            file_data_.size() - offset < sizeof(blob_length)) {
            return false;
        }
        BinaryReader length_reader(file_data_.data() + offset,
                                   file_data_.size() - offset);
        blob_length = length_reader.read_u32();
        if (static_cast<size_t>(blob_length) >
            file_data_.size() - offset - sizeof(blob_length)) {
            return false;
        }
        row_data = file_data_.data() + offset + sizeof(blob_length);
    }
    metric_add(metrics, &FeatureReadMetrics::row_lookup_ms, lookup_start);

    FeatureRecord output_record;
    output_record.fid = fid;
    output_record.blob_len = blob_length;

    if (blob_length == 0) {
        const auto fields_start = metric_start(metrics);
        if (!materialize_zero_length_record(output_record, fields_)) return false;
        metric_add(metrics, &FeatureReadMetrics::field_materialization_ms,
                   fields_start);

        GeometryValue output_geometry;
        if (geometry_field_index_ < 0) {
            output_geometry.status = GeometryStatus::UnsupportedType;
            output_geometry.diagnostic = "table has no geometry field";
        } else {
            output_geometry.status = GeometryStatus::Empty;
            output_geometry.diagnostic = "geometry is null";
        }
        record = std::move(output_record);
        geometry = std::move(output_geometry);
        return true;
    }

    const auto fields_start = metric_start(metrics);
    const int nullable_count = nullable_field_count();
    const size_t max_bitmap_bytes =
        nullable_bitmap_bytes_for(nullable_count);
    FeatureRecord best_record;
    GeometrySlice best_geometry;
    size_t best_padding = static_cast<size_t>(blob_length) + 1U;
    bool found = false;

    for (size_t bitmap_bytes = max_bitmap_bytes;; --bitmap_bytes) {
        if (bitmap_bytes <= blob_length) {
            const int max_present_bits = std::min(
                nullable_count, static_cast<int>(bitmap_bytes * 8U));
            for (int present_bits = max_present_bits;
                 present_bits >= 0;
                 --present_bits) {
                FeatureRecord candidate;
                candidate.fid = fid;
                candidate.blob_len = blob_length;
                GeometrySlice candidate_geometry;

                try {
                    BinaryReader reader(row_data, blob_length);
                    if (bitmap_bytes > 0) {
                        candidate.nullable_flags =
                            reader.read_bytes(bitmap_bytes);
                    }
                    candidate.field_values.reserve(fields_.size());

                    int nullable_bit = 0;
                    bool valid = true;
                    for (size_t field_index = 0;
                         field_index < fields_.size();
                         ++field_index) {
                        const FieldDescriptor& field = fields_[field_index];
                        bool is_null = false;
                        if ((field.flag & 1U) != 0) {
                            is_null = nullable_bit >= present_bits ||
                                nullable_bit_is_set(
                                    candidate.nullable_flags, nullable_bit);
                            ++nullable_bit;
                        }
                        if (is_null) {
                            if (field.type == FieldType::Geometry) {
                                candidate_geometry.present = true;
                                candidate_geometry.is_null = true;
                                candidate_geometry.field_index = field_index;
                            }
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
                                if (length > std::numeric_limits<size_t>::max() ||
                                    !reader.can_read(static_cast<size_t>(length))) {
                                    valid = false;
                                    break;
                                }
                                std::string text(static_cast<size_t>(length), '\0');
                                for (char& byte : text)
                                    byte = static_cast<char>(reader.read_u8());
                                candidate.field_values.push_back(std::move(text));
                                break;
                            }
                            case FieldType::Binary: {
                                const uint64_t length = reader.read_varuint();
                                if (length > std::numeric_limits<size_t>::max() ||
                                    !reader.can_read(static_cast<size_t>(length))) {
                                    valid = false;
                                    break;
                                }
                                candidate.field_values.push_back(
                                    reader.read_bytes(static_cast<size_t>(length)));
                                break;
                            }
                            case FieldType::Raster: {
                                const uint64_t length = reader.read_varuint();
                                if (length > std::numeric_limits<size_t>::max() ||
                                    !reader.can_read(static_cast<size_t>(length))) {
                                    valid = false;
                                    break;
                                }
                                reader.skip(static_cast<size_t>(length));
                                candidate.field_values.push_back(nullptr);
                                break;
                            }
                            case FieldType::Geometry: {
                                const uint64_t length = reader.read_varuint();
                                if (length > std::numeric_limits<size_t>::max() ||
                                    !reader.can_read(static_cast<size_t>(length))) {
                                    valid = false;
                                    break;
                                }
                                candidate_geometry.present = true;
                                candidate_geometry.field_index = field_index;
                                candidate_geometry.offset = reader.tell();
                                candidate_geometry.size = static_cast<size_t>(length);
                                candidate.field_values.push_back(std::string{});
                                reader.skip(candidate_geometry.size);
                                break;
                            }
                            default:
                                valid = false;
                                break;
                        }
                        if (!valid) break;
                    }

                    if (valid && reader.tell() <= blob_length) {
                        const size_t padding = blob_length - reader.tell();
                        if (is_zero_padding(row_data + reader.tell(),
                                            row_data + blob_length) &&
                            padding < best_padding) {
                            best_padding = padding;
                            best_record = std::move(candidate);
                            best_geometry = candidate_geometry;
                            found = true;
                            if (padding == 0) break;
                        }
                    }
                } catch (const std::exception&) {
                }
            }
        }
        if (found && best_padding == 0) break;
        if (bitmap_bytes == 0) break;
    }
    metric_add(metrics, &FeatureReadMetrics::field_materialization_ms,
               fields_start);

    if (!found) return false;

    GeometryValue output_geometry;
    if (geometry_field_index_ < 0) {
        output_geometry.status = GeometryStatus::UnsupportedType;
        output_geometry.diagnostic = "table has no geometry field";
        record = std::move(best_record);
        geometry = std::move(output_geometry);
        return true;
    }
    if (!best_geometry.present || best_geometry.is_null) {
        output_geometry.status = GeometryStatus::Empty;
        output_geometry.diagnostic = "geometry is null";
        record = std::move(best_record);
        geometry = std::move(output_geometry);
        return true;
    }
    if (best_geometry.field_index >= fields_.size() ||
        best_geometry.offset > blob_length ||
        best_geometry.size > blob_length - best_geometry.offset) {
        return false;
    }

    const FieldDescriptor& geometry_field =
        fields_[best_geometry.field_index];
    const auto decode_start = metric_start(metrics);
    GeometryModel model = make_geom_decoder(geometry_field).decode_model(
        row_data + best_geometry.offset, best_geometry.size);
    metric_add(metrics, &FeatureReadMetrics::geometry_decode_ms,
               decode_start);

    if (!model.valid()) {
        output_geometry = WkbWriter::write(model);
        geometry = std::move(output_geometry);
        return false;
    }

    const auto wkt_start = metric_start(metrics);
    std::string wkt = WktWriter::write(model);
    metric_add(metrics, &FeatureReadMetrics::wkt_write_ms, wkt_start);

    const auto wkb_start = metric_start(metrics);
    output_geometry = WkbWriter::write(model);
    metric_add(metrics, &FeatureReadMetrics::wkb_write_ms, wkb_start);
    if (!output_geometry.valid()) {
        geometry = std::move(output_geometry);
        return false;
    }

    best_record.field_values[best_geometry.field_index] = std::move(wkt);
    record = std::move(best_record);
    geometry = std::move(output_geometry);
    return true;
}

} // namespace explorgdb
