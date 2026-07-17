// src/edgar/explorgdb/gdb_indexes.h
// .gdbindexes 索引元数据解析器 — 读取表中定义的索引信息
//
// .gdbindexes 文件包含该表上所有属性索引的描述信息。
// 每个索引条目包含：名称、魔数元组、索引表达式。
// 常见表达式是裸字段名，也可能是 LOWER(field) 等函数表达式。
//
// 文件格式：
//   [0:4]   nindexes (int32) — 索引数量
//   per index:
//     name_len (int32) + name (UTF-16, name_len 字符)
//     magic1 (uint16)
//     magic2 (int32)
//     magic3 (uint16)
//     [if known magic] magic4 (int32)
//     expression_len (int32) + expression (UTF-16)
//     magic5 (uint16)
//
// 已知魔数元组（决定索引类型）:
//   (magic2=2, magic3=0)      — 常见索引
//   (magic2=4, magic3=0)      — 另一种索引
//   (magic2=16, magic3=65535) — 第三种索引

#ifndef EXPLORGDB_GDB_INDEXES_H
#define EXPLORGDB_GDB_INDEXES_H

#include "explorgdb_types.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace explorgdb {

class GdbIndexesParser {
public:
    explicit GdbIndexesParser(const std::string& file_path);

    bool parse();

    const std::vector<IndexEntry>& entries() const { return entries_; }
    size_t index_count() const { return entries_.size(); }

    // Mirrors OpenFileGDB's conservative field extraction. LOWER(field) is
    // associated with field for metadata lookup, but is not a direct-field
    // expression and must not be used as a case-sensitive fast path.
    static std::string field_name_from_expression(
        const std::string& expression);
    static bool is_direct_field_expression(
        const std::string& expression);

private:
    std::string file_path_;
    std::vector<uint8_t> file_data_;
    std::vector<IndexEntry> entries_;
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_INDEXES_H