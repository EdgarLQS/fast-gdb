#include "gdb_table.h"
#include "field_layout.h"
#include <algorithm>

namespace explorgdb {
namespace {

bool is_zero_padding(const uint8_t* cursor, const uint8_t* end) {
    while (cursor < end) {
        if (*cursor++ != 0) return false;
    }
    return true;
}

} // namespace

bool GdbTableParser::peek_geometry_blob(uint32_t fid,
                                        const uint8_t*& blob_data,
                                        size_t& blob_size) {
    blob_data = nullptr;
    blob_size = 0;

    if (fd_ < 0 && file_data_.empty()) {
        if (!open()) return false;
    }
    if (!ensure_fields_loaded()) return false;
    if (fid >= feature_offsets_.size()) return false;

    const uint64_t offset = feature_offsets_[fid];
    if (offset == 0) return false;

    const uint8_t* row_data = nullptr;
    size_t row_size = 0;

    if (fd_ >= 0) {
        if (offset + 4 > file_size_) return false;

        uint8_t len_buffer[4];
        if (!read_at(offset, len_buffer, sizeof(len_buffer))) return false;
        BinaryReader len_reader(len_buffer, sizeof(len_buffer));
        const uint32_t blob_len = len_reader.read_u32();
        if (blob_len == 0 || offset + 4 + blob_len > file_size_) return false;

        if (row_buffer_.size() < blob_len) row_buffer_.resize(blob_len);
        if (!read_at(offset + 4, row_buffer_.data(), blob_len)) return false;
        row_data = row_buffer_.data();
        row_size = blob_len;
    } else {
        if (offset + 4 > file_data_.size()) return false;
        BinaryReader len_reader(file_data_.data() + offset, file_data_.size() - offset);
        const uint32_t blob_len = len_reader.read_u32();
        if (blob_len == 0 || offset + 4 + blob_len > file_data_.size()) return false;
        row_data = file_data_.data() + offset + 4;
        row_size = blob_len;
    }

    try {
        const int nullable_count = nullable_field_count();
        const size_t max_bitmap_size = static_cast<size_t>((nullable_count + 7) / 8);
        const uint8_t* best_blob = nullptr;
        size_t best_size = 0;
        size_t best_padding = row_size + 1;

        for (size_t bitmap_size = max_bitmap_size;; --bitmap_size) {
            if (bitmap_size > row_size) {
                if (bitmap_size == 0) break;
                continue;
            }

            const int max_present_bits =
                std::min(nullable_count, static_cast<int>(bitmap_size * 8));
            for (int present_bits = max_present_bits; present_bits >= 0; --present_bits) {
                BinaryReader reader(row_data, row_size);
                const uint8_t* nullable_bitmap = nullptr;
                if (bitmap_size > 0) {
                    nullable_bitmap = reader.data() + reader.tell();
                    reader.skip(bitmap_size);
                }

                const uint8_t* candidate_blob = nullptr;
                size_t candidate_size = 0;
                int nullable_bit = 0;
                bool valid = true;
                for (const auto& field : fields_) {
                    bool is_null = false;
                    if ((field.flag & 1) != 0) {
                        const int byte_index = nullable_bit / 8;
                        const int bit_index = nullable_bit % 8;
                        is_null =
                            nullable_bit >= present_bits ||
                            (nullable_bitmap != nullptr &&
                             ((nullable_bitmap[byte_index] >> bit_index) & 1U) != 0);
                        ++nullable_bit;
                    }
                    if (is_null) continue;

                    if (field.type == FieldType::Geometry) {
                        const uint64_t geometry_size = reader.read_varuint();
                        if (geometry_size > row_size - reader.tell()) {
                            valid = false;
                            break;
                        }
                        candidate_blob = reader.data() + reader.tell();
                        candidate_size = static_cast<size_t>(geometry_size);
                        reader.skip(candidate_size);
                        continue;
                    }

                    if (!skip_field_value(reader, field.type)) {
                        valid = false;
                        break;
                    }
                }

                if (valid &&
                    reader.tell() <= row_size &&
                    is_zero_padding(row_data + reader.tell(), row_data + row_size) &&
                    candidate_blob != nullptr) {
                    const size_t padding = row_size - reader.tell();
                    if (padding < best_padding ||
                        (padding == best_padding && candidate_size > best_size)) {
                        best_blob = candidate_blob;
                        best_size = candidate_size;
                        best_padding = padding;
                    }
                }
            }

            if (bitmap_size == 0) break;
        }

        if (best_blob != nullptr) {
            blob_data = best_blob;
            blob_size = best_size;
            return true;
        }
    } catch (const std::exception&) {
        blob_data = nullptr;
        blob_size = 0;
        return false;
    }

    return false;
}

} // namespace explorgdb
