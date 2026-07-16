// src/edgar/explorgdb/writer/row_buffer.h
// 行缓冲区 — 零分配 reusable buffer，用于构建单行 .gdbtable 记录
//
// 重要：GDAL 在记录中的字段顺序是按照 .gdbtable 字段描述符的顺序，
// 而不是用户创建的顺序。通常 Geometry 字段排在第一位。
// 因此 RowBuffer 使用 per-field 缓冲，finalize() 时按描述符顺序输出。
//
// 行格式（写入 .gdbtable 的每行）：
//   [blob_len: 4字节 uint32 LE]
//   [nullable_bitmap: N 字节]
//   [field_0 value]     ← 按描述符顺序，跳过 ObjectId
//   [field_1 value]
//   ...
//
// blob_len = bitmap_size + 所有字段值的字节数（不含 blob_len 本身）
//
// 使用方式：
//   RowBuffer buf;
//   buf.init(5, {true, false, true, true, true});  // 5 个描述符字段
//   // 对每一行，按任意顺序写入各字段：
//   buf.set_field(2);             // 切到描述符字段 2（name）
//   buf.append_string("hello");
//   buf.set_field(0);             // 切到描述符字段 0（geometry）
//   buf.append_geometry(blob, len);
//   buf.set_field(3);             // 切到描述符字段 3（population）
//   buf.append_f64(1000.0);
//   buf.set_field(4);             // 切到描述符字段 4（area）
//   buf.append_f64(25.0);
//   buf.finalize();               // 按描述符顺序组装完整行

#ifndef EXPLORGDB_ROW_BUFFER_H
#define EXPLORGDB_ROW_BUFFER_H

#include "../common/varint.h"

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

    // 初始化：指定描述符字段数量和每个字段是否 nullable
    // 必须在第一次 begin_row() 前调用
    void init(int num_descriptor_fields, const std::vector<bool>& nullable_flags) {
        num_fields_ = num_descriptor_fields;
        nullable_flags_ = nullable_flags;
        nullable_count_ = 0;
        for (bool n : nullable_flags_) {
            if (n) ++nullable_count_;
        }
        nullable_bytes_ = (nullable_count_ + 7) / 8;
        bitmap_.assign(nullable_bytes_, 0);

        // 为每个字段分配独立的缓冲区
        field_data_.resize(num_fields_);
        for (auto& fd : field_data_) {
            fd.clear();
        }
    }

    // 开始新行：清空所有字段缓冲区
    void begin_row() {
        std::fill(bitmap_.begin(), bitmap_.end(), 0);
        current_field_ = -1;
        for (auto& fd : field_data_) {
            fd.clear();
        }
    }

    // 切换到指定描述符字段（后续 append_* 调用写入此字段）
    void set_field(int descriptor_field_index) {
        current_field_ = descriptor_field_index;
    }

    // 标记当前字段为 null
    void set_null() {
        if (current_field_ < 0 || current_field_ >= num_fields_) return;
        if (!nullable_flags_[current_field_]) return;

        // 计算此字段在 bitmap 中的 bit 位置
        int bit_pos = 0;
        for (int i = 0; i < current_field_; ++i) {
            if (nullable_flags_[i]) ++bit_pos;
        }
        bitmap_[bit_pos / 8] |= static_cast<uint8_t>(1 << (bit_pos % 8));
    }

    // ── 字段写入方法（写入当前 set_field() 指定的字段缓冲区）──

    void append_i16(int16_t value) {
        auto& buf = cur_buf();
        size_t pos = buf.size();
        buf.resize(pos + 2);
        buf[pos++] = static_cast<uint8_t>(value & 0xFF);
        buf[pos++] = static_cast<uint8_t>((static_cast<uint16_t>(value) >> 8) & 0xFF);
    }

    void append_i32(int32_t value) {
        auto& buf = cur_buf();
        size_t pos = buf.size();
        buf.resize(pos + 4);
        uint32_t u = static_cast<uint32_t>(value);
        buf[pos++] = static_cast<uint8_t>(u & 0xFF);
        buf[pos++] = static_cast<uint8_t>((u >> 8) & 0xFF);
        buf[pos++] = static_cast<uint8_t>((u >> 16) & 0xFF);
        buf[pos++] = static_cast<uint8_t>((u >> 24) & 0xFF);
    }

    void append_i64(int64_t value) {
        auto& buf = cur_buf();
        size_t pos = buf.size();
        buf.resize(pos + 8);
        uint64_t u = static_cast<uint64_t>(value);
        for (int i = 0; i < 8; ++i) {
            buf[pos++] = static_cast<uint8_t>(u & 0xFF);
            u >>= 8;
        }
    }

    void append_f32(float value) {
        auto& buf = cur_buf();
        size_t pos = buf.size();
        buf.resize(pos + 4);
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        buf[pos++] = static_cast<uint8_t>(bits & 0xFF);
        buf[pos++] = static_cast<uint8_t>((bits >> 8) & 0xFF);
        buf[pos++] = static_cast<uint8_t>((bits >> 16) & 0xFF);
        buf[pos++] = static_cast<uint8_t>((bits >> 24) & 0xFF);
    }

    void append_f64(double value) {
        auto& buf = cur_buf();
        size_t pos = buf.size();
        buf.resize(pos + 8);
        uint64_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        for (int i = 0; i < 8; ++i) {
            buf[pos++] = static_cast<uint8_t>(bits & 0xFF);
            bits >>= 8;
        }
    }

    void append_string(const std::string& value) {
        auto& buf = cur_buf();
        uint64_t len = value.size();
        uint8_t varbuf[10];
        size_t vlen = encode_varuint_to(varbuf, len);
        buf.insert(buf.end(), varbuf, varbuf + vlen);
        buf.insert(buf.end(), value.begin(), value.end());
    }

    void append_binary(const uint8_t* data, size_t len) {
        auto& buf = cur_buf();
        uint8_t varbuf[10];
        size_t vlen = encode_varuint_to(varbuf, len);
        buf.insert(buf.end(), varbuf, varbuf + vlen);
        if (len > 0) buf.insert(buf.end(), data, data + len);
    }

    void append_bytes(const uint8_t* data, size_t len) {
        auto& buf = cur_buf();
        if (len > 0) buf.insert(buf.end(), data, data + len);
    }

    void append_geometry(const uint8_t* blob, size_t blob_len) {
        auto& buf = cur_buf();
        uint8_t varbuf[10];
        size_t vlen = encode_varuint_to(varbuf, blob_len);
        buf.insert(buf.end(), varbuf, varbuf + vlen);
        if (blob_len > 0) {
            buf.insert(buf.end(), blob, blob + blob_len);
        }
    }

    // 完成行：按描述符顺序组装完整行数据
    // 调用后 data() 和 size() 返回完整的行数据
    void finalize() {
        // 计算总大小
        size_t total = 4 + nullable_bytes_;  // blob_len + bitmap
        for (int i = 0; i < num_fields_; ++i) {
            // 跳过 ObjectId（不在记录中存储）
            if (is_objectid_[i]) continue;
            // 跳过 null 字段（bitmap 已标记，不写数据）
            if (nullable_flags_[i] && is_null_field(i)) continue;
            total += field_data_[i].size();
        }

        output_.resize(total);
        size_t pos = 0;

        // 1. blob_len（后面回填）
        uint32_t blob_len = static_cast<uint32_t>(total - 4);
        output_[pos++] = static_cast<uint8_t>(blob_len & 0xFF);
        output_[pos++] = static_cast<uint8_t>((blob_len >> 8) & 0xFF);
        output_[pos++] = static_cast<uint8_t>((blob_len >> 16) & 0xFF);
        output_[pos++] = static_cast<uint8_t>((blob_len >> 24) & 0xFF);

        // 2. nullable bitmap
        for (size_t i = 0; i < nullable_bytes_; ++i) {
            output_[pos++] = bitmap_[i];
        }

        // 3. 各字段数据（按描述符顺序，跳过 ObjectId 和 null 字段）
        for (int i = 0; i < num_fields_; ++i) {
            if (is_objectid_[i]) continue;
            if (nullable_flags_[i] && is_null_field(i)) continue;
            if (!field_data_[i].empty()) {
                std::memcpy(output_.data() + pos, field_data_[i].data(),
                            field_data_[i].size());
                pos += field_data_[i].size();
            }
        }
    }

    // 标记某个描述符字段为 ObjectId（不在记录中存储）
    void mark_objectid(int descriptor_field_index) {
        if (descriptor_field_index >= 0 && descriptor_field_index < num_fields_) {
            is_objectid_.resize(num_fields_, false);
            is_objectid_[descriptor_field_index] = true;
        }
    }

    // ── 访问最终数据 ──

    const uint8_t* data() const { return output_.data(); }
    size_t size() const { return output_.size(); }

private:
    std::vector<uint8_t>& cur_buf() {
        static std::vector<uint8_t> dummy;
        if (current_field_ < 0 || current_field_ >= num_fields_) return dummy;
        return field_data_[current_field_];
    }

    bool is_null_field(int field_index) const {
        int bit_pos = 0;
        for (int i = 0; i < field_index; ++i) {
            if (nullable_flags_[i]) ++bit_pos;
        }
        return (bitmap_[bit_pos / 8] & (1 << (bit_pos % 8))) != 0;
    }

    // encode_varuint_to 来自 ../common/varint.h

    int num_fields_ = 0;
    std::vector<bool> nullable_flags_;     // 每个描述符字段是否 nullable
    std::vector<bool> is_objectid_;        // 每个描述符字段是否是 ObjectId
    int nullable_count_ = 0;
    size_t nullable_bytes_ = 0;
    std::vector<uint8_t> bitmap_;          // nullable bitmap

    std::vector<std::vector<uint8_t>> field_data_;  // 每个字段的独立缓冲区
    int current_field_ = -1;               // 当前写入的字段索引

    std::vector<uint8_t> output_;          // finalize() 的输出缓冲区
};

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_ROW_BUFFER_H
