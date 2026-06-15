// src/edgar/explorgdb/utf16.cpp
// UTF-16LE → UTF-8 转换实现
//
// UTF-16 编码规则：
//   BMP 字符 (U+0000 ~ U+FFFF): 直接使用 2 字节表示
//   代理对 (U+10000 ~ U+10FFFF): 使用 4 字节（高代理 + 低代理）
//
// UTF-8 编码规则：
//   U+0000 ~ U+007F:  1 字节  [0xxxxxxx]
//   U+0080 ~ U+07FF:  2 字节  [110xxxxx 10xxxxxx]
//   U+0800 ~ U+FFFF:  3 字节  [1110xxxx 10xxxxxx 10xxxxxx]
//   U+10000 ~ U+10FFFF: 4 字节 [11110xxx 10xxxxxx 10xxxxxx 10xxxxxx]
//
// 当前实现处理 BMP 字符（最多 3 字节 UTF-8），不处理代理对。
// 遇到 null 终止符时提前结束。

#include "utf16.h"

namespace explorgdb {

std::string utf16le_to_utf8(const uint8_t* data, int char_count) {
    std::string out;
    out.reserve(char_count * 3);  // UTF-8 最多 3 字节/字符，预分配避免 realloc
    for (int i = 0; i < char_count; ++i) {
        // UTF-16LE: 低字节在前，高字节在后
        uint16_t c = static_cast<uint16_t>(data[i * 2]) |
                     (static_cast<uint16_t>(data[i * 2 + 1]) << 8);

        if (c == 0) break;  // null 终止符，提前结束

        if (c < 0x80) {
            // ASCII 范围 (U+0000 ~ U+007F): 1 字节 UTF-8
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            // U+0080 ~ U+07FF: 2 字节 UTF-8
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            // U+0800 ~ U+FFFF: 3 字节 UTF-8
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

} // namespace explorgdb
