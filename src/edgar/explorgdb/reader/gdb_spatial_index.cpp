// src/edgar/explorgdb/gdb_spatial_index.cpp
// .spx 空间索引解析实现 — B+ 树按需导航（参考 GDAL OpenFileGDB 实现）
//
// 页面布局（value_size=8）：
//   分支页面：
//     bytes 0-3:   next_page_id
//     bytes 4-7:   entry_count
//     bytes 8..8+N*4:     child_page_id 数组 (N+1 × 4)
//     bytes 12+mpp*4..:   值数组 (N × 8) — 固定偏移
//   叶子页面：
//     bytes 0-3:   next_page_id
//     bytes 4-7:   entry_count
//     bytes 8-11:  UNUSED (padding)
//     bytes 12..12+N*4:     fid 数组 (N × 4)
//     bytes 12+mpp*4..:     值数组 (N × 8) — 固定偏移
//
// B+ 树语义（GDAL FindPages）：
//   - entry[i] 是分隔符，entry[i].raw 是 child[i] 范围的上界参考
//   - child[i] 的 cx 范围是 [entry[i-1].cx+1, entry[i].cx]
//     （child[0] 的范围是 [-inf, entry[0].cx]）
//   - 分支层：entry[i].cx <= q_max_cx → 访问 child[i]
//   - 叶子层：FindMinMaxIdx 完整 64-bit 比较

#include "gdb_spatial_index.h"
#include "binary_reader.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace explorgdb {

static inline int calc_max_per_page(uint8_t value_size) {
    return static_cast<int>((GdbSpatialIndexParser::kPageSize - 12) / (4 + value_size));
}

GdbSpatialIndexParser::GdbSpatialIndexParser(const std::string& file_path)
    : file_path_(file_path) {}

GdbSpatialIndexParser::~GdbSpatialIndexParser() {
    if (mapped_data_) munmap(const_cast<uint8_t*>(mapped_data_), mapped_size_);
    if (fd_ >= 0) close(fd_);
}

bool GdbSpatialIndexParser::parse() {
#ifdef _WIN32
    fd_ = fast_gdb_open_utf8(file_path_.c_str(), O_RDONLY);
#else
    fd_ = ::open(file_path_.c_str(), O_RDONLY);
#endif
    if (fd_ < 0) return false;

    struct stat st;
    if (fstat(fd_, &st) < 0) { close(fd_); fd_ = -1; return false; }
    mapped_size_ = static_cast<size_t>(st.st_size);
    if (mapped_size_ < kTrailerSize) { close(fd_); fd_ = -1; return false; }

    if (!parse_trailer()) { close(fd_); fd_ = -1; return false; }

    max_per_page_ = calc_max_per_page(trailer_.value_size);
    values_offset_ = 12 + max_per_page_ * 4;
    void* mapping = mmap(nullptr, mapped_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapping != MAP_FAILED) {
        mapped_data_ = static_cast<const uint8_t*>(mapping);
    }

    return true;
}

bool GdbSpatialIndexParser::parse_trailer() {
    size_t trailer_off = mapped_size_ - kTrailerSize;
    uint8_t buf[kTrailerSize];
    if (pread(fd_, buf, kTrailerSize, trailer_off) != kTrailerSize) return false;

    BinaryReader br(buf, kTrailerSize);
    trailer_.value_size = br.read_u8();
    uint8_t flags = br.read_u8();
    trailer_.is_string = (flags & 0x20) != 0;
    trailer_.is_numeric = (flags & 0x40) != 0;
    trailer_.magic1 = br.read_u32();
    trailer_.tree_depth = br.read_u32();
    trailer_.total_value_count = br.read_u32();

    if (trailer_.magic1 != 1) return false;
    if (trailer_.value_size != 8) return false;
    if (trailer_.tree_depth < 1 || trailer_.tree_depth > 4) return false;
    return true;
}

const uint8_t* GdbSpatialIndexParser::read_page(uint32_t page_id, int depth) const {
    size_t off = static_cast<size_t>(page_id - 1) * kPageSize;
    if (page_id == 0 || off + kPageSize > mapped_size_) return nullptr;
    if (mapped_data_) return mapped_data_ + off;

    int depth_idx = (depth - 1) % kMaxDepth;
    int slot_start = depth_idx * kCacheSlotsPerLevel;
    int slot_end = slot_start + kCacheSlotsPerLevel;

    for (int i = slot_start; i < slot_end; i++) {
        if (page_cache_[i].valid && page_cache_[i].page_id == page_id) {
            page_cache_[i].last_used = ++cache_counter_;
            return page_cache_[i].data;
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

    if (pread(fd_, page_cache_[slot].data, kPageSize, off) != kPageSize) {
        page_cache_[slot].valid = false;
        return nullptr;
    }
    return page_cache_[slot].data;
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
        uint64_t v; std::memcpy(&v, base + mid * 8, 8);
        if (v <= max_val) lo = mid; else hi = mid;
    }
    while (true) {
        uint64_t v; std::memcpy(&v, base + hi * 8, 8);
        if (v <= max_val) break;
        hi--;
        if (hi < 0) return false;
    }
    max_idx = hi;

    int step1_max = max_idx;
    lo = 0;
    while (step1_max - lo >= 2) {
        int mid = (lo + step1_max) / 2;
        uint64_t v; std::memcpy(&v, base + mid * 8, 8);
        if (v >= min_val) step1_max = mid; else lo = mid;
    }
    while (true) {
        uint64_t v; std::memcpy(&v, base + lo * 8, 8);
        if (v >= min_val) break;
        lo++;
        if (lo == n_vals) return false;
    }
    min_idx = lo;
    return true;
}

void GdbSpatialIndexParser::collect_fids_btree(
    uint32_t page_id, int depth,
    uint64_t start_raw, uint64_t end_raw,
    std::vector<uint32_t>& out_fids) const {

    if (page_id == 0 || depth <= 0) return;
    const uint8_t* page = read_page(page_id, depth);
    if (!page) return;

    uint32_t entry_count;
    std::memcpy(&entry_count, page + 4, 4);
    if (entry_count == 0 || entry_count >
        static_cast<uint32_t>(max_per_page_))
        return;

    uint32_t q_min_cx = static_cast<uint32_t>((start_raw >> 31) & 0x7FFFFFFF);
    uint32_t q_max_cx = static_cast<uint32_t>((end_raw >> 31) & 0x7FFFFFFF);

    if (depth == 1) {
        int min_idx, max_idx;
        if (!find_minmax_idx(page, entry_count, start_raw, end_raw, min_idx, max_idx))
            return;
        for (int i = min_idx; i <= max_idx; i++) {
            uint64_t v; std::memcpy(&v, page + values_offset_ + i * 8, 8);
            if (v >= start_raw && v <= end_raw) {
                uint32_t fid;
                std::memcpy(&fid, (uint8_t*)page + 12 + i * 4, 4);
                if (fid != 0) out_fids.push_back(fid - 1);
            }
        }
    } else {
        int i_last = static_cast<int>(entry_count);
        for (int i = 0; i < static_cast<int>(entry_count); i++) {
            uint64_t v; std::memcpy(&v, page + values_offset_ + i * 8, 8);
            uint32_t cx = static_cast<uint32_t>((v >> 31) & 0x7FFFFFFF);
            if (cx > q_max_cx) { i_last = i; break; }
        }

        int i_first = 0;
        for (int i = 0; i < static_cast<int>(entry_count); i++) {
            uint64_t v; std::memcpy(&v, page + values_offset_ + i * 8, 8);
            uint32_t cx = static_cast<uint32_t>((v >> 31) & 0x7FFFFFFF);
            if (cx >= q_min_cx) { i_first = (i > 0) ? i - 1 : 0; break; }
        }

        int visit_end = std::min(i_last, static_cast<int>(entry_count));
        if (i_first > visit_end) return;

        int num_children = visit_end - i_first + 1;
        uint32_t children[342];
        for (int j = 0; j < num_children; j++) {
            std::memcpy(&children[j], (uint8_t*)page + 8 + (i_first + j) * 4, 4);
        }
        for (int j = 0; j < num_children; j++) {
            collect_fids_btree(children[j], depth - 1,
                               start_raw, end_raw, out_fids);
        }
    }
}

std::vector<uint32_t> GdbSpatialIndexParser::query_bbox(
    double xmin, double ymin, double xmax, double ymax,
    double /*xorig*/, double /*yorig*/, double /*xyscale*/,
    const std::vector<double>& grid_resolutions,
    uint32_t max_fid,
    bool merge_x_ranges) const {

    clear_cache();
    std::vector<uint32_t> result_fids;
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
            collect_fids_btree(
                1, trailer_.tree_depth, start_raw, end_raw, result_fids);
            continue;
        }

        for (int64_t cx = cell_min_x; cx <= cell_max_x; cx++) {
            uint64_t start_raw = (static_cast<uint64_t>(level) << 62)
                               | (static_cast<uint64_t>(cx) << 31)
                               | static_cast<uint64_t>(cell_min_y);
            uint64_t end_raw = (static_cast<uint64_t>(level) << 62)
                             | (static_cast<uint64_t>(cx) << 31)
                             | static_cast<uint64_t>(cell_max_y);
            collect_fids_btree(1, trailer_.tree_depth,
                               start_raw, end_raw, result_fids);
        }
    }

    if (max_fid > 0 && max_fid < 100000000) {
        std::vector<bool> seen(max_fid + 1, false);
        size_t unique_count = 0;
        for (uint32_t fid : result_fids) {
            if (fid <= max_fid && !seen[fid]) {
                seen[fid] = true;
                ++unique_count;
            }
        }

        std::vector<uint32_t> unique_fids;
        unique_fids.reserve(unique_count);
        const size_t fid_domain = static_cast<size_t>(max_fid) + 1U;
        const bool dense_enough_to_enumerate =
            unique_count != 0 && fid_domain / unique_count <= 16U;
        if (dense_enough_to_enumerate) {
            for (uint32_t fid = 0; fid <= max_fid; ++fid) {
                if (seen[fid]) unique_fids.push_back(fid);
                if (fid == max_fid) break;
            }
        } else {
            for (uint32_t fid : result_fids) {
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
