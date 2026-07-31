// tests/edgar/explorgdb/test_utf16.cpp
// UTF-16LE → UTF-8 转换单元测试

#include <gtest/gtest.h>
#include "utf16.h"
#include <vector>
#include <cstring>

using namespace explorgdb;

// 辅助函数：将 UTF-16LE 字符序列转为字节向量
static std::vector<uint8_t> to_utf16le(std::initializer_list<uint16_t> chars) {
    std::vector<uint8_t> buf;
    for (uint16_t c : chars) {
        buf.push_back(static_cast<uint8_t>(c & 0xFF));        // 低字节
        buf.push_back(static_cast<uint8_t>((c >> 8) & 0xFF)); // 高字节
    }
    return buf;
}

// ── 基本场景 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(Utf16Test, EmptyInput) {
    // char_count=0 → 空字符串
    auto buf = to_utf16le({});
    EXPECT_EQ(utf16le_to_utf8(buf.data(), 0), "");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(Utf16Test, AsciiOnly) {
    // "Hello" → UTF-8 "Hello"
    auto buf = to_utf16le({'H', 'e', 'l', 'l', 'o'});
    EXPECT_EQ(utf16le_to_utf8(buf.data(), 5), "Hello");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(Utf16Test, SingleChar) {
    // 单字符 'A' (0x41)
    auto buf = to_utf16le({0x41});
    EXPECT_EQ(utf16le_to_utf8(buf.data(), 1), "A");
}

// ── Null 终止 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(Utf16Test, NullTermination) {
    // "AB\0CD" → 只转换到 null 之前，输出 "AB"
    auto buf = to_utf16le({'A', 'B', 0x0000, 'C', 'D'});
    EXPECT_EQ(utf16le_to_utf8(buf.data(), 5), "AB");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(Utf16Test, NullAtStart) {
    // 第一个字符就是 null → 空字符串
    auto buf = to_utf16le({0x0000, 'A', 'B'});
    EXPECT_EQ(utf16le_to_utf8(buf.data(), 3), "");
}

// ── 2 字节 UTF-8 范围 (U+0080 ~ U+07FF) ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(Utf16Test, TwoByteUtf8) {
    // é (U+00E9) → UTF-8: 0xC3 0xA9
    auto buf = to_utf16le({0x00E9});
    std::string result = utf16le_to_utf8(buf.data(), 1);
    EXPECT_EQ(result.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(result[0]), 0xC3);
    EXPECT_EQ(static_cast<uint8_t>(result[1]), 0xA9);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(Utf16Test, TwoByteBoundary) {
    // U+0080 (2 字节最小值) → 0xC2 0x80
    auto buf = to_utf16le({0x0080});
    std::string result = utf16le_to_utf8(buf.data(), 1);
    EXPECT_EQ(result.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(result[0]), 0xC2);
    EXPECT_EQ(static_cast<uint8_t>(result[1]), 0x80);

    // U+07FF (2 字节最大值) → 0xDF 0xBF
    buf = to_utf16le({0x07FF});
    result = utf16le_to_utf8(buf.data(), 1);
    EXPECT_EQ(result.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(result[0]), 0xDF);
    EXPECT_EQ(static_cast<uint8_t>(result[1]), 0xBF);
}

// ── 3 字节 UTF-8 范围 (U+0800 ~ U+FFFF) ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(Utf16Test, ThreeByteUtf8) {
    // 中 (U+4E2D) → UTF-8: 0xE4 0xB8 0xAD
    auto buf = to_utf16le({0x4E2D});
    std::string result = utf16le_to_utf8(buf.data(), 1);
    EXPECT_EQ(result.size(), 3u);
    EXPECT_EQ(static_cast<uint8_t>(result[0]), 0xE4);
    EXPECT_EQ(static_cast<uint8_t>(result[1]), 0xB8);
    EXPECT_EQ(static_cast<uint8_t>(result[2]), 0xAD);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(Utf16Test, ThreeByteBoundary) {
    // U+0800 (3 字节最小值) → 0xE0 0xA0 0x80
    auto buf = to_utf16le({0x0800});
    std::string result = utf16le_to_utf8(buf.data(), 1);
    EXPECT_EQ(result.size(), 3u);
    EXPECT_EQ(static_cast<uint8_t>(result[0]), 0xE0);
    EXPECT_EQ(static_cast<uint8_t>(result[1]), 0xA0);
    EXPECT_EQ(static_cast<uint8_t>(result[2]), 0x80);
}

// ── 混合内容 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(Utf16Test, MixedContent) {
    // "A中éB" → ASCII + CJK + Latin + ASCII
    auto buf = to_utf16le({'A', 0x4E2D, 0x00E9, 'B'});
    std::string result = utf16le_to_utf8(buf.data(), 4);
    // A(1) + 中(3) + é(2) + B(1) = 7 字节
    EXPECT_EQ(result.size(), 7u);
    EXPECT_EQ(result[0], 'A');
    // 中 → E4 B8 AD
    EXPECT_EQ(static_cast<uint8_t>(result[1]), 0xE4);
    EXPECT_EQ(static_cast<uint8_t>(result[2]), 0xB8);
    EXPECT_EQ(static_cast<uint8_t>(result[3]), 0xAD);
    // é → C3 A9
    EXPECT_EQ(static_cast<uint8_t>(result[4]), 0xC3);
    EXPECT_EQ(static_cast<uint8_t>(result[5]), 0xA9);
    EXPECT_EQ(result[6], 'B');
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(Utf16Test, MixedWithNull) {
    // "A\0中" → null 后不处理
    auto buf = to_utf16le({'A', 0x0000, 0x4E2D});
    std::string result = utf16le_to_utf8(buf.data(), 3);
    EXPECT_EQ(result, "A");
}
