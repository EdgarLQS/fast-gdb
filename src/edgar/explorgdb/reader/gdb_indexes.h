// src/edgar/explorgdb/gdb_indexes.h
// .gdbindexes 索引元数据解析器 — 读取表中定义的索引信息
//
// .gdbindexes 文件包含该表上所有属性索引的描述信息。
// 每个索引条目包含：名称、魔数元组、列名。
//
// 文件格式：
//   [0:4]   nindexes (int32) — 索引数量
//   per index:
//     name_len (int32) + name (UTF-16, name_len 字符)
//     magic1 (uint16)
//     magic2 (int32)
//     magic3 (uint16)
//     [if known magic] magic4 (int32)
//     col_name_len (int32) + col_name (UTF-16)
//     magic5 (uint16)
//
// 已知魔数元组（决定索引类型）:
//   (magic2=2, magic3=0)     — 常见索引
//   (magic2=4, magic3=0)     — 另一种索引
//   (magic2=16, magic3=65535) — 第三种索引
//
// 使用方式:
//   GdbIndexesParser parser("a00000001.gdbindexes");
//   parser.parse();
//   for (const auto& e : parser.entries()) { ... }

#ifndef EXPLORGDB_GDB_INDEXES_H
#define EXPLORGDB_GDB_INDEXES_H

#include "explorgdb_types.h"
#include <string>
#include <vector>

namespace explorgdb {

class GdbIndexesParser {
public:
    // 构造时指定 .gdbindexes 文件路径
    explicit GdbIndexesParser(const std::string& file_path);

    // 解析整个文件
    bool parse();

    // ── 访问器 ──

    const std::vector<IndexEntry>& entries() const { return entries_; }
    size_t index_count() const { return entries_.size(); }

private:
    std::string file_path_;
    std::vector<uint8_t> file_data_;  // 整个文件内容
    std::vector<IndexEntry> entries_; // 解析后的索引条目
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_INDEXES_H
