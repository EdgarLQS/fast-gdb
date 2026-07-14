#include "gdb_table.h"
#include "field_layout.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#ifdef _WIN32
#include <deque>
#include <future>
#endif

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

struct GeometryLocation {
    bool accepted = false;
    const uint8_t* geometry = nullptr;
    size_t geometry_size = 0;
    bool geometry_null = true;
};

GeometryLocation locate_geometry(const std::vector<FieldDescriptor>& fields,
                                 int geometry_field_index,
                                 int nullable_count,
                                 const uint8_t* row_begin,
                                 const uint8_t* row_end) {
    GeometryLocation result;
    const size_t max_bitmap_bytes = bitmap_bytes_for(nullable_count);
    size_t best_padding = static_cast<size_t>(row_end - row_begin) + 1;

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
                     field_index < fields.size();
                     ++field_index) {
                    const FieldDescriptor& field = fields[field_index];
                    bool is_null = false;
                    if ((field.flag & 1U) != 0) {
                        is_null = null_bit(bitmap, bitmap_bytes,
                                           nullable_bit, present_bits);
                        ++nullable_bit;
                    }
                    if (is_null) {
                        if (static_cast<int>(field_index) ==
                            geometry_field_index) {
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
                        geometry_field_index) {
                        geometry = value_data;
                        geometry_size = value_size;
                        geometry_null = false;
                    }
                }

                if (!valid) continue;
                const size_t padding = static_cast<size_t>(row_end - cursor);
                if (!all_zero(cursor, row_end) || padding >= best_padding)
                    continue;

                result.accepted = true;
                result.geometry = geometry;
                result.geometry_size = geometry_size;
                result.geometry_null = geometry_null;
                best_padding = padding;
                if (padding == 0) break;
            }
        }
        if (result.accepted && best_padding == 0) break;
        if (bitmap_bytes == 0) break;
    }
    return result;
}

size_t bounded_window_bytes(const char* env_name,
                            size_t default_mebibytes) {
    constexpr size_t kMiB = 1024U * 1024U;
    constexpr size_t kMaximumMiB = 8;
    const char* value = std::getenv(env_name);
    if (value == nullptr || *value == '\0') return default_mebibytes * kMiB;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0)
        return default_mebibytes * kMiB;
    return std::min<size_t>(static_cast<size_t>(parsed), kMaximumMiB) * kMiB;
}

size_t dense_batch_bytes() {
    return bounded_window_bytes("FAST_GDB_WINDOWS_BATCH_MB", 4);
}

size_t sparse_batch_bytes() {
    return bounded_window_bytes("FAST_GDB_WINDOWS_SPARSE_WINDOW_MB", 1);
}

bool io_trace_enabled() {
    const char* value = std::getenv("FAST_GDB_WINDOWS_IO_TRACE");
    return value != nullptr && value[0] == '1';
}

#ifdef _WIN32
size_t async_depth() {
    constexpr size_t kDefaultDepth = 4;
    constexpr size_t kMaximumDepth = 8;
    const char* enabled = std::getenv("FAST_GDB_WINDOWS_ASYNC_IO");
    // P3 is implemented but deliberately opt-in. Phase H requires P0-P2 to be
    // measured first; the acceptance matrix explicitly enables this mode.
    if (enabled == nullptr || enabled[0] != '1') return 1;
    const char* value = std::getenv("FAST_GDB_WINDOWS_ASYNC_DEPTH");
    if (value == nullptr || *value == '\0') return kDefaultDepth;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0) return kDefaultDepth;
    return std::min<size_t>(static_cast<size_t>(parsed), kMaximumDepth);
}
#endif

struct RowRef {
    uint32_t fid = 0;
    uint64_t offset = 0;
};

struct ReadBatch {
    uint64_t offset = 0;
    size_t size = 0;
    std::vector<RowRef> rows;
};

struct BatchResult {
    ReadBatch batch;
    std::vector<uint8_t> bytes;
    bool ok = false;
};

} // namespace

uint64_t GdbTableParser::scan_geometry_blobs(GeometryScanCallback callback) {
    if (!callback || fields_.empty() || feature_offsets_.empty() ||
        geometry_field_index_ < 0 ||
        geometry_field_index_ >= static_cast<int>(fields_.size())) {
        return 0;
    }

    const int nullable_count = nullable_field_count();
    uint64_t scanned = 0;

    auto emit_row = [&](uint32_t fid,
                        const uint8_t* row_begin,
                        size_t row_size) -> bool {
        if (row_size == 0) {
            if (!callback(fid, nullptr, 0, true)) return false;
            ++scanned;
            return true;
        }
        const uint8_t* row_end = row_begin + row_size;
        const GeometryLocation located = locate_geometry(
            fields_, geometry_field_index_, nullable_count,
            row_begin, row_end);
        if (!located.accepted) {
            if (!callback(fid, nullptr, 0, true)) return false;
        } else if (!callback(fid, located.geometry, located.geometry_size,
                             located.geometry_null)) {
            return false;
        }
        ++scanned;
        return true;
    };

    // P0: MapViewOfFile follows the existing zero-copy mapped_data_ path.
    if (mapped_data_ != nullptr) {
        for (uint32_t fid = 0; fid < feature_offsets_.size(); ++fid) {
            const uint64_t offset = feature_offsets_[fid];
            if (offset == 0 || offset >= file_size_) continue;
            if (offset + 4 > file_size_) return 0;

            uint32_t row_size = 0;
            const uint8_t* length_ptr = mapped_data_ + offset;
            std::memcpy(&row_size, length_ptr, sizeof(row_size));
            const uint8_t* row_begin = length_ptr + 4;
            if (row_size > static_cast<size_t>(
                    mapped_data_ + file_size_ - row_begin)) {
                return 0;
            }
            if (!emit_row(fid, row_begin, row_size)) break;
        }
        if (io_trace_enabled()) {
            std::fprintf(stderr,
                         "fast-gdb windows dense-scan: mode=mmap records=%llu "
                         "batch_reads=0 bytes=0 async_depth=0\n",
                         static_cast<unsigned long long>(scanned));
        }
        return scanned;
    }

    if (fd_ < 0) return 0;

    // P1: build bounded physical windows instead of issuing two reads per FID.
    // The .gdbtablx order is normally physical/FID order; an out-of-order offset
    // closes the current window, retaining correctness without a 10M-entry sort.
    const size_t batch_limit = dense_batch_bytes();
    const uint64_t max_record_bytes =
        static_cast<uint64_t>(std::max<uint32_t>(
            header_.largest_size_record, 1U)) + 4U;
    ReadBatch current;
    uint64_t batch_reads = 0;
    uint64_t bytes_read = 0;
    bool io_failed = false;
    bool callback_stopped = false;

    auto read_batch = [&](ReadBatch batch) -> BatchResult {
        BatchResult result;
        result.batch = std::move(batch);
        result.bytes.resize(result.batch.size);
        result.ok = result.batch.size != 0 &&
                    read_at(result.batch.offset, result.bytes.data(),
                            result.batch.size);
        return result;
    };

    auto process_batch = [&](BatchResult& result) -> bool {
        if (!result.ok) {
            // P3 failure contract: one synchronous retry before abandoning the
            // dense path and allowing QueryEngine to use candidate fallback.
            result.bytes.resize(result.batch.size);
            result.ok = result.batch.size != 0 &&
                        read_at(result.batch.offset, result.bytes.data(),
                                result.batch.size);
        }
        if (!result.ok) {
            io_failed = true;
            return false;
        }

        ++batch_reads;
        bytes_read += result.bytes.size();
        for (const RowRef& row : result.batch.rows) {
            if (row.offset < result.batch.offset) {
                io_failed = true;
                return false;
            }
            const uint64_t local64 = row.offset - result.batch.offset;
            if (local64 > result.bytes.size()) {
                io_failed = true;
                return false;
            }
            const size_t local = static_cast<size_t>(local64);
            if (local + 4 > result.bytes.size()) {
                io_failed = true;
                return false;
            }

            uint32_t row_size = 0;
            std::memcpy(&row_size, result.bytes.data() + local,
                        sizeof(row_size));
            if (row_size > result.bytes.size() - local - 4) {
                io_failed = true;
                return false;
            }
            if (!emit_row(row.fid, result.bytes.data() + local + 4,
                          row_size)) {
                callback_stopped = true;
                return false;
            }
        }
        return true;
    };

#ifdef _WIN32
    const size_t prefetch_depth = async_depth();
    std::deque<std::future<BatchResult>> pending;

    auto consume_front = [&]() -> bool {
        if (pending.empty()) return true;
        BatchResult result = pending.front().get();
        pending.pop_front();
        return process_batch(result);
    };

    auto submit_batch = [&](ReadBatch batch) -> bool {
        if (batch.rows.empty()) return true;
        if (prefetch_depth <= 1) {
            BatchResult result = read_batch(std::move(batch));
            return process_batch(result);
        }
        pending.emplace_back(std::async(
            std::launch::async,
            [&, batch = std::move(batch)]() mutable {
                return read_batch(std::move(batch));
            }));
        if (pending.size() >= prefetch_depth)
            return consume_front();
        return true;
    };
#else
    const size_t prefetch_depth = 1;
    auto submit_batch = [&](ReadBatch batch) -> bool {
        if (batch.rows.empty()) return true;
        BatchResult result = read_batch(std::move(batch));
        return process_batch(result);
    };
#endif

    auto finish_current = [&]() -> bool {
        if (current.rows.empty()) return true;
        ReadBatch batch = std::move(current);
        current = ReadBatch{};
        return submit_batch(std::move(batch));
    };

    for (uint32_t fid = 0; fid < feature_offsets_.size(); ++fid) {
        const uint64_t offset = feature_offsets_[fid];
        if (offset == 0 || offset >= file_size_) continue;
        const uint64_t available = file_size_ - offset;
        const uint64_t bounded_record = std::min(max_record_bytes, available);
        const uint64_t row_end = offset + bounded_record;

        bool fits = false;
        if (!current.rows.empty() && offset >= current.offset) {
            const uint64_t span = row_end - current.offset;
            fits = span <= batch_limit;
        }
        if (!fits) {
            if (!finish_current()) break;
            current.offset = offset;
            current.size = static_cast<size_t>(bounded_record);
        } else {
            current.size = std::max(
                current.size, static_cast<size_t>(row_end - current.offset));
        }
        current.rows.push_back(RowRef{fid, offset});
    }

    if (!callback_stopped && !io_failed)
        (void)finish_current();

#ifdef _WIN32
    while (!pending.empty()) {
        if (!callback_stopped && !io_failed) {
            (void)consume_front();
        } else {
            pending.front().wait();
            pending.pop_front();
        }
    }
#endif

    if (io_trace_enabled()) {
        std::fprintf(stderr,
                     "fast-gdb windows dense-scan: mode=fallback records=%llu "
                     "batch_reads=%llu bytes=%llu async_depth=%zu failed=%s\n",
                     static_cast<unsigned long long>(scanned),
                     static_cast<unsigned long long>(batch_reads),
                     static_cast<unsigned long long>(bytes_read),
                     prefetch_depth, io_failed ? "true" : "false");
    }
    return io_failed ? 0 : scanned;
}

uint64_t GdbTableParser::scan_geometry_candidates(
    const std::vector<uint32_t>& candidates,
    GeometryScanCallback callback) {
    // mmap already gives the canonical locator a zero-copy path. P2 is only for
    // fd fallback, and a zero return instructs QueryEngine to keep that path.
    if (!callback || candidates.empty() || mapped_data_ != nullptr || fd_ < 0 ||
        fields_.empty() || geometry_field_index_ < 0 ||
        geometry_field_index_ >= static_cast<int>(fields_.size())) {
        return 0;
    }

    std::vector<RowRef> physical;
    physical.reserve(candidates.size());
    for (uint32_t fid : candidates) {
        if (fid >= feature_offsets_.size()) return 0;
        const uint64_t offset = feature_offsets_[fid];
        if (offset == 0 || offset >= file_size_) return 0;
        physical.push_back(RowRef{fid, offset});
    }
    std::sort(physical.begin(), physical.end(),
              [](const RowRef& left, const RowRef& right) {
                  if (left.offset != right.offset)
                      return left.offset < right.offset;
                  return left.fid < right.fid;
              });

    const int nullable_count = nullable_field_count();
    const size_t batch_limit = sparse_batch_bytes();
    const uint64_t max_record_bytes =
        static_cast<uint64_t>(std::max<uint32_t>(
            header_.largest_size_record, 1U)) + 4U;
    uint64_t scanned = 0;
    uint64_t batch_reads = 0;
    uint64_t bytes_read = 0;
    std::vector<uint8_t> buffer;

    size_t begin = 0;
    while (begin < physical.size()) {
        const uint64_t batch_offset = physical[begin].offset;
        const uint64_t first_available = file_size_ - batch_offset;
        uint64_t batch_end = batch_offset +
            std::min(max_record_bytes, first_available);
        size_t end = begin + 1;

        while (end < physical.size()) {
            const uint64_t offset = physical[end].offset;
            const uint64_t available = file_size_ - offset;
            const uint64_t candidate_end = offset +
                std::min(max_record_bytes, available);
            if (candidate_end - batch_offset > batch_limit) break;
            batch_end = std::max(batch_end, candidate_end);
            ++end;
        }

        const size_t batch_size = static_cast<size_t>(batch_end - batch_offset);
        buffer.resize(batch_size);
        if (batch_size == 0 ||
            !read_at(batch_offset, buffer.data(), batch_size)) {
            return 0;
        }
        ++batch_reads;
        bytes_read += batch_size;

        for (size_t index = begin; index < end; ++index) {
            const RowRef& row = physical[index];
            const uint64_t local64 = row.offset - batch_offset;
            if (local64 > buffer.size()) return 0;
            const size_t local = static_cast<size_t>(local64);
            if (local + 4 > buffer.size()) return 0;

            uint32_t row_size = 0;
            std::memcpy(&row_size, buffer.data() + local, sizeof(row_size));
            if (row_size > buffer.size() - local - 4) return 0;

            if (row_size == 0) {
                if (!callback(row.fid, nullptr, 0, true)) return scanned;
                ++scanned;
                continue;
            }

            const uint8_t* row_begin = buffer.data() + local + 4;
            const GeometryLocation located = locate_geometry(
                fields_, geometry_field_index_, nullable_count,
                row_begin, row_begin + row_size);
            if (!located.accepted) {
                if (!callback(row.fid, nullptr, 0, true)) return scanned;
            } else if (!callback(row.fid, located.geometry,
                                 located.geometry_size,
                                 located.geometry_null)) {
                return scanned;
            }
            ++scanned;
        }
        begin = end;
    }

    if (io_trace_enabled()) {
        std::fprintf(stderr,
                     "fast-gdb windows sparse-batch: candidates=%zu "
                     "batch_reads=%llu bytes=%llu failed=false\n",
                     candidates.size(),
                     static_cast<unsigned long long>(batch_reads),
                     static_cast<unsigned long long>(bytes_read));
    }
    return scanned;
}

} // namespace explorgdb
