// src/edgar/explorgdb/writer/gdb_indexes_writer.h
// .gdbindexes 元数据写入器 — 记录所有索引的元信息
//
// .gdbindexes 文件结构：
//   nindexes (int32) — 索引数量
//   per index:
//     name_len (int32) + name (UTF-16, name_len 字符)
//     magic1 (uint16) = 0
//     magic2 (int32) — 2=普通属性, 4=几何, 16=ObjectId
//     magic3 (uint16) — 0 或 65535
//     magic4 (int32) = 1
//     col_name_len (int32) + col_name (UTF-16)
//     magic5 (uint16) = 0
//
// 使用方式：
//   GdbIndexesWriter writer;
//   writer.add_index({"idx_shape", "SHAPE", 4, 0});      // 空间索引
//   writer.add_index({"idx_name", "NAME", 2, 0});        // 属性索引
//   writer.write("a00000001.gdbindexes");

#ifndef EXPLORGDB_GDB_INDEXES_WRITER_H
#define EXPLORGDB_GDB_INDEXES_WRITER_H

#include "../common/utf16.h"
#include <cstdint>
#include <string>
#include <vector>
#include <cstdio>

namespace explorgdb {
namespace writer {

struct IndexEntry {
    std::string name;           // 索引名（UTF-8）
    std::string field_name;     // 字段名（UTF-8）
    uint32_t magic2 = 2;        // 2=普通属性, 4=几何, 16=ObjectId
    uint16_t magic3 = 0;        // 0 或 65535
};

class GdbIndexesWriter {
public:
    GdbIndexesWriter() = default;

    void add_index(const IndexEntry& entry) {
        indexes_.push_back(entry);
    }

    void clear() { indexes_.clear(); }

    bool write(const std::string& path) const {
        FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp) return false;

        // nindexes
        uint32_t n = static_cast<uint32_t>(indexes_.size());
        write_u32(fp, n);

        for (const auto& idx : indexes_) {
            // name (UTF-16)
            write_utf16_string(fp, idx.name);

            // magic1 = 0
            write_u16(fp, 0);

            // magic2, magic3
            write_u32(fp, idx.magic2);
            write_u16(fp, idx.magic3);

            // magic4 = 1
            write_u32(fp, 1);

            // field_name (UTF-16)
            write_utf16_string(fp, idx.field_name);

            // magic5 = 0
            write_u16(fp, 0);
        }

        std::fclose(fp);
        return true;
    }

    size_t size() const { return indexes_.size(); }
    bool empty() const { return indexes_.empty(); }

private:
    void write_u16(FILE* fp, uint16_t v) const {
        uint8_t buf[2];
        buf[0] = static_cast<uint8_t>(v & 0xFF);
        buf[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        std::fwrite(buf, 1, 2, fp);
    }

    void write_u32(FILE* fp, uint32_t v) const {
        uint8_t buf[4];
        buf[0] = static_cast<uint8_t>(v & 0xFF);
        buf[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        buf[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        buf[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
        std::fwrite(buf, 1, 4, fp);
    }

    void write_utf16_string(FILE* fp, const std::string& utf8) const {
        std::u16string utf16 = explorgdb::utf8_to_utf16(utf8);
        uint32_t len = static_cast<uint32_t>(utf16.size());
        write_u32(fp, len);
        for (char16_t ch : utf16) {
            write_u16(fp, static_cast<uint16_t>(ch));
        }
    }

    std::vector<IndexEntry> indexes_;
};

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_GDB_INDEXES_WRITER_H
