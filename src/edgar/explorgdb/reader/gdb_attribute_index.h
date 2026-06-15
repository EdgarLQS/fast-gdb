// src/edgar/explorgdb/gdb_attribute_index.h
// .atx 属性索引解析器 — 解析 FileGDB B+ 树属性索引
//
// .atx 文件结构：与 .spx 相同（4096 字节页面 + 22 字节 trailer）
//
// 索引值编码（取决于 value_size 和 flags）：
//   value_size=2  → INT16 (LE uint16)
//   value_size=4  → INT32/FLOAT32 (LE)
//   value_size=8  → INT64/FLOAT64/DATE (LE double)
//   value_size>8  → STRING (UTF16-LE, 空格填充, 最大 80 字符)
//   value_size=38 → GUID (ASCII UUID)
//
// 使用方式：
//   GdbAttributeIndexParser parser("a00000001.MyIndex.atx");
//   parser.parse();
//   auto entries = parser.all_entries();  // 遍历所有条目
//   auto fids = parser.query_double(42.0, "=");  // 数值查询

#ifndef EXPLORGDB_GDB_ATTRIBUTE_INDEX_H
#define EXPLORGDB_GDB_ATTRIBUTE_INDEX_H

#include "explorgdb_types.h"
#include <string>
#include <vector>
#include <cstdint>

namespace explorgdb {

// 比较操作符
enum class AttrOp { Eq, Lt, Le, Gt, Ge, Ne };

class GdbAttributeIndexParser {
public:
    static constexpr size_t kPageSize = 4096;
    static constexpr size_t kTrailerSize = 22;

    explicit GdbAttributeIndexParser(const std::string& file_path);

    // 解析整个文件
    bool parse();

    const BPlusTreeTrailer& trailer() const { return trailer_; }
    const std::vector<AttributeIndexEntry>& all_entries() const { return all_entries_; }

    // 数值查询
    std::vector<uint32_t> query_double(double value, AttrOp op) const;

    // 字符串查询
    std::vector<uint32_t> query_string(const std::string& value, AttrOp op) const;

private:
    bool parse_trailer();
    void traverse_tree(uint32_t page_id, int depth_remaining);

    struct PageInfo {
        uint32_t next_page_id;
        uint32_t entry_count;
    };
    PageInfo parse_page_info(size_t page_offset) const;

    void parse_leaf_page(size_t page_offset, std::vector<AttributeIndexEntry>& out);
    std::vector<uint32_t> parse_nonleaf_page(size_t page_offset);

    // 解码属性值
    AttributeIndexEntry decode_value(const uint8_t* bytes, uint8_t value_size,
                                     bool is_string, uint32_t fid) const;

    // 比较函数
    int compare_value(const AttributeIndexEntry& entry, double numeric,
                      const std::string& str, bool is_string) const;

    std::string file_path_;
    std::vector<uint8_t> file_data_;
    BPlusTreeTrailer trailer_;
    std::vector<AttributeIndexEntry> all_entries_;
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_ATTRIBUTE_INDEX_H
