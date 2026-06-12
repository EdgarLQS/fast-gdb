// src/edgar/explorgdb/gdb_tablx.cpp
// .gdbtablx 偏移索引解析实现
//
// 偏移编码方式（3 种宽度）：
//
// 4 字节偏移:
//   标准小端 uint32，最大支持 4GB 文件
//   byte[0] | (byte[1]<<8) | (byte[2]<<16) | (byte[3]<<24)
//
// 5 字节偏移:
//   byte[0] = 最低 8 位
//   bytes[1:5] = 高 32 位（小端）
//   最终值 = v_low | (v_high << 8)
//   最大支持 ~256TB 文件
//
// 6 字节偏移:
//   byte[0] = 最低 8 位
//   bytes[1:5] = 中间 32 位（小端）
//   byte[5] = 最高 8 位
//   最终值 = v_low | (v_mid << 8) | (v_high << 40)
//   最大支持 ~16EB 文件
//
// 稀疏块位图（仅 v3）:
//   每 1024 个要素为一个块
//   1 bit/块，0 表示整个块无有效要素
//   位图字节数 = (n_bits_for_block_map + 7) / 8（向上取整）

#include "gdb_tablx.h"
#include "binary_reader.h"
#include <fstream>
#include <iostream>

namespace explorgdb {

GdbTablxParser::GdbTablxParser(const std::string& file_path)
    : file_path_(file_path) {}

// ── 主解析入口 ──
bool GdbTablxParser::parse() {
    std::ifstream ifs(file_path_, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) return false;

    auto file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    file_data_.resize(file_size);
    ifs.read(reinterpret_cast<char*>(file_data_.data()), file_size);

    BinaryReader br(file_data_);

    // ── 读取文件头部 ──
    hdr_.version = br.read_u32();

    if (hdr_.version == 3) {
        // v3 头部: 16 字节（无额外 padding）
        hdr_.n1024blocks_v3 = br.read_u32();
        hdr_.nfeatures_v3 = br.read_u32();
        hdr_.size_tablx_offsets = br.read_u32();
    } else if (hdr_.version == 4) {
        // v4 头部: 24 字节（含 padding）
        hdr_.unknown_v4 = br.read_u32();
        hdr_.size_tablx_offsets = br.read_u32();
        br.skip(8);  // 8 字节 padding
    } else {
        std::cerr << "Unknown .gdbtablx version: " << hdr_.version << "\n";
        return false;
    }

    // ── 读取偏移表 ──
    // 条目总数 = n1024blocks × 1024
    // 每个条目宽度由 size_tablx_offsets 决定（4/5/6 字节）
    size_t n_blocks = hdr_.n1024blocks_v3;
    size_t n_entries = n_blocks * 1024;
    int entry_size = static_cast<int>(hdr_.size_tablx_offsets);
    const uint8_t* offset_data = file_data_.data() + br.tell();

    offsets_.resize(n_entries);
    for (size_t i = 0; i < n_entries; ++i) {
        offsets_[i] = read_offset(offset_data + i * entry_size, entry_size);
        // 记录非零偏移的 FID（表示有效要素）
        if (offsets_[i] != 0) {
            valid_offsets_.push_back(static_cast<uint32_t>(i));
        }
    }

    br.seek(br.tell() + n_entries * entry_size);

    // ── 读取稀疏块位图（v3）或 v4 尾部 ──
    if (hdr_.version == 3) {
        // 稀疏位图元数据
        if (br.can_read(4)) {
            uint32_t n_bitmap_int32 = br.read_u32();
            uint32_t n_bits_for_block_map = br.read_u32();
            uint32_t n1024blocks_bis = br.read_u32();
            uint32_t n_leading_nonzero = br.read_u32();
            (void)n_bitmap_int32; (void)n_bits_for_block_map;
            (void)n1024blocks_bis; (void)n_leading_nonzero;

            // 读取位图: 每 bit 对应一个块
            // 字节数向上取整: (n_bits + 7) / 8
            size_t bitmap_bytes = (n_bits_for_block_map + 7) / 8;
            if (bitmap_bytes > 0 && br.can_read(bitmap_bytes)) {
                block_bitmap_.resize(n_bits_for_block_map);
                for (size_t i = 0; i < bitmap_bytes; ++i) {
                    uint8_t byte = br.read_u8();
                    for (int bit = 0; bit < 8; ++bit) {
                        size_t idx = i * 8 + bit;
                        if (idx < n_bits_for_block_map) {
                            block_bitmap_[idx] = (byte >> bit) & 1;
                        }
                    }
                }
            }
        }
    } else if (hdr_.version == 4) {
        // v4 尾部
        hdr_.nfeatures_v4 = br.read_u64();
        hdr_.sizeof_varying_section = br.read_u32();
    }

    return true;
}

// ── 变长偏移读取 ──
// 根据 size_bytes（4/5/6）以不同方式组装小端整数
uint64_t GdbTablxParser::read_offset(const uint8_t* data, int size_bytes) const {
    switch (size_bytes) {
        case 4: {
            // 标准 4 字节小端 uint32
            uint32_t v = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
            return v;
        }
        case 5: {
            // 5 字节编码: byte[0]=低 8 位, bytes[1:5]=高 32 位
            uint64_t v_low = data[0];
            uint32_t v_high = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
            return v_low | (static_cast<uint64_t>(v_high) << 8);
        }
        case 6: {
            // 6 字节编码: byte[0]=低 8 位, bytes[1:5]=中 32 位, byte[5]=高 8 位
            uint64_t v_low = data[0];
            uint32_t v_mid = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
            uint64_t v_high = data[5];
            return v_low | (v_mid << 8) | (v_high << 40);
        }
        default:
            return 0;  // 未知宽度返回 0
    }
}

// ── FID → 偏移查找 ──
uint64_t GdbTablxParser::get_offset(uint32_t fid) const {
    if (fid < offsets_.size()) return offsets_[fid];
    return 0;  // FID 超出范围，视为不存在
}

// ── 块活跃性检查 ──
bool GdbTablxParser::is_block_active(uint32_t block_index) const {
    if (block_bitmap_.empty()) return true;  // 无位图 → 所有块默认活跃
    if (block_index < block_bitmap_.size()) return block_bitmap_[block_index];
    return false;  // 超出位图范围 → 视为非活跃
}

} // namespace explorgdb
