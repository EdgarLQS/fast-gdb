// src/edgar/explorgdb/gdb_spatial_index.h
// .spx 空间索引解析器 — 解析 FileGDB B+ 树空间索引
//
// .spx 文件结构：
//   - N 个 4096 字节页面（B+ 树节点）
//   - 末尾 22 字节 trailer（树元数据）
//
// 索引值编码（64 位）：
//   Bit 63-62: grid_level (0=最细, 1=中, 2=最粗)
//   Bit 61-31: cell_x (31 位)
//   Bit 30-0:  cell_y (31 位)
//
// 使用方式：
//   GdbSpatialIndexParser parser("a00000001.spx");
//   parser.parse();  // 只读 trailer，不加载全部条目
//   auto fids = parser.query_bbox(xmin, ymin, xmax, ymax, ...);  // B+ 树按需导航

#ifndef EXPLORGDB_GDB_SPATIAL_INDEX_H
#define EXPLORGDB_GDB_SPATIAL_INDEX_H

#include "explorgdb_types.h"
#include <string>
#include <vector>
#include <cstdint>
#include <shared_mutex>

namespace explorgdb {

class GdbSpatialIndexParser {
public:
    static constexpr size_t kPageSize = 4096;
    static constexpr size_t kTrailerSize = 22;
    static constexpr int kMaxDepth = 4;  // .spx 最大树深度
    static constexpr int kCacheSlotsPerLevel = 4;  // 每层缓存 slot 数
    static constexpr int kTotalCacheSlots = kMaxDepth * kCacheSlotsPerLevel;  // 总 slot 数 = 16

    explicit GdbSpatialIndexParser(const std::string& file_path);
    ~GdbSpatialIndexParser();

    // 解析：只读 trailer，不加载全部条目
    bool parse();

    const BPlusTreeTrailer& trailer() const { return trailer_; }

    // 空间查询：B+ 树按需导航，返回 bbox 范围内所有 feature 的 FID
    // max_fid: 最大 FID 值（用于 bitset 去重优化，0 表示使用 sort+unique）
    std::vector<uint32_t> query_bbox(
        double xmin, double ymin, double xmax, double ymax,
        double xorig, double yorig, double xyscale,
        const std::vector<double>& grid_resolutions,
        uint32_t max_fid = 0) const;

private:
    // 解析 22 字节 trailer
    bool parse_trailer();

    // 按需读取指定页面（带 LRU cache，按 depth 分组）
    const uint8_t* read_page(uint32_t page_id, int depth) const;

    // 清空页面缓存
    void clear_cache() const;

    // GDAL FindMinMaxIdx: 完整 64-bit 二分查找
    bool find_minmax_idx(const uint8_t* page, int n_vals,
                         uint64_t min_val, uint64_t max_val,
                         int& min_idx, int& max_idx) const;

    // GDAL FindPages + GetNextRow: 递归导航 B+ 树
    void collect_fids_btree(uint32_t page_id, int depth,
                            uint64_t start_raw, uint64_t end_raw,
                            std::vector<uint32_t>& out_fids) const;

    std::string file_path_;
    int fd_ = -1;                    // 文件句柄（保持打开）
    size_t mapped_size_ = 0;         // 文件大小
    const uint8_t* mapped_data_ = nullptr;  // 全文件只读映射；失败时回退 pread
    BPlusTreeTrailer trailer_;
    int max_per_page_ = 0;           // 每页最大条目数
    int values_offset_ = 0;          // 值数组固定偏移 = 12 + max_per_page * 4

    // 页面缓存（LRU，每层 kCacheSlotsPerLevel 个 slot，共 kTotalCacheSlots 个）
    struct PageCache {
        mutable uint32_t page_id = 0;
        mutable uint8_t data[kPageSize];
        mutable bool valid = false;
        mutable uint64_t last_used = 0;  // LRU 访问时间戳
        mutable int depth = -1;  // 该 slot 对应的树深度
    };
    mutable PageCache page_cache_[kTotalCacheSlots];
    mutable uint64_t cache_counter_ = 0;  // 单调递增计数器

    mutable std::shared_mutex mutex_;  // 保护 parse 操作的线程安全
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_SPATIAL_INDEX_H
