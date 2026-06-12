// src/edgar/explorgdb/gdb_indexes.cpp
// .gdbindexes 索引元数据解析实现
//
// 解析逻辑:
//
// 1. 读取索引数量（int32）
// 2. 对每个索引:
//    a. 读取名称（int32 长度 + UTF-16 编码）
//    b. 读取魔数三元组 (magic1, magic2, magic3)
//    c. 判断是否为已知魔数元组:
//       - 已知: (2,0), (4,0), (16,65535) → 有额外 magic4 字段
//       - 未知: magic2 兼作列名长度，无 magic4
//    d. 读取列名（int32 长度 + UTF-16 编码）
//    e. 读取 magic5（末尾标记）
//
// 注意: magic2/magic3 的组合决定索引类型和后续结构，
// 但具体含义尚未完全解析。当前解析器能安全读取所有已知和未知类型，
// 不崩溃、不越界。

#include "gdb_indexes.h"
#include "binary_reader.h"
#include <fstream>
#include <iostream>

namespace explorgdb {

GdbIndexesParser::GdbIndexesParser(const std::string& file_path)
    : file_path_(file_path) {}

// ── 主解析入口 ──
bool GdbIndexesParser::parse() {
    std::ifstream ifs(file_path_, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) return false;

    auto file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    file_data_.resize(file_size);
    ifs.read(reinterpret_cast<char*>(file_data_.data()), file_size);

    BinaryReader br(file_data_);
    int32_t nindexes = br.read_i32();

    for (int i = 0; i < nindexes; ++i) {
        IndexEntry entry;

        // ── 索引名称 ──
        int32_t name_len = br.read_i32();
        entry.name = br.read_utf16(name_len);

        // ── 魔数三元组 ──
        entry.magic1 = br.read_u16();
        entry.magic2 = br.read_i32();
        entry.magic3 = br.read_u16();

        // ── 判断是否为已知魔数元组 ──
        bool known_magic = false;
        if (entry.magic2 == 2 && entry.magic3 == 0) known_magic = true;
        else if (entry.magic2 == 4 && entry.magic3 == 0) known_magic = true;
        else if (entry.magic2 == 16 && entry.magic3 == 65535) known_magic = true;

        // 已知魔数时有额外 magic4 字段
        if (known_magic) {
            entry.magic4 = br.read_i32();
        }

        // ── 列名 ──
        // 已知魔数: 需要额外读取 col_name_len (int32)
        // 未知魔数: magic2 兼作 col_name_len
        int32_t col_name_len;
        if (known_magic) {
            col_name_len = br.read_i32();
        } else {
            col_name_len = entry.magic2;  // 未知魔数时 magic2 = 列名长度
        }
        entry.column_name = br.read_utf16(col_name_len);

        // ── 末尾标记 ──
        entry.magic5 = br.read_u16();

        entries_.push_back(entry);
    }

    return true;
}

} // namespace explorgdb
