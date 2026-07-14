#include "gdb_table.h"
#include "field_layout.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include "windows_posix_compat.h"
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
    if (field.type == FieldType::ObjectId) return true;

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
    if (enabled == nullptr || enabled[0] != '1') return 1;
    const char* value = std::getenv("FAST_GDB_WINDOWS_ASYNC_DEPTH");
    if (value == nullptr || *value == '\0') return kDefaultDepth;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0) return kDefaultDepth;
    return std::min<size_t>(static_cast<size_t>(parsed), kMaximumDepth);
}

bool force_async_launch_failure() {
    const char* value = std::getenv("FAST_GDB_FORCE_ASYNC_LAUNCH_FAILURE");
    return value != nullptr && value[0] == '1';
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
    bool overlapped_attempted = false;
};

bool row_prefix_fits(const ReadBatch& batch, uint64_t offset) {
    if (batch.rows.empty() || offset < batch.offset) return false;
    const uint64_t local64 = offset - batch.offset;
    if (local64 > std::numeric_limits<size_t>::max()) return false;
    const size_t local = static_cast<size_t>(local64);
    return local <= batch.size && batch.size - local >= 4;
}

ReadBatch start_batch(uint64_t offset, uint64_t file_size,
                      size_t batch_limit) {
    ReadBatch batch;
    batch.offset = offset;
    batch.size = static_cast<size_t>(std::min<uint64_t>(
        batch_limit, file_size - offset));
    return batch;
}

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
        const GeometryLocation located = locate_geometry(
            fields_, geometry_field_index_, nullable_count,
            row_begin, row_begin + row_size);
        if (!located.accepted) {
            if (!callback(fid, nullptr, 0, true)) return false;
        } else if (!callback(fid, located.geometry, located.geometry_size,
                             located.geometry_null)) {
            return false;
        }
        ++scanned;
        return true;
    };

    if (mapped_data_ != nullptr) {
        for (uint32_t fid = 0; fid < feature_offsets_.size(); ++fid) {
            const uint64_t offset = feature_offsets_[fid];
            if (offset == 0 || offset >= file_size_) continue;
            if (offset > file_size_ - std::min<size_t>(file_size_, 4U))
                return 0;

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
                         "batch_reads=0 bytes=0 exact_reads=0 exact_bytes=0 "
                         "overlapped_batches=0 async_depth=0 failed=false\n",
                         static_cast<unsigned long long>(scanned));
        }
        return scanned;
    }

#ifdef _WIN32
    // A full view is deliberately avoided for large/address-space-constrained
    // tables. This parser-owned scan object keeps one mapping handle and one
    // allocation-granularity-aligned view, remapping as physical offsets move.
    if (fd_ >= 0) {
        FastGdbSlidingMap sliding;
        if (sliding.open(fd_)) {
            const size_t preferred = dense_batch_bytes();
            for (uint32_t fid = 0; fid < feature_offsets_.size(); ++fid) {
                const uint64_t offset = feature_offsets_[fid];
                if (offset == 0 || offset >= file_size_) continue;
                if (offset > file_size_ - std::min<size_t>(file_size_, 4U))
                    return 0;

                const uint8_t* prefix = sliding.map(offset, 4, preferred);
                if (prefix == nullptr) return 0;
                uint32_t row_size = 0;
                std::memcpy(&row_size, prefix, sizeof(row_size));
                if (row_size > file_size_ - static_cast<size_t>(offset + 4))
                    return 0;
                if (row_size == 0) {
                    if (!emit_row(fid, nullptr, 0)) break;
                    continue;
                }
                const uint8_t* row = sliding.map(
                    offset + 4, row_size,
                    std::max(preferred, static_cast<size_t>(row_size)));
                if (row == nullptr) return 0;
                if (!emit_row(fid, row, row_size)) break;
            }
            if (io_trace_enabled()) {
                std::fprintf(stderr,
                             "fast-gdb windows dense-scan: mode=mmap-windowed "
                             "records=%llu batch_reads=0 bytes=0 exact_reads=0 "
                             "exact_bytes=0 overlapped_batches=0 async_depth=0 "
                             "failed=false\n",
                             static_cast<unsigned long long>(scanned));
            }
            return scanned;
        }
    }
#endif

    if (fd_ < 0) return 0;

    const size_t batch_limit = dense_batch_bytes();
    ReadBatch current;
    uint64_t batch_reads = 0;
    uint64_t bytes_read = 0;
    uint64_t exact_reads = 0;
    uint64_t exact_bytes = 0;
    uint64_t overlapped_batches = 0;
    bool io_failed = false;
    bool callback_stopped = false;

    auto read_batch_sync = [&](ReadBatch batch) -> BatchResult {
        BatchResult result;
        result.batch = std::move(batch);
        try {
            result.bytes.resize(result.batch.size);
            result.ok = result.batch.size != 0 &&
                        read_at(result.batch.offset, result.bytes.data(),
                                result.batch.size);
        } catch (...) {
            result.ok = false;
        }
        return result;
    };

#ifdef _WIN32
    auto read_batch_overlapped = [&](ReadBatch batch) -> BatchResult {
        BatchResult result;
        result.batch = std::move(batch);
        result.overlapped_attempted = true;
        try {
            result.bytes.resize(result.batch.size);
            result.ok = result.batch.size != 0 &&
                fast_gdb_pread_overlapped(
                    fd_, result.bytes.data(), result.batch.size,
                    static_cast<__int64>(result.batch.offset)) ==
                static_cast<ssize_t>(result.batch.size);
        } catch (...) {
            result.ok = false;
        }
        return result;
    };
#endif

    auto process_batch = [&](BatchResult& result) -> bool {
        if (result.overlapped_attempted) ++overlapped_batches;
        if (!result.ok) result = read_batch_sync(std::move(result.batch));
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
            if (row.offset + 4 > file_size_ ||
                row_size > file_size_ - static_cast<size_t>(row.offset + 4)) {
                io_failed = true;
                return false;
            }

            if (row_size <= result.bytes.size() - local - 4) {
                if (!emit_row(row.fid, result.bytes.data() + local + 4,
                              row_size)) {
                    callback_stopped = true;
                    return false;
                }
                continue;
            }

            std::vector<uint8_t> exact;
            try {
                exact.resize(row_size);
            } catch (...) {
                io_failed = true;
                return false;
            }
            if (row_size != 0 &&
                !read_at(row.offset + 4, exact.data(), exact.size())) {
                io_failed = true;
                return false;
            }
            ++exact_reads;
            exact_bytes += exact.size();
            if (!emit_row(row.fid, exact.data(), exact.size())) {
                callback_stopped = true;
                return false;
            }
        }
        return true;
    };

#ifdef _WIN32
    struct PendingRead {
        ReadBatch batch;
        std::future<BatchResult> future;
    };
    const size_t requested_depth = async_depth();
    size_t actual_depth = 0;
    std::deque<PendingRead> pending;

    auto consume_front = [&]() -> bool {
        if (pending.empty()) return true;
        PendingRead item = std::move(pending.front());
        pending.pop_front();
        BatchResult result;
        try {
            result = item.future.get();
        } catch (...) {
            result.batch = std::move(item.batch);
            result.ok = false;
            result.overlapped_attempted = true;
        }
        return process_batch(result);
    };

    auto submit_batch = [&](ReadBatch batch) -> bool {
        if (batch.rows.empty()) return true;
        if (requested_depth <= 1) {
            BatchResult result = read_batch_sync(std::move(batch));
            return process_batch(result);
        }
        PendingRead item;
        item.batch = batch;
        try {
            if (force_async_launch_failure())
                throw std::runtime_error("forced async launch failure");
            item.future = std::async(
                std::launch::async,
                [&, batch = std::move(batch)]() mutable {
                    return read_batch_overlapped(std::move(batch));
                });
        } catch (...) {
            BatchResult result = read_batch_sync(std::move(item.batch));
            return process_batch(result);
        }
        pending.emplace_back(std::move(item));
        actual_depth = std::max(actual_depth, pending.size());
        if (pending.size() >= requested_depth) return consume_front();
        return true;
    };
#else
    const size_t requested_depth = 1;
    const size_t actual_depth = 0;
    auto submit_batch = [&](ReadBatch batch) -> bool {
        if (batch.rows.empty()) return true;
        BatchResult result = read_batch_sync(std::move(batch));
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
        if (!row_prefix_fits(current, offset)) {
            if (!finish_current()) break;
            current = start_batch(offset, file_size_, batch_limit);
        }
        current.rows.push_back(RowRef{fid, offset});
    }

    if (!callback_stopped && !io_failed) (void)finish_current();

#ifdef _WIN32
    while (!pending.empty()) {
        if (!callback_stopped && !io_failed) {
            (void)consume_front();
        } else {
            try {
                pending.front().future.wait();
            } catch (...) {
            }
            pending.pop_front();
        }
    }
#endif

    if (io_trace_enabled()) {
        std::fprintf(stderr,
                     "fast-gdb windows dense-scan: mode=%s records=%llu "
                     "batch_reads=%llu bytes=%llu exact_reads=%llu "
                     "exact_bytes=%llu overlapped_batches=%llu "
                     "async_depth=%zu failed=%s\n",
                     requested_depth > 1 ? "fallback-overlapped" :
                                           "fallback-sync",
                     static_cast<unsigned long long>(scanned),
                     static_cast<unsigned long long>(batch_reads),
                     static_cast<unsigned long long>(bytes_read),
                     static_cast<unsigned long long>(exact_reads),
                     static_cast<unsigned long long>(exact_bytes),
                     static_cast<unsigned long long>(overlapped_batches),
                     actual_depth, io_failed ? "true" : "false");
    }
    return io_failed ? 0 : scanned;
}

uint64_t GdbTableParser::scan_geometry_candidates(
    const std::vector<uint32_t>& candidates,
    GeometryScanCallback callback) {
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
    uint64_t scanned = 0;
    uint64_t batch_reads = 0;
    uint64_t bytes_read = 0;
    uint64_t exact_reads = 0;
    uint64_t exact_bytes = 0;
    uint64_t overlapped_batches = 0;
    bool failed = false;

    std::vector<ReadBatch> batches;
    ReadBatch current;
    for (const RowRef& row : physical) {
        if (!row_prefix_fits(current, row.offset)) {
            if (!current.rows.empty()) batches.push_back(std::move(current));
            current = start_batch(row.offset, file_size_, batch_limit);
        }
        current.rows.push_back(row);
    }
    if (!current.rows.empty()) batches.push_back(std::move(current));

    auto emit_candidate = [&](uint32_t fid,
                              const uint8_t* row_begin,
                              size_t row_size) -> bool {
        if (row_size == 0) {
            if (!callback(fid, nullptr, 0, true)) return false;
            ++scanned;
            return true;
        }
        const GeometryLocation located = locate_geometry(
            fields_, geometry_field_index_, nullable_count,
            row_begin, row_begin + row_size);
        if (!located.accepted) {
            if (!callback(fid, nullptr, 0, true)) return false;
        } else if (!callback(fid, located.geometry, located.geometry_size,
                             located.geometry_null)) {
            return false;
        }
        ++scanned;
        return true;
    };

    auto read_sync = [&](ReadBatch batch) -> BatchResult {
        BatchResult result;
        result.batch = std::move(batch);
        try {
            result.bytes.resize(result.batch.size);
            result.ok = result.batch.size != 0 &&
                        read_at(result.batch.offset, result.bytes.data(),
                                result.batch.size);
        } catch (...) {
            result.ok = false;
        }
        return result;
    };

#ifdef _WIN32
    auto read_overlapped = [&](ReadBatch batch) -> BatchResult {
        BatchResult result;
        result.batch = std::move(batch);
        result.overlapped_attempted = true;
        try {
            result.bytes.resize(result.batch.size);
            result.ok = result.batch.size != 0 &&
                fast_gdb_pread_overlapped(
                    fd_, result.bytes.data(), result.batch.size,
                    static_cast<__int64>(result.batch.offset)) ==
                static_cast<ssize_t>(result.batch.size);
        } catch (...) {
            result.ok = false;
        }
        return result;
    };
    const size_t requested_depth = async_depth();
#else
    const size_t requested_depth = 1;
#endif

    auto process = [&](BatchResult& result) -> bool {
        if (result.overlapped_attempted) ++overlapped_batches;
        if (!result.ok) result = read_sync(std::move(result.batch));
        if (!result.ok) return false;
        ++batch_reads;
        bytes_read += result.bytes.size();

        for (const RowRef& row : result.batch.rows) {
            const uint64_t local64 = row.offset - result.batch.offset;
            if (local64 > result.bytes.size()) return false;
            const size_t local = static_cast<size_t>(local64);
            if (local + 4 > result.bytes.size()) return false;

            uint32_t row_size = 0;
            std::memcpy(&row_size, result.bytes.data() + local,
                        sizeof(row_size));
            if (row.offset + 4 > file_size_ ||
                row_size > file_size_ - static_cast<size_t>(row.offset + 4)) {
                return false;
            }
            if (row_size <= result.bytes.size() - local - 4) {
                if (!emit_candidate(row.fid,
                                    result.bytes.data() + local + 4,
                                    row_size)) {
                    return false;
                }
                continue;
            }

            std::vector<uint8_t> exact;
            try {
                exact.resize(row_size);
            } catch (...) {
                return false;
            }
            if (row_size != 0 &&
                !read_at(row.offset + 4, exact.data(), exact.size())) {
                return false;
            }
            ++exact_reads;
            exact_bytes += exact.size();
            if (!emit_candidate(row.fid, exact.data(), exact.size()))
                return false;
        }
        return true;
    };

#ifdef _WIN32
    struct PendingRead {
        ReadBatch batch;
        std::future<BatchResult> future;
    };
    std::deque<PendingRead> pending;
    size_t actual_depth = 0;

    auto consume = [&]() -> bool {
        PendingRead item = std::move(pending.front());
        pending.pop_front();
        BatchResult result;
        try {
            result = item.future.get();
        } catch (...) {
            result.batch = std::move(item.batch);
            result.ok = false;
            result.overlapped_attempted = true;
        }
        return process(result);
    };

    for (ReadBatch& source : batches) {
        ReadBatch batch = std::move(source);
        if (requested_depth <= 1) {
            BatchResult result = read_sync(std::move(batch));
            if (!process(result)) {
                failed = true;
                break;
            }
            continue;
        }

        PendingRead item;
        item.batch = batch;
        try {
            if (force_async_launch_failure())
                throw std::runtime_error("forced async launch failure");
            item.future = std::async(
                std::launch::async,
                [&, batch = std::move(batch)]() mutable {
                    return read_overlapped(std::move(batch));
                });
            pending.emplace_back(std::move(item));
            actual_depth = std::max(actual_depth, pending.size());
        } catch (...) {
            BatchResult result = read_sync(std::move(item.batch));
            if (!process(result)) {
                failed = true;
                break;
            }
        }
        if (pending.size() >= requested_depth && !consume()) {
            failed = true;
            break;
        }
    }
    while (!pending.empty()) {
        if (!failed) {
            if (!consume()) failed = true;
        } else {
            try {
                pending.front().future.wait();
            } catch (...) {
            }
            pending.pop_front();
        }
    }
#else
    const size_t actual_depth = 0;
    for (ReadBatch& batch : batches) {
        BatchResult result = read_sync(std::move(batch));
        if (!process(result)) {
            failed = true;
            break;
        }
    }
#endif

    if (io_trace_enabled()) {
        std::fprintf(stderr,
                     "fast-gdb windows sparse-batch: mode=%s candidates=%zu "
                     "records=%llu batch_reads=%llu bytes=%llu "
                     "exact_reads=%llu exact_bytes=%llu "
                     "overlapped_batches=%llu async_depth=%zu failed=%s\n",
                     requested_depth > 1 ? "fallback-overlapped" :
                                           "fallback-sync",
                     candidates.size(),
                     static_cast<unsigned long long>(scanned),
                     static_cast<unsigned long long>(batch_reads),
                     static_cast<unsigned long long>(bytes_read),
                     static_cast<unsigned long long>(exact_reads),
                     static_cast<unsigned long long>(exact_bytes),
                     static_cast<unsigned long long>(overlapped_batches),
                     actual_depth, failed ? "true" : "false");
    }
    return failed ? 0 : scanned;
}

} // namespace explorgdb
