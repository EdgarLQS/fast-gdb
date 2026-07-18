// src/edgar/explorgdb/reader/gdb_table_geometry_scan.cpp
// 几何字段扫描 — 零拷贝跳过 Geometry blob、按需物化 GeometryValue 的扫描器实现。

#include "gdb_table.h"
#include "field_layout.h"
#include "explorgdb_constants.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
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
    if (value_data != nullptr) *value_data = nullptr;
    if (value_size != nullptr) *value_size = 0;
    if (field.type == FieldType::ObjectId) return true;

    const size_t fixed_width = fixed_physical_width(field.type);
    if (fixed_width != 0) {
        if (fixed_width > static_cast<size_t>(end - cursor)) return false;
        if (value_data != nullptr) *value_data = cursor;
        if (value_size != nullptr) *value_size = fixed_width;
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
            if (value_data != nullptr) *value_data = cursor;
            if (value_size != nullptr) *value_size = size;
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
                const uint8_t* bitmap = bitmap_bytes == 0 ? nullptr : row_begin;
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
    constexpr size_t kMaximumWindowMiB = 8U;
    const char* value = std::getenv(env_name);
    if (value == nullptr || *value == '\0') return default_mebibytes * kMiB;

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0)
        return default_mebibytes * kMiB;
    return std::min<size_t>(static_cast<size_t>(parsed), kMaximumWindowMiB) * kMiB;
}

size_t dense_batch_bytes() {
    return bounded_window_bytes("FAST_GDB_WINDOWS_BATCH_MB", 4U);
}

size_t sparse_batch_bytes() {
    return bounded_window_bytes("FAST_GDB_WINDOWS_SPARSE_WINDOW_MB", 1U);
}

bool io_trace_enabled() {
    const char* value = std::getenv("FAST_GDB_WINDOWS_IO_TRACE");
    return value != nullptr && value[0] == '1';
}

#ifdef _WIN32
size_t async_depth() {
    constexpr size_t kDefaultDepth = 4U;
    constexpr size_t kMaximumDepth = 8U;
    const char* enabled = std::getenv("FAST_GDB_WINDOWS_ASYNC_IO");
    if (enabled == nullptr || enabled[0] != '1') return 1U;

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

struct SlidingMapReset {
    explicit SlidingMapReset(FastGdbSlidingMap& map) : map_(map) {}
    ~SlidingMapReset() { map_.reset(); }
    FastGdbSlidingMap& map_;
};
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

struct IoCounters {
    uint64_t batch_reads = 0;
    uint64_t batch_bytes = 0;
    uint64_t exact_reads = 0;
    uint64_t exact_bytes = 0;
    uint64_t overlapped_batches = 0;
    size_t actual_async_depth = 0;
};

bool has_record_prefix(uint64_t offset, uint64_t file_size) {
    return offset <= file_size && file_size - offset >= 4U;
}

bool valid_record_size(uint64_t offset, uint32_t row_size,
                       uint64_t file_size) {
    return has_record_prefix(offset, file_size) &&
           static_cast<uint64_t>(row_size) <= file_size - offset - 4U;
}

bool row_prefix_fits(const ReadBatch& batch, uint64_t offset) {
    if (batch.rows.empty() || offset < batch.offset) return false;
    const uint64_t local64 = offset - batch.offset;
    if (local64 > std::numeric_limits<size_t>::max()) return false;
    const size_t local = static_cast<size_t>(local64);
    return local <= batch.size && batch.size - local >= 4U;
}

ReadBatch start_batch(uint64_t offset, uint64_t file_size,
                      size_t batch_limit) {
    ReadBatch batch;
    batch.offset = offset;
    batch.size = static_cast<size_t>(std::min<uint64_t>(
        batch_limit, file_size - offset));
    return batch;
}

std::vector<ReadBatch> build_sparse_batches(
    const std::vector<RowRef>& rows,
    uint64_t file_size,
    size_t batch_limit) {
    std::vector<ReadBatch> batches;
    ReadBatch current;
    for (const RowRef& row : rows) {
        if (!row_prefix_fits(current, row.offset)) {
            if (!current.rows.empty()) batches.push_back(std::move(current));
            current = start_batch(row.offset, file_size, batch_limit);
        }
        current.rows.push_back(row);
    }
    if (!current.rows.empty()) batches.push_back(std::move(current));
    return batches;
}

void trace_scan(const char* scanner,
                const char* mode,
                uint64_t records,
                const IoCounters& counters,
                bool failed) {
    if (!io_trace_enabled()) return;
    std::fprintf(stderr,
                 "fast-gdb windows %s: mode=%s records=%llu "
                 "batch_reads=%llu bytes=%llu exact_reads=%llu "
                 "exact_bytes=%llu overlapped_batches=%llu "
                 "async_depth=%zu failed=%s\n",
                 scanner,
                 mode,
                 static_cast<unsigned long long>(records),
                 static_cast<unsigned long long>(counters.batch_reads),
                 static_cast<unsigned long long>(counters.batch_bytes),
                 static_cast<unsigned long long>(counters.exact_reads),
                 static_cast<unsigned long long>(counters.exact_bytes),
                 static_cast<unsigned long long>(counters.overlapped_batches),
                 counters.actual_async_depth,
                 failed ? "true" : "false");
}

} // namespace

uint64_t GdbTableParser::scan_geometry_blobs(GeometryScanCallback callback) {
    if (!callback || fields_.empty() || feature_offsets_.empty() ||
        geometry_field_index_ < 0 ||
        geometry_field_index_ >= static_cast<int>(fields_.size())) {
        return 0;
    }

    const uint64_t file_size64 = static_cast<uint64_t>(file_size_);
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
        if (row_begin == nullptr) return false;
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
            if (offset == 0 || offset >= file_size64) continue;
            if (!has_record_prefix(offset, file_size64)) return 0;

            uint32_t row_size = 0;
            std::memcpy(&row_size, mapped_data_ + offset, sizeof(row_size));
            if (!valid_record_size(offset, row_size, file_size64)) return 0;
            if (!emit_row(fid, mapped_data_ + offset + 4U, row_size)) break;
        }
        trace_scan("dense-scan", "mmap", scanned, IoCounters{}, false);
        return scanned;
    }

#ifdef _WIN32
    if (fd_ >= 0) {
        sliding_map_.reset();
        if (sliding_map_.open(fd_)) {
            SlidingMapReset reset(sliding_map_);
            const size_t preferred = dense_batch_bytes();
            for (uint32_t fid = 0; fid < feature_offsets_.size(); ++fid) {
                const uint64_t offset = feature_offsets_[fid];
                if (offset == 0 || offset >= file_size64) continue;
                if (!has_record_prefix(offset, file_size64)) return 0;

                const uint8_t* prefix = sliding_map_.map(offset, 4U, preferred);
                if (prefix == nullptr) return 0;
                uint32_t row_size = 0;
                std::memcpy(&row_size, prefix, sizeof(row_size));
                if (!valid_record_size(offset, row_size, file_size64)) return 0;

                if (row_size == 0) {
                    if (!emit_row(fid, nullptr, 0)) break;
                    continue;
                }
                const uint8_t* row = sliding_map_.map(
                    offset + 4U,
                    static_cast<size_t>(row_size),
                    std::max(preferred, static_cast<size_t>(row_size)));
                if (row == nullptr) return 0;
                if (!emit_row(fid, row, row_size)) break;
            }
            trace_scan("dense-scan", "mmap-windowed", scanned,
                       IoCounters{}, false);
            return scanned;
        }
    }
#endif

    if (fd_ < 0) return 0;

    const size_t batch_limit = dense_batch_bytes();
    ReadBatch current;
    IoCounters counters;
    bool io_failed = false;
    bool callback_stopped = false;

    auto read_sync = [&](ReadBatch batch) -> BatchResult {
        BatchResult result;
        result.batch = std::move(batch);
        try {
            result.bytes.resize(result.batch.size);
            result.ok = result.batch.size != 0 &&
                        read_at(result.batch.offset,
                                result.bytes.data(),
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
#endif

    auto process_batch = [&](BatchResult& result) -> bool {
        if (result.overlapped_attempted) ++counters.overlapped_batches;
        if (!result.ok) result = read_sync(std::move(result.batch));
        if (!result.ok) {
            io_failed = true;
            return false;
        }

        ++counters.batch_reads;
        counters.batch_bytes += result.bytes.size();
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
            if (local + 4U > result.bytes.size()) {
                io_failed = true;
                return false;
            }

            uint32_t row_size = 0;
            std::memcpy(&row_size,
                        result.bytes.data() + local,
                        sizeof(row_size));
            if (!valid_record_size(row.offset, row_size, file_size64)) {
                io_failed = true;
                return false;
            }

            if (row_size <= result.bytes.size() - local - 4U) {
                if (!emit_row(row.fid,
                              result.bytes.data() + local + 4U,
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
                !read_at(row.offset + 4U,
                         exact.data(), exact.size())) {
                io_failed = true;
                return false;
            }
            ++counters.exact_reads;
            counters.exact_bytes += exact.size();
            if (!emit_row(row.fid, exact.data(), exact.size())) {
                callback_stopped = true;
                return false;
            }
        }
        return true;
    };

#ifdef _WIN32
    struct PendingRead {
        ReadBatch retry_batch;
        std::future<BatchResult> future;
    };

    const size_t requested_depth = async_depth();
    std::deque<PendingRead> pending;

    auto consume_front = [&]() -> bool {
        if (pending.empty()) return true;
        PendingRead item = std::move(pending.front());
        pending.pop_front();
        BatchResult result;
        try {
            result = item.future.get();
        } catch (...) {
            result.batch = std::move(item.retry_batch);
            result.ok = false;
            result.overlapped_attempted = true;
        }
        return process_batch(result);
    };

    auto submit_batch = [&](ReadBatch batch) -> bool {
        if (batch.rows.empty()) return true;
        if (requested_depth <= 1U) {
            BatchResult result = read_sync(std::move(batch));
            return process_batch(result);
        }

        PendingRead item;
        item.retry_batch = batch;
        try {
            if (force_async_launch_failure())
                throw std::runtime_error("forced async launch failure");
            item.future = std::async(
                std::launch::async,
                [&, async_batch = std::move(batch)]() mutable {
                    return read_overlapped(std::move(async_batch));
                });
        } catch (...) {
            BatchResult result = read_sync(std::move(item.retry_batch));
            return process_batch(result);
        }
        pending.push_back(std::move(item));
        counters.actual_async_depth = std::max(
            counters.actual_async_depth, pending.size());
        if (pending.size() >= requested_depth) return consume_front();
        return true;
    };
#else
    const size_t requested_depth = 1U;
    auto submit_batch = [&](ReadBatch batch) -> bool {
        if (batch.rows.empty()) return true;
        BatchResult result = read_sync(std::move(batch));
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
        if (offset == 0 || offset >= file_size64) continue;
        if (!has_record_prefix(offset, file_size64)) {
            io_failed = true;
            break;
        }
        if (!row_prefix_fits(current, offset)) {
            if (!finish_current()) break;
            current = start_batch(offset, file_size64, batch_limit);
        }
        current.rows.push_back(RowRef{fid, offset});
    }

    if (!callback_stopped && !io_failed) {
        if (!finish_current()) io_failed = true;
    }

#ifdef _WIN32
    while (!pending.empty()) {
        if (!callback_stopped && !io_failed) {
            if (!consume_front()) io_failed = true;
        } else {
            try {
                pending.front().future.wait();
            } catch (...) {
            }
            pending.pop_front();
        }
    }
#endif

    const char* mode = requested_depth > 1U
        ? "fallback-overlapped" : "fallback-sync";
    trace_scan("dense-scan", mode, scanned, counters, io_failed);
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

    const uint64_t file_size64 = static_cast<uint64_t>(file_size_);
    std::vector<RowRef> physical;
    physical.reserve(candidates.size());
    for (uint32_t fid : candidates) {
        if (fid >= feature_offsets_.size()) return 0;
        const uint64_t offset = feature_offsets_[fid];
        if (offset == 0 || !has_record_prefix(offset, file_size64)) return 0;
        physical.push_back(RowRef{fid, offset});
    }
    std::sort(physical.begin(), physical.end(),
              [](const RowRef& left, const RowRef& right) {
                  if (left.offset != right.offset)
                      return left.offset < right.offset;
                  return left.fid < right.fid;
              });

    const int nullable_count = nullable_field_count();
    uint64_t scanned = 0;
    auto emit_candidate = [&](uint32_t fid,
                              const uint8_t* row_begin,
                              size_t row_size) -> bool {
        if (row_size == 0) {
            if (!callback(fid, nullptr, 0, true)) return false;
            ++scanned;
            return true;
        }
        if (row_begin == nullptr) return false;
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

#ifdef _WIN32
    sliding_map_.reset();
    if (sliding_map_.open(fd_)) {
        SlidingMapReset reset(sliding_map_);
        const size_t preferred = sparse_batch_bytes();
        for (const RowRef& row_ref : physical) {
            const uint8_t* prefix = sliding_map_.map(
                row_ref.offset, 4U, preferred);
            if (prefix == nullptr) return 0;
            uint32_t row_size = 0;
            std::memcpy(&row_size, prefix, sizeof(row_size));
            if (!valid_record_size(row_ref.offset, row_size, file_size64))
                return 0;

            if (row_size == 0) {
                if (!emit_candidate(row_ref.fid, nullptr, 0)) return scanned;
                continue;
            }
            const uint8_t* row = sliding_map_.map(
                row_ref.offset + 4U,
                static_cast<size_t>(row_size),
                std::max(preferred, static_cast<size_t>(row_size)));
            if (row == nullptr) return 0;
            if (!emit_candidate(row_ref.fid, row, row_size)) return scanned;
        }
        trace_scan("sparse-batch", "mmap-windowed", scanned,
                   IoCounters{}, false);
        return scanned;
    }
#endif

    const size_t batch_limit = sparse_batch_bytes();
    std::vector<ReadBatch> batches = build_sparse_batches(
        physical, file_size64, batch_limit);
    IoCounters counters;
    bool failed = false;

    auto read_sync = [&](ReadBatch batch) -> BatchResult {
        BatchResult result;
        result.batch = std::move(batch);
        try {
            result.bytes.resize(result.batch.size);
            result.ok = result.batch.size != 0 &&
                        read_at(result.batch.offset,
                                result.bytes.data(),
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
    const size_t requested_depth = 1U;
#endif

    auto process_batch = [&](BatchResult& result) -> bool {
        if (result.overlapped_attempted) ++counters.overlapped_batches;
        if (!result.ok) result = read_sync(std::move(result.batch));
        if (!result.ok) return false;

        ++counters.batch_reads;
        counters.batch_bytes += result.bytes.size();
        for (const RowRef& row_ref : result.batch.rows) {
            if (row_ref.offset < result.batch.offset) return false;
            const uint64_t local64 = row_ref.offset - result.batch.offset;
            if (local64 > result.bytes.size()) return false;
            const size_t local = static_cast<size_t>(local64);
            if (local + 4U > result.bytes.size()) return false;

            uint32_t row_size = 0;
            std::memcpy(&row_size,
                        result.bytes.data() + local,
                        sizeof(row_size));
            if (!valid_record_size(row_ref.offset, row_size, file_size64))
                return false;

            if (row_size <= result.bytes.size() - local - 4U) {
                if (!emit_candidate(row_ref.fid,
                                    result.bytes.data() + local + 4U,
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
                !read_at(row_ref.offset + 4U,
                         exact.data(), exact.size())) {
                return false;
            }
            ++counters.exact_reads;
            counters.exact_bytes += exact.size();
            if (!emit_candidate(row_ref.fid, exact.data(), exact.size()))
                return false;
        }
        return true;
    };

#ifdef _WIN32
    struct PendingRead {
        ReadBatch retry_batch;
        std::future<BatchResult> future;
    };
    std::deque<PendingRead> pending;

    auto consume_front = [&]() -> bool {
        PendingRead item = std::move(pending.front());
        pending.pop_front();
        BatchResult result;
        try {
            result = item.future.get();
        } catch (...) {
            result.batch = std::move(item.retry_batch);
            result.ok = false;
            result.overlapped_attempted = true;
        }
        return process_batch(result);
    };

    for (ReadBatch& source : batches) {
        ReadBatch batch = std::move(source);
        if (requested_depth <= 1U) {
            BatchResult result = read_sync(std::move(batch));
            if (!process_batch(result)) {
                failed = true;
                break;
            }
            continue;
        }

        PendingRead item;
        item.retry_batch = batch;
        try {
            if (force_async_launch_failure())
                throw std::runtime_error("forced async launch failure");
            item.future = std::async(
                std::launch::async,
                [&, async_batch = std::move(batch)]() mutable {
                    return read_overlapped(std::move(async_batch));
                });
        } catch (...) {
            BatchResult result = read_sync(std::move(item.retry_batch));
            if (!process_batch(result)) {
                failed = true;
                break;
            }
            continue;
        }
        pending.push_back(std::move(item));
        counters.actual_async_depth = std::max(
            counters.actual_async_depth, pending.size());
        if (pending.size() >= requested_depth && !consume_front()) {
            failed = true;
            break;
        }
    }

    while (!pending.empty()) {
        if (!failed) {
            if (!consume_front()) failed = true;
        } else {
            try {
                pending.front().future.wait();
            } catch (...) {
            }
            pending.pop_front();
        }
    }
#else
    for (ReadBatch& batch : batches) {
        BatchResult result = read_sync(std::move(batch));
        if (!process_batch(result)) {
            failed = true;
            break;
        }
    }
#endif

    const char* mode = requested_depth > 1U
        ? "fallback-overlapped" : "fallback-sync";
    trace_scan("sparse-batch", mode, scanned, counters, failed);
    return failed ? 0 : scanned;
}

} // namespace explorgdb
