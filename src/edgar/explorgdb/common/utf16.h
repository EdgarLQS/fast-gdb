// src/edgar/explorgdb/common/utf16.h
// UTF-16LE 转换 — FileGDB 名称、别名和空间参考文本的编码边界。

#ifndef EXPLORGDB_UTF16_H
#define EXPLORGDB_UTF16_H

#include <cstdint>
#include <string>

namespace explorgdb {

/**
 * 将 UTF-16LE 代码单元转换为 UTF-8。
 *
 * FileGDB 字段名称、别名和空间参考定义通常以 UTF-16LE 保存，调用方传入的
 * char_count 是 16-bit 代码单元数量而不是字节数。当前实现只处理 BMP，遇到
 * U+0000 提前结束，不组合代理对。
 *
 * @param data UTF-16LE 字节序列起始地址。
 * @param char_count 可读取的 16-bit 代码单元数量。
 * @return UTF-8 字符串。
 */
/** 将 UTF-16LE 字节序列转换为 UTF-8 字符串。
 * @param data UTF-16LE 数据首地址。
 * @param char_count UTF-16 代码单元数量。
 * @return 转换后的 UTF-8 字符串；非法序列按实现策略替换或截断。
 */
std::string utf16le_to_utf8(const uint8_t* data, int char_count);

/**
 * 将 UTF-8 字符串转换为 UTF-16 代码单元。
 *
 * 该函数服务于 Writer 的 FileGDB 元数据编码。当前只接受可表示为单个 BMP
 * 代码单元的字符；4 字节 UTF-8 序列不会生成代理对。
 *
 * @param str UTF-8 输入。
 * @return UTF-16 代码单元序列。
 */
/** 将 UTF-8 字符串转换为 UTF-16 字符串。
 * @param str 待转换的 UTF-8 字符串。
 * @return 转换后的 UTF-16 字符串。
 */
std::u16string utf8_to_utf16(const std::string& str);

} // namespace explorgdb

#endif // EXPLORGDB_UTF16_H
