// src/edgar/explorgdb/binary_reader.h
// 二进制光标读取器 — 所有 GDB 文件解析的底层基础设施
//
// 设计目标：
//   - 在原始字节缓冲区上提供小端（LE）有序的光标读取
//   - 每次读取自动推进位置，支持 seek/skip/tell 显式移动
//   - 越界读取抛出 std::out_of_range，防止缓冲区溢出
//   - 支持非标准宽度整数（5 字节 uint40、6 字节 uint48），用于 .gdbtablx 变长偏移表
//
// 使用方式：
//   BinaryReader br(file_data);
//   uint32_t version = br.read_u32();   // 读 4 字节，光标自动前进
//   br.seek(40);                        // 跳到偏移 40
//   std::string name = br.read_utf16(10); // 读 10 个 UTF-16 字符

#ifndef EXPLORGDB_BINARY_READER_H
#define EXPLORGDB_BINARY_READER_H

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace explorgdb {

// ── 非标准宽度整数类型 ──
// 用于 .gdbtablx 偏移表中 5 字节和 6 字节编码的条目。
// FileGDB 根据表大小选择偏移宽度：
//   小表 → 4 字节（最大 4GB 文件）
//   中表 → 5 字节（最大 ~256TB）
//   大表 → 6 字节（最大 ~16EB）
struct uint40_t { uint64_t value; };  // 5 字节无符号，小端读取
struct uint48_t { uint64_t value; };  // 6 字节无符号，小端读取

// ── BinaryReader 类 ──
// 不拷贝数据，仅持有一个 const uint8_t* 指针和大小。
// 线程安全：多个独立实例可并行读取同一份数据。
class BinaryReader {
public:
    // 从 vector 构造（常用方式，文件数据加载到 vector 后传入）
    explicit BinaryReader(const std::vector<uint8_t>& data)
        : data_(data.data()), size_(data.size()), pos_(0) {}

    // 从原始指针构造（用于切片视图，如解析记录时从中间偏移开始）
    explicit BinaryReader(const uint8_t* data, size_t size)
        : data_(data), size_(size), pos_(0) {}

    // ── 基本类型读取（小端有序） ──

    uint8_t  read_u8();   // 读取 1 字节无符号
    uint16_t read_u16();  // 读取 2 字节无符号，LE：byte0 | (byte1 << 8)
    uint32_t read_u32();  // 读取 4 字节无符号，LE
    uint64_t read_u64();  // 读取 8 字节无符号，LE
    uint40_t read_u40();  // 读取 5 字节无符号，用于 .gdbtablx 5 字节偏移
    uint48_t read_u48();  // 读取 6 字节无符号，用于 .gdbtablx 6 字节偏移
    int16_t  read_i16();  // 读取 2 字节有符号（位模式相同，reinterpret）
    int32_t  read_i32();  // 读取 4 字节有符号
    int64_t  read_i64();  // 读取 8 字节有符号
    float    read_f32();  // 读取 IEEE 754 单精度浮点
    double   read_f64();  // 读取 IEEE 754 双精度浮点

    // ── 字符串和变长编码读取 ──

    // 读取 UTF-16LE 编码的字符串并转为 UTF-8
    // char_count 是字符个数，不是字节数（每字符 2 字节）
    std::string read_utf16(int char_count);

    // 读取无符号 VarInt（7-bit 数据/字节，bit 7 = 延续标志）
    // 编码示例：0 → [0x00], 127 → [0x7F], 128 → [0x80, 0x01]
    uint64_t    read_varuint();

    // 读取有符号 VarInt（bit 6 = 符号位，首字节 6-bit 数据）
    // 编码示例：0 → [0x00], +1 → [0x02], -1 → [0x03]
    int64_t     read_varint();

    // 读取 n 个原始字节（用于位图、flags 等不需要类型解释的数据）
    std::vector<uint8_t> read_bytes(size_t n);

    // ── 光标控制 ──

    size_t tell() const { return pos_; }  // 当前读取位置

    // 将光标移动到绝对偏移（超过文件大小时抛异常）
    void   seek(size_t offset) { if (offset > size_) throw std::out_of_range("seek past end"); pos_ = offset; }

    // 跳过 n 字节（数据不足时抛异常）
    void   skip(size_t bytes) { ensure(bytes); pos_ += bytes; }

    // 检查是否还能读取 n 字节
    bool   can_read(size_t bytes) const { return pos_ + bytes <= size_; }

    // 缓冲区总大小
    size_t size() const { return size_; }

    // 返回原始指针（用于零拷贝切片或外部 C API 交互）
    const uint8_t* data() const { return data_; }

private:
    // 确保当前位置后至少有 n 字节可读，否则抛出 std::out_of_range
    void ensure(size_t n) const {
        if (pos_ + n > size_) throw std::out_of_range("BinaryReader: read past end");
    }

    const uint8_t* data_;   // 不拥有数据，仅指向加载到内存的文件内容
    size_t size_;           // 缓冲区总大小
    size_t pos_;            // 当前光标位置，每次读取后自动递增
};

} // namespace explorgdb

#endif // EXPLORGDB_BINARY_READER_H
