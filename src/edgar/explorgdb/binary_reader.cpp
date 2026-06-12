// src/edgar/explorgdb/binary_reader.cpp
// BinaryReader 实现 — 所有方法均为小端读取，带边界检查
//
// 实现要点：
//   - 所有多字节读取通过逐字节移位组装，不依赖平台字节序
//   - 浮点读取用 std::memcpy 避免 strict aliasing 违规
//   - ensure() 在每个读取方法开头调用，越界立即抛异常

#include "binary_reader.h"
#include "utf16.h"
#include <cstring>

namespace explorgdb {

// ── 单字节读取 ──
uint8_t BinaryReader::read_u8() {
    ensure(1);
    return data_[pos_++];
}

// ── 双字节无符号（小端） ──
// 例如：[0x01, 0x02] → 0x0201
uint16_t BinaryReader::read_u16() {
    ensure(2);
    uint16_t v = static_cast<uint16_t>(data_[pos_]) |
                 (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
}

// ── 四字节无符号（小端） ──
// 例如：[0x01, 0x02, 0x03, 0x04] → 0x04030201
uint32_t BinaryReader::read_u32() {
    ensure(4);
    uint32_t v = static_cast<uint32_t>(data_[pos_]) |
                 (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                 (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                 (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
    pos_ += 4;
    return v;
}

// ── 八字节无符号（小端） ──
// 例如：[0x01, ..., 0x08] → 0x0807060504030201
uint64_t BinaryReader::read_u64() {
    ensure(8);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(data_[pos_ + i]) << (i * 8);
    pos_ += 8;
    return v;
}

// ── 五字节无符号（小端） ──
// 用于 .gdbtablx 中等大小表的偏移条目
// byte[0] = 最低 8 位, bytes[1:5] = 高 32 位
uint40_t BinaryReader::read_u40() {
    ensure(5);
    uint64_t v = 0;
    for (int i = 0; i < 5; ++i)
        v |= static_cast<uint64_t>(data_[pos_ + i]) << (i * 8);
    pos_ += 5;
    return {v};
}

// ── 六字节无符号（小端） ──
// 用于 .gdbtablx 超大表的偏移条目
uint48_t BinaryReader::read_u48() {
    ensure(6);
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i)
        v |= static_cast<uint64_t>(data_[pos_ + i]) << (i * 8);
    pos_ += 6;
    return {v};
}

// ── 有符号整数（位模式复用无符号读取结果） ──
int16_t BinaryReader::read_i16() {
    uint16_t u = read_u16();
    return static_cast<int16_t>(u);
}

int32_t BinaryReader::read_i32() {
    uint32_t u = read_u32();
    return static_cast<int32_t>(u);
}

int64_t BinaryReader::read_i64() {
    uint64_t u = read_u64();
    return static_cast<int64_t>(u);
}

// ── 浮点数（memcpy 避免 strict aliasing） ──
float BinaryReader::read_f32() {
    ensure(4);
    float v;
    std::memcpy(&v, data_ + pos_, 4);
    pos_ += 4;
    return v;
}

double BinaryReader::read_f64() {
    ensure(8);
    double v;
    std::memcpy(&v, data_ + pos_, 8);
    pos_ += 8;
    return v;
}

// ── UTF-16LE 字符串 ──
std::string BinaryReader::read_utf16(int char_count) {
    ensure(char_count * 2);
    std::string result = utf16le_to_utf8(data_ + pos_, char_count);
    pos_ += char_count * 2;  // 每字符 2 字节
    return result;
}

// ── 无符号 VarInt 解码 ──
// 每字节提供 7 bit 数据，bit 7 = 1 表示还有后续字节
// 最多可编码 10 字节（64-bit 值需要 10 个 7-bit 组）
uint64_t BinaryReader::read_varuint() {
    uint64_t ret = 0;
    int shift = 0;
    uint8_t byte;
    do {
        ensure(1);
        byte = data_[pos_++];
        ret |= static_cast<uint64_t>(byte & 0x7F) << shift;
        shift += 7;
    } while (byte & 0x80);  // bit 7 = continuation
    return ret;
}

// ── 有符号 VarInt 解码 ──
// 首字节：bit 6 = 符号（0=正, 1=负），bit 7 = 延续标志，低 6 bit = 数据
// 后续字节：与无符号 VarInt 相同（7-bit 数据，bit 7 = 延续）
int64_t BinaryReader::read_varint() {
    ensure(1);
    uint8_t b = data_[pos_++];
    int64_t sign = (b & 0x40) ? -1 : 1;  // bit 6 = sign
    int64_t ret = b & 0x3F;               // 低 6 bit = 数据
    int shift = 6;
    while (b & 0x80) {                    // continuation bit
        ensure(1);
        b = data_[pos_++];
        ret |= static_cast<int64_t>(b & 0x7F) << shift;
        shift += 7;
    }
    return sign * ret;
}

// ── 原始字节块读取 ──
std::vector<uint8_t> BinaryReader::read_bytes(size_t n) {
    ensure(n);
    std::vector<uint8_t> result(data_ + pos_, data_ + pos_ + n);
    pos_ += n;
    return result;
}

} // namespace explorgdb
