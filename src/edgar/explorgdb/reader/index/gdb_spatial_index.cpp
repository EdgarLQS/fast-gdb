// src/edgar/explorgdb/gdb_spatial_index.cpp
// .spx 空间索引解析实现 — B+ 树按需导航（参考 GDAL OpenFileGDB 实现）
//
// 页面布局（value_size=8）：v1 使用 4096 字节页和 4 字节 ID，v2 使用
// 65536 字节页和 8 字节 ID；header、子页/FID 数组及值数组偏移由版本决定。
//
// B+ 树语义（GDAL FindPages）：
//   - entry[i] 是分隔符，entry[i].raw 是 child[i] 范围的上界参考
//   - child[i] 的 cx 范围是 [entry[i-1].cx+1, entry[i].cx]
//     （child[0] 的范围是 [-inf, entry[0].cx]）
//   - 分支层：entry[i].cx <= q_max_cx → 访问 child[i]
//   - 叶子层：FindMinMaxIdx 完整 64-bit 比较

#include "gdb_spatial_index.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <limits>

namespace explorgdb {

namespace {

uint32_t read_u32_le(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8U) |
           (static_cast<uint32_t>(bytes[2]) << 16U) |
           (static_cast<uint32_t>(bytes[3]) << 24U);
}

ObjectId read_object_id(const uint8_t* bytes, uint8_t width) {
    ObjectId value = 0U;
    for (size_t index = 0; index < width; ++index) {
        value |= static_cast<ObjectId>(bytes[index]) << (index * 8U);
    }
    return value;
}

size_t leaf_header_size(uint32_t version) {
    return version == 1U ? 12U : 20U;
}

size_t nonleaf_header_size(uint32_t version) {
    return version == 1U ? 8U : 12U;
}

size_t calc_max_per_page(uint8_t value_size,
                         size_t page_size,
                         uint8_t object_id_size,
                         size_t leaf_header) {
    if (page_size < leaf_header || value_size == 0U || object_id_size == 0U)
        return 0U;
    return (page_size - leaf_header) /
           (object_id_size + static_cast<size_t>(value_size));
}

} // namespace

GdbSpatialIndexParser::GdbSpatialIndexParser(const std::string& file_path)
    : file_path_(file_path) {}

bool GdbSpatialIndexParser::supports_layer_extent(
    double xmin, double ymin, double xmax, double ymax,
    const std::vector<double>& grid_resolutions) {
    if (grid_resolutions.empty() ||
        grid_resolutions.size() > kMaxDepth ||
        !std::isfinite(grid_resolutions.front()) ||
        grid_resolutions.front() <= 0.0 ||
        !std::isfinite(xmin) || !std::isfinite(ymin) ||
        !std::isfinite(xmax) || !std::isfinite(ymax) ||
        xmin > xmax || ymin > ymax) {
        return false;
    }
    for (double resolution : grid_resolutions) {
        if (!std::isfinite(resolution) || resolution <= 0.0) return false;
    }

    const long double grid_resolution = grid_resolutions.front();
    const long double cell_offset = 1LL << 29U;
    const long double maximum_cell =
        static_cast<long double>(std::numeric_limits<int32_t>::max());
    const auto scaled_center_is_representable = [&](double minimum,
                                                     double maximum) {
        const long double center = static_cast<long double>(minimum) +
            (static_cast<long double>(maximum) - minimum) / 2.0L;
        const long double scaled = center / grid_resolution + cell_offset;
        return std::isfinite(scaled) && scaled >= 0.0L &&
               scaled <= maximum_cell;
    };

    return scaled_center_is_representable(xmin, xmax) &&
           scaled_center_is_representable(ymin, ymax);
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
GdbSpatialIndexParser::~GdbSpatialIndexParser() {
    if (mapped_data_) munmap(const_cast<uint8_t*>(mapped_data_), mapped_size_);
    if (fd_ >= 0) close(fd_);
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbSpatialIndexParser::parse() {
    if (mapped_data_) {
        munmap(const_cast<uint8_t*>(mapped_data_), mapped_size_);
        mapped_data_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    mapped_size_ = 0U;
    page_count_ = 0U;
    trailer_ = BPlusTreeTrailer{};
    max_per_page_ = 0U;
    values_offset_ = 0U;
    clear_cache();

#ifdef _WIN32
    fd_ = fast_gdb_open_utf8(file_path_.c_str(), O_RDONLY);
#else
    fd_ = ::open(file_path_.c_str(), O_RDONLY);
#endif
    if (fd_ < 0) return false;

    struct stat st;
    if (fstat(fd_, &st) < 0) { close(fd_); fd_ = -1; return false; }
    if (st.st_size < 0 || static_cast<uintmax_t>(st.st_size) >
            static_cast<uintmax_t>(std::numeric_limits<size_t>::max())) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    mapped_size_ = static_cast<size_t>(st.st_size);
    if (mapped_size_ < sizeof(uint32_t)) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    if (!parse_trailer()) { close(fd_); fd_ = -1; return false; }

    const size_t trailer_size = trailer_.version == 1U
        ? kTrailerSize : kV2TrailerSize;
    if (mapped_size_ < trailer_size) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    const size_t page_bytes = mapped_size_ - trailer_size;
    if (page_bytes % trailer_.page_size != 0U) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    page_count_ = page_bytes / trailer_.page_size;
    if (page_count_ == 0U || trailer_.tree_depth > page_count_) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    max_per_page_ = calc_max_per_page(
        trailer_.value_size, trailer_.page_size, trailer_.object_id_size,
        leaf_header_size(trailer_.version));
    values_offset_ = leaf_header_size(trailer_.version) +
        max_per_page_ * trailer_.object_id_size;
    if (max_per_page_ == 0U || values_offset_ > trailer_.page_size ||
        trailer_.value_size > trailer_.page_size - values_offset_) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    for (PageCache& page : page_cache_) {
        page.data.resize(trailer_.page_size);
    }
    void* mapping = mmap(nullptr, mapped_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapping != MAP_FAILED) {
        mapped_data_ = static_cast<const uint8_t*>(mapping);
    }

    return true;
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbSpatialIndexParser::parse_trailer() {
    uint8_t version_bytes[sizeof(uint32_t)]{};
    const off_t version_offset = static_cast<off_t>(
        mapped_size_ - sizeof(version_bytes));
    if (pread(fd_, version_bytes, sizeof(version_bytes), version_offset) !=
        static_cast<ssize_t>(sizeof(version_bytes))) return false;
    trailer_.version = read_u32_le(version_bytes);
    if (trailer_.version == 1U) {
        trailer_.page_size = static_cast<uint32_t>(kPageSize);
        trailer_.object_id_size = 4U;
    } else if (trailer_.version == 2U) {
        trailer_.page_size = static_cast<uint32_t>(kV2PageSize);
        trailer_.object_id_size = 8U;
    } else {
        return false;
    }
    const size_t trailer_size = trailer_.version == 1U
        ? kTrailerSize : kV2TrailerSize;
    if (mapped_size_ < trailer_size) return false;
    uint8_t bytes[kV2TrailerSize]{};
    const size_t trailer_offset = mapped_size_ - trailer_size;
    if (pread(fd_, bytes, trailer_size, static_cast<off_t>(trailer_offset)) !=
        static_cast<ssize_t>(trailer_size)) return false;

    trailer_.value_size = bytes[0];
    const uint8_t flags = bytes[1];
    trailer_.is_string = (flags & 0x20U) != 0U;
    trailer_.is_numeric = (flags & 0x40U) != 0U;
    trailer_.magic1 = read_u32_le(bytes + 2U);
    trailer_.tree_depth = read_u32_le(bytes + 6U);
    trailer_.total_value_count = trailer_.version == 1U
        ? read_u32_le(bytes + 10U) : read_object_id(bytes + 10U, 8U);
    const uint32_t stored_version = read_u32_le(
        bytes + trailer_size - sizeof(uint32_t));

    if (trailer_.magic1 != 1U || stored_version != trailer_.version)
        return false;
    if (trailer_.value_size != 8) return false;
    if (trailer_.tree_depth < 1U || trailer_.tree_depth > 4U) return false;
    return true;
}

const uint8_t* GdbSpatialIndexParser::read_page(
    ObjectId page_id, int depth) const {
    if (page_id == 0U || page_id > page_count_) return nullptr;
    const ObjectId page_index = page_id - 1U;
    if (page_index > static_cast<ObjectId>(
                         std::numeric_limits<size_t>::max() /
                         trailer_.page_size)) return nullptr;
    const size_t off = static_cast<size_t>(page_index) * trailer_.page_size;
    const size_t page_bytes = page_count_ * trailer_.page_size;
    if (off > page_bytes || page_bytes - off < trailer_.page_size)
        return nullptr;
    if (mapped_data_) return mapped_data_ + off;

    int depth_idx = (depth - 1) % kMaxDepth;
    int slot_start = depth_idx * kCacheSlotsPerLevel;
    int slot_end = slot_start + kCacheSlotsPerLevel;

    for (int i = slot_start; i < slot_end; i++) {
        if (page_cache_[i].valid && page_cache_[i].page_id == page_id) {
            page_cache_[i].last_used = ++cache_counter_;
            return page_cache_[i].data.data();
        }
    }

    int slot = slot_start;
    uint64_t min_ts = page_cache_[slot_start].last_used;
    bool found_empty = false;
    for (int i = slot_start; i < slot_end; i++) {
        if (!page_cache_[i].valid) {
            slot = i;
            found_empty = true;
            break;
        }
        if (page_cache_[i].last_used < min_ts) {
            min_ts = page_cache_[i].last_used;
            slot = i;
        }
    }

    if (!found_empty) page_cache_[slot].valid = false;
    page_cache_[slot].page_id = page_id;
    page_cache_[slot].valid = true;
    page_cache_[slot].last_used = ++cache_counter_;
    page_cache_[slot].depth = depth;

    if (pread(fd_, page_cache_[slot].data.data(), trailer_.page_size,
              static_cast<off_t>(off)) !=
        static_cast<ssize_t>(trailer_.page_size)) {
        page_cache_[slot].valid = false;
        return nullptr;
    }
    return page_cache_[slot].data.data();
}

void GdbSpatialIndexParser::clear_cache() const {
    for (int i = 0; i < kTotalCacheSlots; i++) {
        page_cache_[i].valid = false;
        page_cache_[i].last_used = 0;
        page_cache_[i].depth = -1;
    }
    cache_counter_ = 0;
}

bool GdbSpatialIndexParser::find_minmax_idx(
    const uint8_t* page, int n_vals,
    uint64_t min_val, uint64_t max_val,
    int& min_idx, int& max_idx) const {

    const uint8_t* base = page + values_offset_;
    int lo = 0, hi = n_vals - 1;
    while (hi - lo >= 2) {
        int mid = (lo + hi) / 2;
        const uint64_t v = read_object_id(base + mid * 8, 8U);
        if (v <= max_val) lo = mid; else hi = mid;
    }
    while (true) {
        const uint64_t v = read_object_id(base + hi * 8, 8U);
        if (v <= max_val) break;
        hi--;
        if (hi < 0) return false;
    }
    max_idx = hi;

    int step1_max = max_idx;
    lo = 0;
    while (step1_max - lo >= 2) {
        int mid = (lo + step1_max) / 2;
        const uint64_t v = read_object_id(base + mid * 8, 8U);
        if (v >= min_val) step1_max = mid; else lo = mid;
    }
    while (true) {
        const uint64_t v = read_object_id(base + lo * 8, 8U);
        if (v >= min_val) break;
        lo++;
        if (lo == n_vals) return false;
    }
    min_idx = lo;
    return true;
}

bool GdbSpatialIndexParser::collect_fids_btree(
    ObjectId page_id, int depth,
    uint64_t start_raw, uint64_t end_raw,
    std::vector<RowSlot>& out_fids) const {

    if (page_id == 0 || depth <= 0) return false;
    const uint8_t* page = read_page(page_id, depth);
    if (!page) return false;

    const uint32_t entry_count = read_u32_le(page + trailer_.object_id_size);
    if (entry_count == 0U) return depth == 1;
    if (entry_count > max_per_page_) return false;

    uint32_t q_min_cx = static_cast<uint32_t>((start_raw >> 31) & 0x7FFFFFFF);
    uint32_t q_max_cx = static_cast<uint32_t>((end_raw >> 31) & 0x7FFFFFFF);

    if (depth == 1) {
        int min_idx, max_idx;
        if (!find_minmax_idx(page, entry_count, start_raw, end_raw, min_idx, max_idx))
            return true;
        for (int i = min_idx; i <= max_idx; i++) {
            const uint64_t v = read_object_id(
                page + values_offset_ + static_cast<size_t>(i) * 8U, 8U);
            if (v >= start_raw && v <= end_raw) {
                const ObjectId fid = read_object_id(
                    page + leaf_header_size(trailer_.version) +
                        static_cast<size_t>(i) * trailer_.object_id_size,
                    trailer_.object_id_size);
                RowSlot row_slot = 0U;
                if (!object_id_to_row_slot(fid, row_slot)) return false;
                out_fids.push_back(row_slot);
            }
        }
    } else {
        int i_last = static_cast<int>(entry_count);
        for (int i = 0; i < static_cast<int>(entry_count); i++) {
            const uint64_t v = read_object_id(
                page + values_offset_ + static_cast<size_t>(i) * 8U, 8U);
            uint32_t cx = static_cast<uint32_t>((v >> 31) & 0x7FFFFFFF);
            if (cx > q_max_cx) { i_last = i; break; }
        }

        int i_first = 0;
        for (int i = 0; i < static_cast<int>(entry_count); i++) {
            const uint64_t v = read_object_id(
                page + values_offset_ + static_cast<size_t>(i) * 8U, 8U);
            uint32_t cx = static_cast<uint32_t>((v >> 31) & 0x7FFFFFFF);
            if (cx >= q_min_cx) { i_first = (i > 0) ? i - 1 : 0; break; }
        }

        int visit_end = std::min(i_last, static_cast<int>(entry_count));
        if (i_first > visit_end) return true;

        const int num_children = visit_end - i_first + 1;
        const size_t child_offset = nonleaf_header_size(trailer_.version);
        std::vector<ObjectId> children(static_cast<size_t>(num_children));
        for (int j = 0; j < num_children; j++) {
            const size_t child_index = static_cast<size_t>(i_first + j);
            children[static_cast<size_t>(j)] = read_object_id(
                page + child_offset + child_index * trailer_.object_id_size,
                trailer_.object_id_size);
        }
        for (size_t j = 0; j < children.size(); ++j) {
            const ObjectId child = children[j];
            // Forced single-child roots store count=1 and one pointer only.
            if (child == 0 && entry_count == 1 && i_first + j == 1 &&
                children.front() != 0) continue;
            if (!collect_fids_btree(child, depth - 1,
                                    start_raw, end_raw, out_fids)) return false;
        }
    }
    return true;
}

std::vector<RowSlot> GdbSpatialIndexParser::query_bbox(
    double xmin, double ymin, double xmax, double ymax,
    double /*xorig*/, double /*yorig*/, double /*xyscale*/,
    const std::vector<double>& grid_resolutions,
    RowSlot max_fid,
    bool merge_x_ranges, bool* query_succeeded) const {

    if (query_succeeded) *query_succeeded = false;
    clear_cache();
    std::vector<RowSlot> result_fids;
    if (mapped_size_ == 0 || grid_resolutions.empty() ||
        grid_resolutions.size() > kMaxDepth || trailer_.tree_depth == 0 ||
        !std::isfinite(xmin) || !std::isfinite(ymin) ||
        !std::isfinite(xmax) || !std::isfinite(ymax) ||
        xmin > xmax || ymin > ymax)
        return result_fids;

    for (double resolution : grid_resolutions) {
        if (!std::isfinite(resolution) || resolution <= 0.0)
            return result_fids;
    }

    for (size_t level = 0; level < grid_resolutions.size(); ++level) {
        const long double scale_factor =
            static_cast<long double>(grid_resolutions[level]) /
            static_cast<long double>(grid_resolutions[0]);
        auto clamp31 = [](long double value) -> int64_t {
            if (std::isnan(value) || value <= 0.0L) return 0;
            if (!std::isfinite(value) || value >= 0x7FFFFFFF)
                return 0x7FFFFFFF;
            return static_cast<int64_t>(value);
        };
        auto cell_for_min = [&](double coord) -> int64_t {
            const long double raw = std::floor(
                static_cast<long double>(coord) / grid_resolutions[0]);
            const long double scaled = std::floor(raw / scale_factor);
            return clamp31(scaled + (1LL << 29));
        };
        auto cell_for_max = [&](double coord) -> int64_t {
            const long double raw = std::floor(
                static_cast<long double>(coord) / grid_resolutions[0]);
            const long double scaled = std::floor(raw / scale_factor);
            return clamp31(scaled + (1LL << 29) + 1);
        };

        int64_t cell_min_x = cell_for_min(xmin);
        int64_t cell_max_x = cell_for_max(xmax);
        int64_t cell_min_y = cell_for_min(ymin);
        int64_t cell_max_y = cell_for_max(ymax);

        if (merge_x_ranges) {
            const uint64_t start_raw =
                (static_cast<uint64_t>(level) << 62) |
                (static_cast<uint64_t>(cell_min_x) << 31) |
                static_cast<uint64_t>(cell_min_y);
            const uint64_t end_raw =
                (static_cast<uint64_t>(level) << 62) |
                (static_cast<uint64_t>(cell_max_x) << 31) |
                static_cast<uint64_t>(cell_max_y);
            if (!collect_fids_btree(
                1, trailer_.tree_depth, start_raw, end_raw, result_fids)) return {};
            continue;
        }

        for (int64_t cx = cell_min_x; cx <= cell_max_x; cx++) {
            uint64_t start_raw = (static_cast<uint64_t>(level) << 62)
                               | (static_cast<uint64_t>(cx) << 31)
                               | static_cast<uint64_t>(cell_min_y);
            uint64_t end_raw = (static_cast<uint64_t>(level) << 62)
                             | (static_cast<uint64_t>(cx) << 31)
                             | static_cast<uint64_t>(cell_max_y);
            if (!collect_fids_btree(1, trailer_.tree_depth,
                                    start_raw, end_raw, result_fids)) return {};
        }
    }

    if (query_succeeded) *query_succeeded = true;
    if (max_fid > 0 && max_fid < 100000000) {
        std::vector<bool> seen(static_cast<size_t>(max_fid + 1U), false);
        size_t unique_count = 0;
        for (RowSlot fid : result_fids) {
            if (fid <= max_fid && !seen[fid]) {
                seen[fid] = true;
                ++unique_count;
            }
        }

        std::vector<RowSlot> unique_fids;
        unique_fids.reserve(unique_count);
        const size_t fid_domain = static_cast<size_t>(max_fid) + 1U;
        const bool dense_enough_to_enumerate =
            unique_count != 0 && fid_domain / unique_count <= 16U;
        if (dense_enough_to_enumerate) {
            for (RowSlot fid = 0; fid <= max_fid; ++fid) {
                if (seen[fid]) unique_fids.push_back(fid);
                if (fid == max_fid) break;
            }
        } else {
            for (RowSlot fid : result_fids) {
                if (fid <= max_fid && seen[fid]) {
                    unique_fids.push_back(fid);
                    seen[fid] = false;
                }
            }
            std::sort(unique_fids.begin(), unique_fids.end());
        }
        return unique_fids;
    }

    std::sort(result_fids.begin(), result_fids.end());
    result_fids.erase(
        std::unique(result_fids.begin(), result_fids.end()),
        result_fids.end());
    return result_fids;
}

} // namespace explorgdb
