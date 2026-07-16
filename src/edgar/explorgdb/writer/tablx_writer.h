// src/edgar/explorgdb/writer/tablx_writer.h
// .gdbtablx 偏移表写入器 — 管理 FID → .gdbtable 文件偏移的映射
//
// .gdbtablx 文件结构（v3 — GDAL 默认格式）：
//   [header: 16 字节]
//     version(4) = 3
//     n1024BlocksPresent(4) = ceil(nFeatures / 1024)
//     nTotalRecordCount(4) = 有效要素数
//     entrySize(4) = 5（默认；4=最大4GB，6=最大256TB）
//   [offset entries: n1024BlocksPresent × 1024 × entrySize 字节]
//     每个条目是 .gdbtable 中对应 FID 行的文件偏移（5 字节 LE）
//     偏移值 0 = 该 FID 不存在（已删除或填充）
//   [trailer: 16 字节]
//     nBitmapInt32Words(4) = 0（无 block bitmap）
//     n1024BlocksTotal(4) = ceil(nFeatures / 1024)
//     n1024BlocksPresent(4) = 同 header
//     nLeadingNonZero32BitWords(4) = 0
//
// 使用方式：
//   TablxWriter writer;
//   writer.add_offset(329);   // FID 0 → offset 329
//   writer.add_offset(461);   // FID 1 → offset 461
//   writer.write("/path/to/a00000009.gdbtablx");

#ifndef EXPLORGDB_TABLX_WRITER_H
#define EXPLORGDB_TABLX_WRITER_H

#include <cstdint>
#include <cstddef>
#include <cerrno>
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

    // 写入 .gdbtablx 文件（v3 格式 — GDAL 默认）
    bool write(const std::string& path, std::string* error = nullptr) const {
        if (offsets_.size() > UINT32_MAX) {
            if (error) *error = "record count exceeds uint32";
            return false;
        }
        for (uint64_t offset : offsets_) {
            if (offset >= (1ULL << 40)) {
                if (error) *error = "record offset exceeds 5-byte tablx capacity";
                return false;
            }
        }

        errno = 0;
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            if (error) *error = std::strerror(errno);
            return false;
        }

        uint32_t n_features = static_cast<uint32_t>(offsets_.size());
        uint32_t n_blocks = (n_features + 1023) / 1024;
        if (n_blocks == 0) n_blocks = 1;  // 至少 1 个块
        uint32_t n_entries = n_blocks * 1024;
        uint32_t entry_size = 5;  // 默认 5 字节偏移（GDAL 默认值，支持 ~1TB）

        // ── 写头部（16 字节）──
        write_u32(ofs, 3);            // version = 3
        write_u32(ofs, n_blocks);     // n1024BlocksPresent
        write_u32(ofs, n_features);   // nTotalRecordCount
        write_u32(ofs, entry_size);   // entrySize = 5

        // ── 写偏移表（每条目 5 字节 LE）──
        for (uint32_t i = 0; i < n_features; ++i) {
            write_u40(ofs, offsets_[i]);
        }
        // 填充到块边界（用 0 填充 = 不存在的 FID）
        for (uint32_t i = n_features; i < n_entries; ++i) {
            write_zero_n(ofs, entry_size);
        }

        // ── 写 trailer（16 字节）──
        write_u32(ofs, 0);            // nBitmapInt32Words = 0（无 bitmap）
        write_u32(ofs, n_blocks);     // n1024BlocksTotal
        write_u32(ofs, n_blocks);     // n1024BlocksPresent
        write_u32(ofs, 0);            // nLeadingNonZero32BitWords = 0

        ofs.flush();
        if (!ofs.good()) {
            if (error) *error = std::strerror(errno);
            ofs.close();
            return false;
        }
        ofs.close();
        if (ofs.fail()) {
            if (error) *error = std::strerror(errno);
            return false;
        }
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

    // 写 5 字节 little-endian（tablx v3 默认 entry size）
    static void write_u40(std::ofstream& ofs, uint64_t value) {
        uint8_t buf[5];
        for (int i = 0; i < 5; ++i) {
            buf[i] = static_cast<uint8_t>(value & 0xFF);
            value >>= 8;
        }
        ofs.write(reinterpret_cast<const char*>(buf), 5);
    }

    // 写 N 字节零
    static void write_zero_n(std::ofstream& ofs, uint32_t n) {
        uint8_t buf[8] = {0};
        ofs.write(reinterpret_cast<const char*>(buf), n);
    }

    std::vector<uint64_t> offsets_;  // FID → .gdbtable 文件偏移
};

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_TABLX_WRITER_H
