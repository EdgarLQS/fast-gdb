// src/edgar/explorgdb/writer/tablx_writer.h
// .gdbtablx 偏移表写入器 — 管理 FID → .gdbtable 文件偏移的映射
//
// .gdbtablx 文件结构（v4）：
//   [header: 20 字节]
//     version(4) = 4
//     unknown(4) = 0
//     size_tablx_offsets(4) = 4（或 5/6）
//     padding(8) = 0
//   [offset entries: N × size_tablx_offsets 字节]
//     每个条目是 .gdbtable 中对应 FID 行的文件偏移
//     偏移值 0 = 该 FID 不存在
//   [trailer: 12 字节]
//     nfeatures_v4(8) = 有效要素数
//     sizeof_varying_section(4) = 0
//
// 使用方式：
//   TablxWriter writer;
//   writer.set_offset(0, 1024);   // FID 0 → offset 1024
//   writer.set_offset(1, 2048);   // FID 1 → offset 2048
//   writer.write("/path/to/a00000001.gdbtablx");

#ifndef EXPLORGDB_TABLX_WRITER_H
#define EXPLORGDB_TABLX_WRITER_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

namespace explorgdb {
namespace writer {

class TablxWriter {
public:
    TablxWriter() = default;

    // 添加一个偏移条目（FID 从 0 开始连续递增）
    void add_offset(uint64_t offset) {
        offsets_.push_back(offset);
    }

    // 清除所有偏移
    void clear() { offsets_.clear(); }

    // 条目数
    size_t count() const { return offsets_.size(); }

    // 写入 .gdbtablx 文件（v4 格式）
    bool write(const std::string& path) const {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;

        uint32_t n_features = static_cast<uint32_t>(offsets_.size());
        uint32_t n_blocks = (n_features + 1023) / 1024;
        if (n_blocks == 0) n_blocks = 1;  // 至少 1 个块
        uint32_t n_entries = n_blocks * 1024;
        uint32_t entry_size = 4;  // 4 字节偏移（支持最大 4GB 文件）

        // ── 写头部（20 字节）──
        write_u32(ofs, 4);            // version = 4
        write_u32(ofs, 0);            // unknown = 0
        write_u32(ofs, entry_size);   // size_tablx_offsets = 4
        write_u32(ofs, 0);            // padding (4 bytes)
        write_u32(ofs, 0);            // padding (4 bytes)

        // ── 写偏移表 ──
        // 有效条目
        for (uint32_t i = 0; i < n_features; ++i) {
            write_u32(ofs, static_cast<uint32_t>(offsets_[i]));
        }
        // 填充到块边界（用 0 填充 = 不存在的 FID）
        for (uint32_t i = n_features; i < n_entries; ++i) {
            write_u32(ofs, 0);
        }

        // ── 写 trailer ──
        write_u64(ofs, n_features);           // nfeatures_v4
        write_u32(ofs, 0);                    // sizeof_varying_section = 0

        return true;
    }

private:
    static void write_u32(std::ofstream& ofs, uint32_t value) {
        uint8_t buf[4];
        buf[0] = static_cast<uint8_t>(value & 0xFF);
        buf[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        buf[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
        buf[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        ofs.write(reinterpret_cast<const char*>(buf), 4);
    }

    static void write_u64(std::ofstream& ofs, uint64_t value) {
        uint8_t buf[8];
        for (int i = 0; i < 8; ++i) {
            buf[i] = static_cast<uint8_t>(value & 0xFF);
            value >>= 8;
        }
        ofs.write(reinterpret_cast<const char*>(buf), 8);
    }

    std::vector<uint64_t> offsets_;  // FID → .gdbtable 文件偏移
};

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_TABLX_WRITER_H
