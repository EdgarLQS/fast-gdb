// src/edgar/explorgdb/writer/varint_encoder.h
// VarInt 写入端 — 将整数编码为 FileGDB 变长字节序列
//
// 与 varint.h 的区别：
//   - varint.h 返回 std::vector<uint8_t>（适合测试/debug，有堆分配）
//   - varint_encoder.h 直接写入 uint8_t* 缓冲区（零分配，适合热路径）
//
// FileGDB 使用两种 VarInt：
//
// 1. 无符号 Varuint（encode_varuint_to）
//    每字节 7-bit 数据，bit 7 = 延续标志（1=还有后续字节）
//    0 → [0x00], 127 → [0x7F], 128 → [0x80, 0x01]
//
// 2. 有符号 Varint（encode_varint_to）
//    首字节：bit 6 = 符号（0=正, 1=负），bit 7 = 延续，低 6-bit = 数据
//    后续字节：与无符号相同（7-bit 数据，bit 7 = 延续）
//    0 → [0x00], +1 → [0x01], -1 → [0x41]
//
// 所有函数返回写入的字节数。调用者需确保 dst 有足够空间（最大 10 字节）。

#ifndef EXPLORGDB_VARINT_ENCODER_H
#define EXPLORGDB_VARINT_ENCODER_H

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace explorgdb {
namespace writer {

// 最大 varint 编码长度（64-bit 值最多 10 字节）
static constexpr size_t kMaxVarintLen = 10;

// 将无符号整数编码写入 dst，返回写入字节数
// dst 至少需要 kMaxVarintLen 字节空间
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

// 将无符号整数编码写入 dst（偏移 offset 处），返回写入字节数
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
    // 分离符号和绝对值
    uint64_t sign_bit = 0;
    uint64_t abs_val;
    if (value < 0) {
        sign_bit = 0x40;  // bit 6 = 1 表示负数
        abs_val = static_cast<uint64_t>(-(value + 1));  // 避免 INT64_MIN 溢出
        // 注意：FileGDB varint 对负数的编码是 sign * abs_val
        // 这里我们直接编码绝对值，符号放在 bit 6
        abs_val = static_cast<uint64_t>(-value);
    } else {
        abs_val = static_cast<uint64_t>(value);
    }

    // 首字节：低 6-bit 数据 + bit 6 符号 + bit 7 延续
    uint64_t first_data = abs_val & 0x3F;
    abs_val >>= 6;

    size_t n = 0;
    if (abs_val == 0) {
        dst[n++] = static_cast<uint8_t>(first_data | sign_bit);
        return n;
    }

    // 需要延续
    dst[n++] = static_cast<uint8_t>(first_data | sign_bit | 0x80);

    // 后续字节：每字节 7-bit 数据 + 延续标志
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
    dst[0] = static_cast<uint8_t>(bits & 0xFF);
    dst[1] = static_cast<uint8_t>((bits >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((bits >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((bits >> 24) & 0xFF);
    dst[4] = static_cast<uint8_t>((bits >> 32) & 0xFF);
    dst[5] = static_cast<uint8_t>((bits >> 40) & 0xFF);
    dst[6] = static_cast<uint8_t>((bits >> 48) & 0xFF);
    dst[7] = static_cast<uint8_t>((bits >> 56) & 0xFF);
}

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_VARINT_ENCODER_H
