// src/edgar/explorgdb/gdb_attribute_index.h
// .atx 属性索引解析器 — 解析 FileGDB B+ 树属性索引
//
// .atx 文件结构：与 .spx 相同（4096 字节页面 + 22 字节 trailer）
//
// 索引值编码（取决于 value_size + flags）：
//   value_size=2  → INT16 (LE uint16)
//   value_size=4  → INT32/FLOAT32 (LE)
//   value_size=8  → INT64/FLOAT64/DATE (LE double)
//   value_size>8  → STRING (UTF16-LE, 空格填充, 最大 80 字符)
//   value_size=38 → GUID (ASCII UUID)
//
// parse() 保留完整物化接口，供诊断和兼容测试使用。查询热路径应优先使用
// query_*_direct()：它验证完整叶链，但只物化真正命中的 FID。

#ifndef EXPLORGDB_GDB_ATTRIBUTE_INDEX_H
#define EXPLORGDB_GDB_ATTRIBUTE_INDEX_H

#include "explorgdb_types.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace explorgdb {

// 比较操作符
enum class AttrOp { Eq, Lt, Le, Gt, Ge, Ne };

struct AttributeIndexQueryMetrics {
    size_t file_bytes = 0;
    size_t page_count = 0;
    size_t pages_visited = 0;
    size_t entries_scanned = 0;
    size_t candidate_count = 0;
    double file_load_ms = 0.0;
    double trailer_ms = 0.0;
    double tree_navigation_ms = 0.0;
    double leaf_scan_ms = 0.0;
    double candidate_order_ms = 0.0;
    double total_ms = 0.0;
};

class GdbAttributeIndexParser {
public:
    static constexpr size_t kPageSize = 4096;
    static constexpr size_t kTrailerSize = 22;

    explicit GdbAttributeIndexParser(const std::string& file_path);

    // 解析整个文件并物化全部条目。任何页面越界、循环页链、条目计数
    // 不一致或零 FID 都会失败。
    bool parse();

    const BPlusTreeTrailer& trailer() const { return trailer_; }
    const std::vector<AttributeIndexEntry>& all_entries() const {
        return all_entries_;
    }

    std::vector<uint32_t> query_double(double value, AttrOp op) const;
    std::vector<uint32_t> query_string(
        const std::string& value, AttrOp op) const;

    // 查询热路径：读取文件并直接扫描经根节点定位到的完整叶链，不构造
    // all_entries_。只有在完整结构校验通过后才发布排序、去重后的零基 FID。
    bool query_double_direct(
        double value,
        AttrOp op,
        size_t max_fid_count,
        std::vector<uint32_t>& result,
        AttributeIndexQueryMetrics* metrics = nullptr);
    bool query_string_direct(
        const std::string& value,
        AttrOp op,
        size_t max_fid_count,
        std::vector<uint32_t>& result,
        AttributeIndexQueryMetrics* metrics = nullptr);

private:
    bool parse_trailer();
    bool traverse_tree(uint32_t page_id,
                       int depth_remaining,
                       size_t& remaining_page_visits);

    struct PageInfo {
        uint32_t next_page_id;
        uint32_t entry_count;
    };
    PageInfo parse_page_info(size_t page_offset) const;

    void parse_leaf_page(
        size_t page_offset, std::vector<AttributeIndexEntry>& out);
    std::vector<uint32_t> parse_nonleaf_page(size_t page_offset);

    AttributeIndexEntry decode_value(
        const uint8_t* bytes, uint8_t value_size,
        bool is_string, uint32_t fid) const;

    int compare_value(const AttributeIndexEntry& entry,
                      double numeric,
                      const std::string& str,
                      bool is_string) const;

    bool query_direct(
        double numeric_value,
        const std::string& string_value,
        bool is_string,
        AttrOp op,
        size_t max_fid_count,
        std::vector<uint32_t>& result,
        AttributeIndexQueryMetrics* metrics);

    std::string file_path_;
    std::vector<uint8_t> file_data_;
    BPlusTreeTrailer trailer_;
    std::vector<AttributeIndexEntry> all_entries_;
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_ATTRIBUTE_INDEX_H
