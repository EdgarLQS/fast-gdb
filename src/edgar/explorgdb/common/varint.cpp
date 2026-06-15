// src/edgar/explorgdb/varint.cpp
// VarInt 编码实现
//
// 编码算法（参考 Google Protocol Buffers Varint 设计）：
//
// 无符号 Varuint:
//   将 64-bit 整数拆分为 7-bit 组，从最低位开始编码。
//   除最后一个字节外，每个字节设置 bit 7 = 1（延续标志）。
//   字节流中每个字节的数据位依次为: bits 0-6, bits 7-13, bits 14-20, ...
//
// 有符号 Varint:
//   首字节需要编码符号位（bit 6），所以首字节只有 6-bit 数据。
//   后续字节与无符号相同（7-bit 数据，bit 7 = 延续）。
//   符号位放在首字节的 bit 6: 0=正数, 1=负数。
//   负数的绝对值编码在数据位中，解码时乘以符号。

#include "varint.h"

namespace explorgdb {

// ── 无符号 Varuint 编码 ──
// 算法: 每次取低 7 位，如果还有剩余数据则设置延续标志
std::vector<uint8_t> encode_varuint(uint64_t value) {
    std::vector<uint8_t> out;
    do {
        uint8_t byte = value & 0x7F;    // 取低 7 位数据
        value >>= 7;                     // 右移 7 位
        if (value != 0) byte |= 0x80;   // 还有后续字节，设置 continuation bit
        out.push_back(byte);
    } while (value != 0);
    return out;
}

// ── 有符号 Varint 编码 ──
// 算法: 先提取符号位（bit 6），首字节用 6-bit 数据，后续用 7-bit
std::vector<uint8_t> encode_varint(int64_t value) {
    // 分离符号和绝对值
    int64_t sign = value < 0 ? -1 : 1;
    value *= sign;  // 转为非负数（绝对值）

    // 首字节: 低 6 bit = 数据, bit 6 = 符号, bit 7 = 延续（视情况）
    uint8_t first = (value & 0x3F);     // 取低 6 位
    if (sign < 0) first |= 0x40;        // 负数设置符号位
    value >>= 6;                         // 右移 6 位（已处理）

    std::vector<uint8_t> out;
    if (value == 0) {
        // 6-bit 足够表示，不需要延续
        out.push_back(first);
        return out;
    }

    // 需要更多字节，在首字节设置延续标志
    first |= 0x80;
    out.push_back(first);

    // 后续字节: 每字节 7-bit 数据 + 延续标志
    while (value != 0) {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value != 0) byte |= 0x80;   // 还有后续，设置延续
        out.push_back(byte);
    }
    return out;
}

} // namespace explorgdb
