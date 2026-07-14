#include "gdb_table.h"
#include "field_layout.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace explorgdb {
namespace {

bool is_zero_padding(const uint8_t* cursor, const uint8_t* end) {
    while (cursor < end) {
        if (*cursor++ != 0) return false;
    }
    return true;
}

size_t sparse_window_bytes() {
    constexpr size_t kMiB = 1024U * 1024U;
    constexpr size_t kDefaultMiB = 1;
    constexpr size_t kMaximumMiB = 8;
    const char* value = std::getenv("FAST_GDB_WINDOWS_SPARSE_WINDOW_MB");
    if (value == nullptr || *value == '\0') return kDefaultMiB * kMiB;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0) return kDefaultMiB * kMiB;
    return std::min<size_t>(static_cast<size_t>(parsed), kMaximumMiB) * kMiB;
}

bool io_trace_enabled() {
    const char* value = std::getenv("FAST_GDB_WINDOWS_IO_TRACE");
    return value != nullptr && value[0] == '1';
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

    // The table is normally mmap-backed. Return a stable view into that mapping
    // instead of copying every candidate row into row_buffer_. Large spatial
    // queries can otherwise perform millions of avoidable memcpy operations.
    if (mapped_data_ != nullptr) {
        if (offset + 4 > file_size_) return false;
        uint32_t blob_len = 0;
        std::memcpy(&blob_len, mapped_data_ + offset, sizeof(blob_len));
        if (blob_len == 0 || offset + 4 + blob_len > file_size_) return false;
        row_data = mapped_data_ + offset + 4;
        row_size = blob_len;
    } else if (fd_ >= 0) {
        if (offset + 4 > file_size_) return false;

        // P2: merge physically adjacent sparse candidates into one bounded read
        // window. .spx candidates are normally close to FID/physical order, so
        // subsequent lookups reuse this buffer; non-local candidates simply
        // replace it without changing spatial semantics or FID identity.
        constexpr uint64_t kAlignment = 64U * 1024U;
        const uint64_t window_end = sparse_window_offset_ + sparse_window_size_;
        bool in_window = !sparse_window_buffer_.empty() &&
                         offset >= sparse_window_offset_ &&
                         offset + 4 <= window_end;
        if (!in_window) {
            sparse_window_offset_ = offset - (offset % kAlignment);
            sparse_window_size_ = static_cast<size_t>(std::min<uint64_t>(
                sparse_window_bytes(), file_size_ - sparse_window_offset_));
            sparse_window_buffer_.resize(sparse_window_size_);
            if (sparse_window_size_ == 0 ||
                !read_at(sparse_window_offset_, sparse_window_buffer_.data(),
                         sparse_window_size_)) {
                sparse_window_buffer_.clear();
                sparse_window_size_ = 0;
                return false;
            }
            if (io_trace_enabled()) {
                std::fprintf(stderr,
                             "fast-gdb windows sparse-read: offset=%llu bytes=%zu\n",
                             static_cast<unsigned long long>(sparse_window_offset_),
                             sparse_window_size_);
            }
        }

        const size_t local_offset = static_cast<size_t>(
            offset - sparse_window_offset_);
        if (local_offset + 4 > sparse_window_buffer_.size()) return false;
        uint32_t blob_len = 0;
        std::memcpy(&blob_len, sparse_window_buffer_.data() + local_offset,
                    sizeof(blob_len));
        if (blob_len == 0 || offset + 4 + blob_len > file_size_) return false;

        if (local_offset + 4 + blob_len <= sparse_window_buffer_.size()) {
            row_data = sparse_window_buffer_.data() + local_offset + 4;
            row_size = blob_len;
        } else {
            // A wide record straddles the bounded window. Preserve correctness
            // with one exact positional read rather than growing the cache.
            if (row_buffer_.size() < blob_len) row_buffer_.resize(blob_len);
            if (!read_at(offset + 4, row_buffer_.data(), blob_len)) return false;
            row_data = row_buffer_.data();
            row_size = blob_len;
        }
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
