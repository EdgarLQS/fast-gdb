// src/edgar/explorgdb/gdb_indexes.cpp
// .gdbindexes 索引元数据解析实现
//
// 文件中的字符串是索引表达式。常见值为裸字段名，也可能是
// LOWER(field)。解析器保留原始表达式在 IndexEntry::column_name 中，
// 并通过辅助函数提供字段关联和直接表达式判定。

#include "gdb_indexes.h"
#include "binary_reader.h"
#include "explorgdb_constants.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

namespace explorgdb {
namespace {

bool starts_with_ci(const std::string& value, const char* prefix) {
    const size_t prefix_size = std::strlen(prefix);
    if (value.size() < prefix_size) return false;
    for (size_t index = 0; index < prefix_size; ++index) {
        if (std::tolower(static_cast<unsigned char>(value[index])) !=
            std::tolower(static_cast<unsigned char>(prefix[index]))) {
            return false;
        }
    }
    return true;
}

} // namespace

GdbIndexesParser::GdbIndexesParser(const std::string& file_path)
    : file_path_(file_path) {}

std::string GdbIndexesParser::field_name_from_expression(
    const std::string& expression) {
    constexpr const char* lower_prefix = "LOWER(";
    if (starts_with_ci(expression, lower_prefix) &&
        expression.size() > std::strlen(lower_prefix) &&
        expression.back() == ')') {
        return expression.substr(
            std::strlen(lower_prefix),
            expression.size() - std::strlen("LOWER()"));
    }
    return expression;
}

bool GdbIndexesParser::is_direct_field_expression(
    const std::string& expression) {
    return !expression.empty() &&
           expression.find('(') == std::string::npos &&
           expression.find(')') == std::string::npos;
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbIndexesParser::parse() {
    std::ifstream input(file_path_, std::ios::binary | std::ios::ate);
    if (!input.is_open()) return false;

    const std::streamoff end_offset = input.tellg();
    if (end_offset < 0) return false;
    const uintmax_t unsigned_size = static_cast<uintmax_t>(end_offset);
    if (unsigned_size >
            static_cast<uintmax_t>(std::numeric_limits<size_t>::max()) ||
        unsigned_size > static_cast<uintmax_t>(
            std::numeric_limits<std::streamsize>::max())) {
        return false;
    }
    const size_t byte_count = static_cast<size_t>(unsigned_size);
    input.seekg(0, std::ios::beg);
    file_data_.resize(byte_count);
    input.read(reinterpret_cast<char*>(file_data_.data()),
               static_cast<std::streamsize>(byte_count));
    if (!input) return false;

    entries_.clear();
    try {
        BinaryReader reader(file_data_);
        const int32_t index_count = reader.read_i32();
        if (index_count < 0) return false;

        for (int32_t index = 0; index < index_count; ++index) {
            IndexEntry entry;

            const int32_t name_length = reader.read_i32();
            if (name_length < 0) return false;
            entry.name = reader.read_utf16(name_length);

            entry.magic1 = reader.read_u16();
            entry.magic2 = reader.read_i32();
            entry.magic3 = reader.read_u16();

            const bool known_magic =
                (entry.magic2 == 2 && entry.magic3 == 0) ||
                (entry.magic2 == 4 && entry.magic3 == 0) ||
                (entry.magic2 == 16 && entry.magic3 == 65535);
            if (known_magic) entry.magic4 = reader.read_i32();

            const int32_t expression_length = known_magic
                ? reader.read_i32()
                : entry.magic2;
            if (expression_length < 0) return false;
            entry.column_name = reader.read_utf16(expression_length);
            entry.magic5 = reader.read_u16();
            entries_.push_back(std::move(entry));
        }
    } catch (...) {
        entries_.clear();
        return false;
    }

    return true;
}

} // namespace explorgdb
