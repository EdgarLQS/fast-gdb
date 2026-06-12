// src/edgar/explorgdb/varint.h
// VarInt 编码工具 — 用于将整数编码为变长字节序列
//
// FileGDB 广泛使用两种 VarInt 格式：
//
// 1. 无符号 Varuint（encode_varuint）
//    - 每字节 7 bit 数据，bit 7 = 延续标志（1=还有后续字节）
//    - 编码：0 → [0x00], 127 → [0x7F], 128 → [0x80, 0x01]
//    - 用途：字段描述符中的 String 长度、记录中的变长字段长度等
//
// 2. 有符号 Varint（encode_varint）
//    - 首字节：bit 6 = 符号（0=正, 1=负），bit 7 = 延续标志，低 6 bit = 数据
//    - 后续字节：与无符号相同（7-bit 数据，bit 7 = 延续）
//    - 编码：0 → [0x00], +1 → [0x02], -1 → [0x03]
//    - 用途：目前主要用于测试验证，记录解析中不直接使用
//
// 注意：这些 encode 函数主要用于测试和调试，
// BinaryReader 中已有对应的 decode 实现（read_varuint/read_varint）。

#ifndef EXPLORGDB_VARINT_H
#define EXPLORGDB_VARINT_H

#include <cstdint>
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

} // namespace explorgdb

#endif // EXPLORGDB_VARINT_H
