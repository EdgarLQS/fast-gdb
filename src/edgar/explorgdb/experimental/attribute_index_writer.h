// src/edgar/explorgdb/writer/attribute_index_writer.h
// 属性索引写入器 — 多类型字段值 B+ 树构建
//
// 值类型映射：
//   value_size=2  → INT16 (LE uint16)
//   value_size=4  → INT32/FLOAT32 (LE)
//   value_size=8  → INT64/FLOAT64/DATE (LE double)
//   value_size>8  → STRING (UTF16-LE, 空格填充, 最大 80 字符)
//
// 使用方式：
//   // Int32 索引
//   AttributeIndexWriter<int32_t> writer;
//   writer.set_value_size(4);
//   writer.set_numeric(true);
//   writer.add_value(fid1, value1);
//   writer.write("a00000001.MyIndex.atx");
//
//   // String 索引
//   StringAttributeIndexWriter writer;
//   writer.set_max_length(40);  // 最大字符数
//   writer.add_value(fid1, "hello");
//   writer.write("a00000001.NameIndex.atx");

#ifndef EXPLORGDB_ATTRIBUTE_INDEX_WRITER_H
#define EXPLORGDB_ATTRIBUTE_INDEX_WRITER_H

#include "bplus_tree_writer.h"
#include "../common/utf16.h"
#include <cstdint>
#include <string>
#include <cstring>
#include <algorithm>

namespace explorgdb {
namespace writer {

// 通用属性索引写入器（数值类型）
template<typename T>
class AttributeIndexWriter {
public:
    AttributeIndexWriter() = default;

    void set_value_size(uint8_t size) { btree_.set_value_size(size); }
    void set_numeric(bool v) { btree_.set_numeric(v); }
    void set_string(bool v) { btree_.set_string(v); }

    void add_value(uint32_t fid, T value) {
        btree_.add_entry(value, fid);
    }

    void clear() { btree_.clear(); }

    bool write(const std::string& path) const {
        if (btree_.empty()) return false;
        return btree_.write(path);
    }

    size_t size() const { return btree_.size(); }
    bool empty() const { return btree_.empty(); }

private:
    BPlusTreeWriter<T> btree_;
};

// 字符串属性索引写入器
class StringAttributeIndexWriter {
public:
    StringAttributeIndexWriter() {
        btree_.set_numeric(false);
        btree_.set_string(true);
    }

    // 设置最大字符数（默认 80）
    void set_max_length(int max_chars) {
        max_chars_ = max_chars;
        btree_.set_value_size(static_cast<uint8_t>(max_chars * 2));  // UTF-16
    }

    void add_value(uint32_t fid, const std::string& value) {
        // UTF-8 → UTF-16LE，空格填充到固定长度
        std::u16string utf16 = explorgdb::utf8_to_utf16(value);

        // 截断或填充
        if (utf16.size() > static_cast<size_t>(max_chars_)) {
            utf16 = utf16.substr(0, max_chars_);
        }
        while (utf16.size() < static_cast<size_t>(max_chars_)) {
            utf16.push_back(u' ');  // 空格填充 (0x0020)
        }

        // 存储为 StringValue
        StringValue sv;
        sv.data.resize(max_chars_ * 2);
        for (int i = 0; i < max_chars_; ++i) {
            uint16_t ch = static_cast<uint16_t>(utf16[i]);
            sv.data[i * 2] = static_cast<uint8_t>(ch & 0xFF);
            sv.data[i * 2 + 1] = static_cast<uint8_t>((ch >> 8) & 0xFF);
        }

        btree_.add_entry(sv, fid);
    }

    void clear() { btree_.clear(); }

    bool write(const std::string& path) const {
        if (btree_.empty()) return false;
        return btree_.write(path);
    }

    size_t size() const { return btree_.size(); }
    bool empty() const { return btree_.empty(); }

private:
    // 字符串值（固定长度字节数组）
    struct StringValue {
        std::vector<uint8_t> data;

        bool operator<(const StringValue& other) const {
            return data < other.data;
        }
        bool operator==(const StringValue& other) const {
            return data == other.data;
        }
    };

    BPlusTreeWriter<StringValue> btree_;
    int max_chars_ = 80;
};

// StringValue 的序列化特化
template<>
inline void BPlusTreeWriter<StringAttributeIndexWriter::StringValue>::serialize_value(
    std::vector<uint8_t>& buf,
    const StringAttributeIndexWriter::StringValue& v) const
{
    buf.insert(buf.end(), v.data.begin(), v.data.end());
}

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_ATTRIBUTE_INDEX_WRITER_H
