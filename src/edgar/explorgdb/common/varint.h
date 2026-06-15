// src/edgar/explorgdb/common/varint.h
// VarInt 编解码统一工具 — 读写共享
//
// 提供两类编码接口：
//   1. encode_varuint/encode_varint → 返回 std::vector<uint8_t>（测试/debug 用）
//   2. encode_varuint_to/encode_varint_to → 直接写入 uint8_t*（零分配，热路径用）
//   3. write_u16_le/write_u32_le/write_f64_le → 小端写入辅助
//
// FileGDB 两种 VarInt 格式：
//
// 1. 无符号 Varuint
//    - 每字节 7 bit 数据，bit 7 = 延续标志（1=还有后续字节）
//    - 编码：0 → [0x00], 127 → [0x7F], 128 → [0x80, 0x01]
//
// 2. 有符号 Varint
//    - 首字节：bit 6 = 符号（0=正, 1=负），bit 7 = 延续标志，低 6 bit = 数据
//    - 后续字节：与无符号相同（7-bit 数据，bit 7 = 延续）
//    - 编码：0 → [0x00], +1 → [0x02], -1 → [0x03]
//
// BinaryReader 中已有对应的 decode 实现（read_varuint/read_varint）。

#ifndef EXPLORGDB_VARINT_H
#define EXPLORGDB_VARINT_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace explorgdb {

// 无符号 Varuint 编码
// 输入: value — 要编码的无符号 64 位整数
// 返回: 编码后的字节序列（1~10 字节）
std::vector<uint8_t> encode_varuint(uint64_t value);

// 有符号 Varint 编码
// 输入: value — 要编码的有符号 64 位整数
// 返回: 编码后的字节序列（1~10 字节）
std::vector<uint8_t> encode_varint(int64_t value);

// ── 零分配编码函数（直接写入 uint8_t* 缓冲区）──
// 用于性能热路径，避免 std::vector 堆分配。
// 所有函数返回写入的字节数。调用者需确保 dst 有足够空间（最大 10 字节）。

static constexpr size_t kMaxVarintLen = 10;

// 将无符号整数编码写入 dst，返回写入字节数
inline size_t encode_varuint_to(uint8_t* dst, uint64_t value) {
    size_t n = 0;
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value != 0) byte |= 0x80;
        dst[n++] = byte;
    } while (value != 0);
    return n;
}

// 将无符号整数编码写入 dst+offset，返回写入字节数
inline size_t encode_varuint_at(uint8_t* dst, size_t offset, uint64_t value) {
    return encode_varuint_to(dst + offset, value);
}

// 计算 varuint 编码后的字节长度（不实际编码）
inline size_t varuint_encoded_len(uint64_t value) {
    size_t n = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++n;
    }
    return n;
}

// 将有符号整数编码写入 dst，返回写入字节数
// 编码规则：首字节 bit6=符号，bit7=延续，低6bit=数据
inline size_t encode_varint_to(uint8_t* dst, int64_t value) {
    uint64_t sign_bit = 0;
    uint64_t abs_val;
    if (value < 0) {
        sign_bit = 0x40;
        abs_val = static_cast<uint64_t>(-value);
    } else {
        abs_val = static_cast<uint64_t>(value);
    }

    uint64_t first_data = abs_val & 0x3F;
    abs_val >>= 6;

    size_t n = 0;
    if (abs_val == 0) {
        dst[n++] = static_cast<uint8_t>(first_data | sign_bit);
        return n;
    }

    dst[n++] = static_cast<uint8_t>(first_data | sign_bit | 0x80);

    while (abs_val != 0) {
        uint8_t byte = static_cast<uint8_t>(abs_val & 0x7F);
        abs_val >>= 7;
        if (abs_val != 0) byte |= 0x80;
        dst[n++] = byte;
    }
    return n;
}

// 将小端 16-bit 整数写入 dst
inline void write_u16_le(uint8_t* dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFF);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

// 将小端 32-bit 整数写入 dst
inline void write_u32_le(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFF);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

// 将小端 64-bit 浮点（double）写入 dst
inline void write_f64_le(uint8_t* dst, double value) {
    uint64_t bits;
    static_assert(sizeof(double) == sizeof(uint64_t), "double must be 64 bits");
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<uint8_t>(bits & 0xFF);
        bits >>= 8;
    }
}

} // namespace explorgdb

#endif // EXPLORGDB_VARINT_H
