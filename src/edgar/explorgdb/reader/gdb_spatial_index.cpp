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
    if (fd_ >= 0) close(fd_);
}

bool GdbSpatialIndexParser::parse() {
    fd_ = fast_gdb_open_utf8(file_path_.c_str(), O_RDONLY);
    if (fd_ < 0) return false;

    struct stat st;
    if (fstat(fd_, &st) < 0) { close(fd_); fd_ = -1; return false; }
    mapped_size_ = static_cast<size_t>(st.st_size);
    if (mapped_size_ < kTrailerSize) { close(fd_); fd_ = -1; return false; }

    if (!parse_trailer()) { close(fd_); fd_ = -1; return false; }

    max_per_page_ = calc_max_per_page(trailer_.value_size);
    values_offset_ = 12 + max_per_page_ * 4;

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
    // 计算当前 depth 对应的 slot 范围
    // depth 1 = 叶子层，depth tree_depth = 根节点层
    int depth_idx = (depth - 1) % kMaxDepth;  // 0..3
    int slot_start = depth_idx * kCacheSlotsPerLevel;
    int slot_end = slot_start + kCacheSlotsPerLevel;

    // 在当前 depth 的 slot 中查找缓存命中
    for (int i = slot_start; i < slot_end; i++) {
        if (page_cache_[i].valid && page_cache_[i].page_id == page_id) {
            page_cache_[i].last_used = ++cache_counter_;
            return page_cache_[i].data;
        }
    }

    // 缓存未命中，在当前 depth 的 slot 中找 LRU 或空 slot
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

    size_t off = static_cast<size_t>(page_id - 1) * kPageSize;
    if (off + kPageSize > mapped_size_) return nullptr;

    if (!found_empty) {
        page_cache_[slot].valid = false;
    }

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

// GDAL FindMinMaxIdx: 完整 64-bit 二分查找（仅用于叶子页面）
//
// 关键：GDAL 在步骤 1 后保存 maxIdxOut，步骤 2 修改 nMaxIdx 但不影响输出。
// 我们必须保存步骤 1 的结果，否则步骤 2 会覆盖它。
bool GdbSpatialIndexParser::find_minmax_idx(
    const uint8_t* page, int n_vals,
    uint64_t min_val, uint64_t max_val,
    int& min_idx, int& max_idx) const {

    const uint8_t* base = page + values_offset_;

    // 步骤 1: 找最大索引使得值 <= max_val
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
    max_idx = hi;  // 保存步骤 1 结果（GDAL 的 maxIdxOut）

    // 步骤 2: 找最小索引使得值 >= min_val
    // 使用 step1_max 作为上界，避免被覆盖
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
    // max_idx 保持步骤 1 的结果不变
    return true;
}

// B+ 树递归导航 — GDAL FindPages
//
// 分支页面语义（关键！）：
//   entry[i].raw 不是 child[i] 的最小值
//   child[i] 的 cx 范围：entry[i-1].cx < cx <= entry[i].cx
//   child[0] 的 cx 范围：cx <= entry[0].cx
//
// 匹配规则（GDAL FindPages）：
//   - 找到第一个 entry[i].cx > q_max_cx → 停止（iLastPageIdx）
//   - 如果所有 entry.cx > q_min_cx → 从 child[0] 开始（iFirstPageIdx = 0）
//   - 否则找到第一个 entry[i].cx >= q_min_cx → 从 child[i-1] 开始
//   - 遍历 [iFirstPageIdx, iLastPageIdx] 范围的 child
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
        // 叶子页面：FindMinMaxIdx 完整 64-bit 比较
        int min_idx, max_idx;
        if (!find_minmax_idx(page, entry_count, start_raw, end_raw, min_idx, max_idx)) {
            return;
        }
        for (int i = min_idx; i <= max_idx; i++) {
            uint64_t v; std::memcpy(&v, page + values_offset_ + i * 8, 8);
            if (v >= start_raw && v <= end_raw) {
                uint32_t fid; std::memcpy(&fid, (uint8_t*)page + 12 + i * 4, 4);
                if (fid != 0) out_fids.push_back(fid - 1);
            }
        }
    } else {
        // 分支页面：GDAL FindPages 逻辑
        // Find iLastPageIdx: first entry where cx > q_max_cx
        int i_last = static_cast<int>(entry_count);  // default: past end
        for (int i = 0; i < static_cast<int>(entry_count); i++) {
            uint64_t v; std::memcpy(&v, page + values_offset_ + i * 8, 8);
            uint32_t cx = static_cast<uint32_t>((v >> 31) & 0x7FFFFFFF);
            if (cx > q_max_cx) { i_last = i; break; }
        }

        // Find iFirstPageIdx: first entry where cx >= q_min_cx, then use child[i-1]
        int i_first = 0;
        for (int i = 0; i < static_cast<int>(entry_count); i++) {
            uint64_t v; std::memcpy(&v, page + values_offset_ + i * 8, 8);
            uint32_t cx = static_cast<uint32_t>((v >> 31) & 0x7FFFFFFF);
            if (cx >= q_min_cx) { i_first = (i > 0) ? i - 1 : 0; break; }
        }

        // B+ 树有 N 个 entries（分隔符）和 N+1 个 children。
        // child[N] 是最右侧子节点，包含所有 > entry[N-1] 的值。
        // GDAL 的 iLastPageIdx 上限是 nEntries（即 entry_count），
        // 所以 visit_end 最大为 entry_count（而非 entry_count - 1）。
        int visit_end = std::min(i_last, static_cast<int>(entry_count));

        if (i_first > visit_end) return;

        // 保存 child IDs 再递归调用。
        // 原因：page 指针指向 LRU 缓存 slot，递归调用可能驱逐该 slot，
        // 导致后续迭代从过期数据中读取 child_id。
        int num_children = visit_end - i_first + 1;
        uint32_t children[342];  // mpp 最大 340，分支页有 N+1=341 个 children，留余量
        for (int j = 0; j < num_children; j++) {
            std::memcpy(&children[j], (uint8_t*)page + 8 + (i_first + j) * 4, 4);
        }

        for (int j = 0; j < num_children; j++) {
            collect_fids_btree(children[j], depth - 1, start_raw, end_raw, out_fids);
        }
    }
}

std::vector<uint32_t> GdbSpatialIndexParser::query_bbox(
    double xmin, double ymin, double xmax, double ymax,
    double /*xorig*/, double /*yorig*/, double /*xyscale*/,
    const std::vector<double>& grid_resolutions,
    uint32_t max_fid) const {

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

        // Cell computation matching GDAL GetScaledCoord:
        //   1. raw = floor(coord / grid_res[0])         — base level cell index
        //   2. scaled = floor(raw / scale_factor)       — level scaling
        //   3. biased = scaled + (1 << 29)              — bias AFTER scaling
        // For max boundaries, add +1 to catch features whose geometry extends
        // into the query bbox but whose cell index is just outside.
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

        for (int64_t cx = cell_min_x; cx <= cell_max_x; cx++) {
            uint64_t start_raw = (static_cast<uint64_t>(level) << 62)
                               | (static_cast<uint64_t>(cx) << 31)
                               | static_cast<uint64_t>(cell_min_y);
            uint64_t end_raw = (static_cast<uint64_t>(level) << 62)
                             | (static_cast<uint64_t>(cx) << 31)
                             | static_cast<uint64_t>(cell_max_y);

            collect_fids_btree(1, trailer_.tree_depth, start_raw, end_raw, result_fids);
        }
    }

    // Bitset 去重优化：O(N) 去重 + O(N log N) 排序（恢复空间局部性）
    if (max_fid > 0 && max_fid < 100000000) {  // 限制最大内存占用（100M bits = 12.5MB）
        std::vector<bool> seen(max_fid + 1, false);
        std::vector<uint32_t> unique_fids;
        unique_fids.reserve(result_fids.size());
        for (uint32_t fid : result_fids) {
            if (fid <= max_fid && !seen[fid]) {
                seen[fid] = true;
                unique_fids.push_back(fid);
            }
        }
        // 排序恢复空间局部性（对 peek_geometry_blob 的 mmap 预取至关重要）
        std::sort(unique_fids.begin(), unique_fids.end());
        return unique_fids;
    } else {
        // 降级到 sort+unique（兼容旧调用或 max_fid 未知）
        std::sort(result_fids.begin(), result_fids.end());
        result_fids.erase(std::unique(result_fids.begin(), result_fids.end()), result_fids.end());
        return result_fids;
    }
}

} // namespace explorgdb
