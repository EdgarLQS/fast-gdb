#include "gdb_table.h"
#include "field_layout.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace explorgdb {
namespace {

bool read_varuint_checked(const uint8_t*& cursor,
                          const uint8_t* end,
                          uint64_t& value) {
    value = 0;
    unsigned shift = 0;
    while (cursor < end && shift <= 63U) {
        const uint8_t byte = *cursor++;
        value |= static_cast<uint64_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) return true;
        shift += 7U;
    }
    return false;
}

bool skip_value(const FieldDescriptor& field,
                const uint8_t*& cursor,
                const uint8_t* end,
                const uint8_t** value_data,
                size_t* value_size) {
    if (value_data) *value_data = nullptr;
    if (value_size) *value_size = 0;

    if (field.type == FieldType::ObjectId)
        return true;

    const size_t fixed_width = fixed_physical_width(field.type);
    if (fixed_width != 0) {
        if (fixed_width > static_cast<size_t>(end - cursor)) return false;
        if (value_data) *value_data = cursor;
        if (value_size) *value_size = fixed_width;
        cursor += fixed_width;
        return true;
    }

    switch (field.type) {
        case FieldType::String:
        case FieldType::XML:
        case FieldType::Binary:
        case FieldType::Raster:
        case FieldType::Geometry: {
            uint64_t encoded_size = 0;
            if (!read_varuint_checked(cursor, end, encoded_size)) return false;
            if (encoded_size > static_cast<uint64_t>(end - cursor) ||
                encoded_size > static_cast<uint64_t>(
                    std::numeric_limits<size_t>::max())) {
                return false;
            }
            const size_t size = static_cast<size_t>(encoded_size);
            if (value_data) *value_data = cursor;
            if (value_size) *value_size = size;
            cursor += size;
            return true;
        }
        default:
            return false;
    }
}

bool all_zero(const uint8_t* cursor, const uint8_t* end) {
    while (cursor < end) {
        if (*cursor++ != 0) return false;
    }
    return true;
}

size_t bitmap_bytes_for(int nullable_count) {
    return static_cast<size_t>((nullable_count + 7) / 8);
}

bool null_bit(const uint8_t* bitmap,
              size_t bitmap_bytes,
              int nullable_bit,
              int present_bits) {
    if (nullable_bit >= present_bits) return true;
    const size_t byte_index = static_cast<size_t>(nullable_bit / 8);
    if (bitmap == nullptr || byte_index >= bitmap_bytes) return true;
    const unsigned bit_index = static_cast<unsigned>(nullable_bit % 8);
    return ((bitmap[byte_index] >> bit_index) & 1U) != 0;
}

} // namespace

uint64_t GdbTableParser::scan_geometry_blobs(GeometryScanCallback callback) {
    if (!callback || fields_.empty() || feature_offsets_.empty() ||
        geometry_field_index_ < 0 ||
        geometry_field_index_ >= static_cast<int>(fields_.size())) {
        return 0;
    }

    const int nullable_count = nullable_field_count();
    const size_t max_bitmap_bytes = bitmap_bytes_for(nullable_count);
    uint64_t scanned = 0;

    for (uint32_t fid = 0; fid < feature_offsets_.size(); ++fid) {
        const uint64_t offset = feature_offsets_[fid];
        if (offset == 0 || offset >= file_size_) continue;

        uint32_t row_size = 0;
        const uint8_t* row_begin = nullptr;
        const uint8_t* row_end = nullptr;

        if (mapped_data_ != nullptr) {
            if (offset > file_size_ - std::min<size_t>(file_size_, 4U)) break;
            const uint8_t* length_ptr = mapped_data_ + offset;
            if (length_ptr + 4 > mapped_data_ + file_size_) break;
            std::memcpy(&row_size, length_ptr, sizeof(row_size));
            row_begin = length_ptr + 4;
            if (row_size > static_cast<size_t>(mapped_data_ + file_size_ - row_begin))
                break;
            row_end = row_begin + row_size;
        } else if (fd_ >= 0) {
            uint8_t length_buffer[4];
            if (!read_at(offset, length_buffer, sizeof(length_buffer))) break;
            std::memcpy(&row_size, length_buffer, sizeof(row_size));
            if (offset + 4 > file_size_ ||
                row_size > file_size_ - static_cast<size_t>(offset + 4)) {
                break;
            }
            if (row_buffer_.size() < row_size) row_buffer_.resize(row_size);
            if (row_size != 0 &&
                !read_at(offset + 4, row_buffer_.data(), row_size)) {
                break;
            }
            row_begin = row_buffer_.data();
            row_end = row_begin + row_size;
        } else {
            break;
        }

        if (row_size == 0) {
            if (!callback(fid, nullptr, 0, true)) break;
            ++scanned;
            continue;
        }

        bool accepted = false;
        size_t best_padding = static_cast<size_t>(row_end - row_begin) + 1;
        const uint8_t* best_geometry = nullptr;
        size_t best_geometry_size = 0;
        bool best_geometry_null = true;

        for (size_t bitmap_bytes = max_bitmap_bytes;; --bitmap_bytes) {
            if (bitmap_bytes <= static_cast<size_t>(row_end - row_begin)) {
                const int max_present_bits = std::min(
                    nullable_count, static_cast<int>(bitmap_bytes * 8));
                for (int present_bits = max_present_bits;
                     present_bits >= 0;
                     --present_bits) {
                    const uint8_t* cursor = row_begin + bitmap_bytes;
                    const uint8_t* bitmap = bitmap_bytes ? row_begin : nullptr;
                    const uint8_t* geometry = nullptr;
                    size_t geometry_size = 0;
                    bool geometry_null = true;
                    int nullable_bit = 0;
                    bool valid = true;

                    for (size_t field_index = 0;
                         field_index < fields_.size();
                         ++field_index) {
                        const FieldDescriptor& field = fields_[field_index];
                        bool is_null = false;
                        if ((field.flag & 1U) != 0) {
                            is_null = null_bit(bitmap, bitmap_bytes,
                                               nullable_bit, present_bits);
                            ++nullable_bit;
                        }
                        if (is_null) {
                            if (static_cast<int>(field_index) ==
                                geometry_field_index_) {
                                geometry_null = true;
                            }
                            continue;
                        }

                        const uint8_t* value_data = nullptr;
                        size_t value_size = 0;
                        if (!skip_value(field, cursor, row_end,
                                        &value_data, &value_size)) {
                            valid = false;
                            break;
                        }
                        if (static_cast<int>(field_index) ==
                            geometry_field_index_) {
                            geometry = value_data;
                            geometry_size = value_size;
                            geometry_null = false;
                        }
                    }

                    if (!valid) continue;
                    const size_t padding = static_cast<size_t>(row_end - cursor);
                    if (!all_zero(cursor, row_end) || padding >= best_padding)
                        continue;

                    accepted = true;
                    best_padding = padding;
                    best_geometry = geometry;
                    best_geometry_size = geometry_size;
                    best_geometry_null = geometry_null;
                    if (padding == 0) break;
                }
            }
            if (accepted && best_padding == 0) break;
            if (bitmap_bytes == 0) break;
        }

        if (!accepted) {
            if (!callback(fid, nullptr, 0, true)) break;
        } else if (!callback(fid, best_geometry, best_geometry_size,
                             best_geometry_null)) {
            break;
        }
        ++scanned;
    }

    return scanned;
}

} // namespace explorgdb
