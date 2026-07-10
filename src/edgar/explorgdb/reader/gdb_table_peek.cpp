#include "gdb_table.h"
#include "field_layout.h"

namespace explorgdb {

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
        BinaryReader reader(row_data, row_size);
        const int nullable_count = nullable_field_count();
        const uint8_t* nullable_bitmap = nullptr;
        if (nullable_count > 0) {
            const size_t bitmap_size = static_cast<size_t>((nullable_count + 7) / 8);
            nullable_bitmap = reader.data() + reader.tell();
            reader.skip(bitmap_size);
        }

        int nullable_bit = 0;
        for (const auto& field : fields_) {
            bool is_null = false;
            if ((field.flag & 1) != 0) {
                const int byte_index = nullable_bit / 8;
                const int bit_index = nullable_bit % 8;
                is_null = nullable_bitmap != nullptr &&
                          ((nullable_bitmap[byte_index] >> bit_index) & 1U) != 0;
                ++nullable_bit;
            }
            if (is_null) continue;

            if (field.type == FieldType::Geometry) {
                const uint64_t geometry_size = reader.read_varuint();
                if (geometry_size > row_size - reader.tell()) return false;
                blob_data = reader.data() + reader.tell();
                blob_size = static_cast<size_t>(geometry_size);
                return true;
            }

            if (!skip_field_value(reader, field.type)) return false;
        }
    } catch (const std::exception&) {
        blob_data = nullptr;
        blob_size = 0;
        return false;
    }

    return false;
}

} // namespace explorgdb
