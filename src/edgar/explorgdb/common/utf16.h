// src/edgar/explorgdb/utf16.h
// UTF-16LE 字符串转换 — FileGDB 中所有名称和 WKT 字符串的编码格式
//
// FileGDB 约定：
//   - 字符串统一使用 UTF-16LE 编码（小端序，每字符 2 字节）
//   - 名称长度以字符个数给出（不是字节数）
//   - WKT 字符串长度以字节数给出（需要除以 2 才是字符数）
//   - 支持 BMP 字符（U+0000 ~ U+FFFF），不含代理对（surrogate pairs）
//
// 转换函数:
//   utf16le_to_utf8(data, char_count) — 将 UTF-16LE 字节流转为 UTF-8 std::string
//
// 注意：当前实现仅支持 BMP 字符（最多 3 字节 UTF-8 编码），
// 不处理代理对（surrogate pairs）。如果未来 GDB 数据包含 Emoji 等
// 超出 BMP 的字符，需要补充代理对处理。

#ifndef EXPLORGDB_UTF16_H
#define EXPLORGDB_UTF16_H

#include <cstdint>
#include <string>

namespace explorgdb {

// 将 UTF-16LE 编码的字节序列转换为 UTF-8 字符串
// data       — 指向 UTF-16LE 字节序列的指针（小端序）
// char_count — 字符个数（不是字节数，每字符占 2 字节）
// 返回       — UTF-8 编码的 std::string
// 遇到 null 字符（U+0000）时提前终止
std::string utf16le_to_utf8(const uint8_t* data, int char_count);

} // namespace explorgdb

#endif // EXPLORGDB_UTF16_H
