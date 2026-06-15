// src/edgar/explorgdb/writer/row_buffer.h
// 行缓冲区 — 零分配 reusable buffer，用于构建单行 .gdbtable 记录
//
// 设计目标：
//   - 内部 buffer 只增长不缩小，跨行复用避免反复 malloc/free
//   - 提供 append_* 系列方法，逐步写入各字段
//   - finalize() 后 buffer 内容即为完整的行记录（含 blob_len 前缀）
//
// 行格式（写入 .gdbtable 的每行）：
//   [blob_len: 4字节 uint32 LE]     ← begin_row 预留，finalize 回填
//   [nullable_bitmap: N 字节]       ← begin_row 预留，finalize 回填
//   [field_1 value]                 ← append_* 方法逐步写入
//   [field_2 value]
//   ...
//   [geometry varuint_len + blob]
//
// blob_len = bitmap_size + 所有字段值的字节数（不含 blob_len 本身）
//
// 使用方式：
//   RowBuffer buf;
//   buf.init({true, false, true, true});  // 4 个字段，3 个 nullable
//   // 对每一行：
//   buf.begin_row();
//   buf.set_null();           // 字段 0 = null（nullable）
//   buf.append_i32(42);       // 字段 1 = 42（不可 null）
//   buf.append_string("hello"); // 字段 2 = "hello"（nullable）
//   buf.append_geometry(...); // 字段 3 = 几何 blob（nullable）
//   buf.finalize();           // 回填 blob_len + bitmap
//   const uint8_t* data = buf.data();
//   size_t size = buf.size();

#ifndef EXPLORGDB_ROW_BUFFER_H
#define EXPLORGDB_ROW_BUFFER_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace explorgdb {
namespace writer {

class RowBuffer {
public:
    RowBuffer() = default;

    // 初始化：指定每个字段是否 nullable（决定 bitmap 布局）
    // 必须在第一次 begin_row() 前调用
    void init(const std::vector<bool>& nullable_flags) {
        nullable_flags_ = nullable_flags;
        nullable_count_ = 0;
        for (bool n : nullable_flags_) {
            if (n) ++nullable_count_;
        }
        nullable_bytes_ = (nullable_count_ + 7) / 8;
        bitmap_.assign(nullable_bytes_, 0);
    }

    // 开始新行：重置写入位置，预留 blob_len + bitmap 空间
    void begin_row() {
        pos_ = 0;
        // 预留 4 字节给 blob_len（写 0 占位）
        append_raw_u32(0);
        // 预留 nullable_bytes_ 字节给 bitmap（写 0 占位）
        for (size_t i = 0; i < nullable_bytes_; ++i) {
            append_raw_u8(0);
        }
        // 清空 bitmap 和字段计数器
        std::fill(bitmap_.begin(), bitmap_.end(), 0);
        field_index_ = 0;
        nullable_bit_index_ = 0;
    }

    // ── 字段写入方法 ──
    // 注意：调用顺序必须与 init() 中的字段顺序一致。
    // 如果当前字段是 nullable 的，调用者必须先决定是否 set_null()。

    // 标记当前字段为 null（仅 nullable 字段可调用）
    void set_null() {
        int byte_idx = nullable_bit_index_ / 8;
        int bit_idx = nullable_bit_index_ % 8;
        bitmap_[byte_idx] |= static_cast<uint8_t>(1 << bit_idx);
        ++nullable_bit_index_;
        ++field_index_;
        // null 字段不写入任何数据到 buffer
    }

    // 写入 Int16（2 字节 LE）
    void append_i16(int16_t value) {
        ensure_capacity(pos_ + 2);
        buffer_[pos_++] = static_cast<uint8_t>(value & 0xFF);
        buffer_[pos_++] = static_cast<uint8_t>((static_cast<uint16_t>(value) >> 8) & 0xFF);
        advance_field();
    }

    // 写入 Int32（4 字节 LE）
    void append_i32(int32_t value) {
        ensure_capacity(pos_ + 4);
        uint32_t u = static_cast<uint32_t>(value);
        buffer_[pos_++] = static_cast<uint8_t>(u & 0xFF);
        buffer_[pos_++] = static_cast<uint8_t>((u >> 8) & 0xFF);
        buffer_[pos_++] = static_cast<uint8_t>((u >> 16) & 0xFF);
        buffer_[pos_++] = static_cast<uint8_t>((u >> 24) & 0xFF);
        advance_field();
    }

    // 写入 Int64（8 字节 LE）
    void append_i64(int64_t value) {
        ensure_capacity(pos_ + 8);
        uint64_t u = static_cast<uint64_t>(value);
        for (int i = 0; i < 8; ++i) {
            buffer_[pos_++] = static_cast<uint8_t>(u & 0xFF);
            u >>= 8;
        }
        advance_field();
    }

    // 写入 Float32（4 字节 IEEE754 LE）
    void append_f32(float value) {
        ensure_capacity(pos_ + 4);
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        buffer_[pos_++] = static_cast<uint8_t>(bits & 0xFF);
        buffer_[pos_++] = static_cast<uint8_t>((bits >> 8) & 0xFF);
        buffer_[pos_++] = static_cast<uint8_t>((bits >> 16) & 0xFF);
        buffer_[pos_++] = static_cast<uint8_t>((bits >> 24) & 0xFF);
        advance_field();
    }

    // 写入 Float64（8 字节 IEEE754 LE）
    void append_f64(double value) {
        ensure_capacity(pos_ + 8);
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        for (int i = 0; i < 8; ++i) {
            buffer_[pos_++] = static_cast<uint8_t>(bits & 0xFF);
            bits >>= 8;
        }
        advance_field();
    }

    // 写入字符串（varuint 字节长度 + UTF-8 字节）
    void append_string(const std::string& value) {
        uint64_t len = value.size();
        size_t vlen = encode_varuint(buffer_.data() + pos_, len);
        pos_ += vlen;
        ensure_capacity(pos_ + len);
        std::memcpy(buffer_.data() + pos_, value.data(), len);
        pos_ += len;
        advance_field();
    }

    // 写入原始二进制（varuint 长度前缀 + 数据）
    void append_binary(const uint8_t* data, size_t len) {
        size_t vlen = encode_varuint(buffer_.data() + pos_, len);
        pos_ += vlen;
        ensure_capacity(pos_ + len);
        std::memcpy(buffer_.data() + pos_, data, len);
        pos_ += len;
        advance_field();
    }

    // 写入几何 blob（varuint 长度前缀 + blob 数据）
    // blob 内容由 GeometrySerializer 生成
    // blob_len=0 表示空几何（POINT EMPTY）
    void append_geometry(const uint8_t* blob, size_t blob_len) {
        size_t vlen = encode_varuint(buffer_.data() + pos_, blob_len);
        pos_ += vlen;
        if (blob_len > 0) {
            ensure_capacity(pos_ + blob_len);
            std::memcpy(buffer_.data() + pos_, blob, blob_len);
            pos_ += blob_len;
        }
        advance_field();
    }

    // 完成行：回填 blob_len 和 nullable bitmap
    // 调用后 data() 和 size() 返回完整的行数据
    void finalize() {
        // 1. 回填 nullable bitmap（位于 offset 4 处）
        for (size_t i = 0; i < nullable_bytes_; ++i) {
            buffer_[4 + i] = bitmap_[i];
        }

        // 2. 回填 blob_len（位于 offset 0）
        //    blob_len = 总大小 - 4（blob_len 本身不算）
        uint32_t blob_len = static_cast<uint32_t>(pos_ - 4);
        buffer_[0] = static_cast<uint8_t>(blob_len & 0xFF);
        buffer_[1] = static_cast<uint8_t>((blob_len >> 8) & 0xFF);
        buffer_[2] = static_cast<uint8_t>((blob_len >> 16) & 0xFF);
        buffer_[3] = static_cast<uint8_t>((blob_len >> 24) & 0xFF);
    }

    // ── 访问最终数据 ──

    const uint8_t* data() const { return buffer_.data(); }
    size_t size() const { return pos_; }

    // 内容大小（不含 blob_len 前缀的 4 字节）
    size_t content_size() const { return pos_ - 4; }

private:
    void advance_field() {
        if (field_index_ < static_cast<int>(nullable_flags_.size()) &&
            nullable_flags_[field_index_]) {
            ++nullable_bit_index_;
        }
        ++field_index_;
    }

    void ensure_capacity(size_t needed) {
        if (needed > buffer_.size()) {
            buffer_.resize(std::max(needed, buffer_.size() * 2));
        }
    }

    void append_raw_u8(uint8_t value) {
        ensure_capacity(pos_ + 1);
        buffer_[pos_++] = value;
    }

    void append_raw_u32(uint32_t value) {
        ensure_capacity(pos_ + 4);
        buffer_[pos_++] = static_cast<uint8_t>(value & 0xFF);
        buffer_[pos_++] = static_cast<uint8_t>((value >> 8) & 0xFF);
        buffer_[pos_++] = static_cast<uint8_t>((value >> 16) & 0xFF);
        buffer_[pos_++] = static_cast<uint8_t>((value >> 24) & 0xFF);
    }

    static size_t encode_varuint(uint8_t* dst, uint64_t value) {
        size_t n = 0;
        do {
            uint8_t byte = static_cast<uint8_t>(value & 0x7F);
            value >>= 7;
            if (value != 0) byte |= 0x80;
            dst[n++] = byte;
        } while (value != 0);
        return n;
    }

    std::vector<uint8_t> buffer_;       // 内部 buffer（只增不减）
    size_t pos_ = 0;                     // 当前写入位置

    std::vector<bool> nullable_flags_;   // 每个字段是否 nullable
    int nullable_count_ = 0;             // nullable 字段总数
    size_t nullable_bytes_ = 0;          // bitmap 字节数
    std::vector<uint8_t> bitmap_;        // nullable bitmap（finalize 时写回 buffer）

    int field_index_ = 0;                // 当前字段索引
    int nullable_bit_index_ = 0;         // 当前 nullable bit 索引
};

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_ROW_BUFFER_H
