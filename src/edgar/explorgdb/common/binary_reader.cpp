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
#include <limits>

namespace explorgdb {

// ── 单字节读取 ──
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
uint8_t BinaryReader::read_u8() {
    ensure(1);
    return data_[pos_++];
}

// ── 双字节无符号（小端） ──
// 例如：[0x01, 0x02] → 0x0201
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
uint16_t BinaryReader::read_u16() {
    ensure(2);
    uint16_t v = static_cast<uint16_t>(data_[pos_]) |
                 (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
}

// ── 四字节无符号（小端） ──
// 例如：[0x01, 0x02, 0x03, 0x04] → 0x04030201
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
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
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
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
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
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
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
uint48_t BinaryReader::read_u48() {
    ensure(6);
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i)
        v |= static_cast<uint64_t>(data_[pos_ + i]) << (i * 8);
    pos_ += 6;
    return {v};
}

// ── 有符号整数（位模式复用无符号读取结果） ──
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
int16_t BinaryReader::read_i16() {
    uint16_t u = read_u16();
    return static_cast<int16_t>(u);
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
int32_t BinaryReader::read_i32() {
    uint32_t u = read_u32();
    return static_cast<int32_t>(u);
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
int64_t BinaryReader::read_i64() {
    uint64_t u = read_u64();
    return static_cast<int64_t>(u);
}

// ── 浮点数（memcpy 避免 strict aliasing） ──
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
float BinaryReader::read_f32() {
    ensure(4);
    float v;
    std::memcpy(&v, data_ + pos_, 4);
    pos_ += 4;
    return v;
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
double BinaryReader::read_f64() {
    ensure(8);
    double v;
    std::memcpy(&v, data_ + pos_, 8);
    pos_ += 8;
    return v;
}

// ── UTF-16LE 字符串 ──
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
std::string BinaryReader::read_utf16(int char_count) {
    ensure(char_count * 2);
    std::string result = utf16le_to_utf8(data_ + pos_, char_count);
    pos_ += char_count * 2;  // 每字符 2 字节
    return result;
}

// ── 无符号 VarInt 解码 ──
// 每字节提供 7 bit 数据，bit 7 = 1 表示还有后续字节
// 最多可编码 10 字节（64-bit 值需要 10 个 7-bit 组）
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
uint64_t BinaryReader::read_varuint() {
    uint64_t ret = 0;
    for (unsigned byte_index = 0; byte_index < 10; ++byte_index) {
        ensure(1);
        const uint8_t byte = data_[pos_++];
        const uint64_t payload = byte & 0x7fU;
        if (byte_index == 9 &&
            ((byte & 0x80U) != 0 || payload > 1U)) {
            throw std::out_of_range("BinaryReader: invalid varuint");
        }
        ret |= payload << (byte_index * 7U);
        if ((byte & 0x80U) == 0) return ret;
    }
    throw std::out_of_range("BinaryReader: invalid varuint");
}

// ── 有符号 VarInt 解码 ──
// 首字节：bit 6 = 符号（0=正, 1=负），bit 7 = 延续标志，低 6 bit = 数据
// 后续字节：与无符号 VarInt 相同（7-bit 数据，bit 7 = 延续）
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
int64_t BinaryReader::read_varint() {
    ensure(1);
    uint8_t byte = data_[pos_++];
    const bool negative = (byte & 0x40U) != 0;
    uint64_t magnitude = byte & 0x3fU;
    const uint64_t max_magnitude = negative
        ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1U
        : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

    for (unsigned byte_index = 1; byte & 0x80U; ++byte_index) {
        if (byte_index >= 10)
            throw std::out_of_range("BinaryReader: invalid varint");
        ensure(1);
        byte = data_[pos_++];
        const unsigned shift = 6U + (byte_index - 1U) * 7U;
        const uint64_t payload = byte & 0x7fU;
        if (payload > (max_magnitude >> shift))
            throw std::out_of_range("BinaryReader: invalid varint");
        magnitude |= payload << shift;
        if (magnitude > max_magnitude)
            throw std::out_of_range("BinaryReader: invalid varint");
    }

    if (!negative) return static_cast<int64_t>(magnitude);
    if (magnitude == max_magnitude)
        return std::numeric_limits<int64_t>::min();
    return -static_cast<int64_t>(magnitude);
}

// ── 原始字节块读取 ──
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
std::vector<uint8_t> BinaryReader::read_bytes(size_t n) {
    ensure(n);
    std::vector<uint8_t> result(data_ + pos_, data_ + pos_ + n);
    pos_ += n;
    return result;
}

} // namespace explorgdb
