// tests/edgar/explorgdb/test_varint.cpp
// VarInt 编解码往返测试
// 测试无符号 varuint 和有符号 varint 的编码/解码正确性

#include "binary_reader.h"
#include "varint.h"
#include <gtest/gtest.h>

using namespace explorgdb;

// ── 无符号 Varuint 测试 ──

// 测试 0 的编码和解码
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(VaruintTest, Zero) {
    auto encoded = encode_varuint(0);
    ASSERT_EQ(encoded.size(), 1u);
    EXPECT_EQ(encoded[0], 0x00);  // 无 continuation bit

    // 解码验证
    BinaryReader br(encoded);
    EXPECT_EQ(br.read_varuint(), 0u);
}

// 测试小值 (单字节) 编码解码
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(VaruintTest, SingleByte) {
    for (uint64_t v = 1; v <= 127; ++v) {
        auto encoded = encode_varuint(v);
        ASSERT_EQ(encoded.size(), 1u);
        EXPECT_EQ(encoded[0], static_cast<uint8_t>(v));

        BinaryReader br(encoded);
        EXPECT_EQ(br.read_varuint(), v);
    }
}

// 测试 127 (最大单字节值)
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(VaruintTest, MaxSingleByte) {
    auto encoded = encode_varuint(127);
    ASSERT_EQ(encoded.size(), 1u);
    EXPECT_EQ(encoded[0], 127);

    BinaryReader br(encoded);
    EXPECT_EQ(br.read_varuint(), 127u);
}

// 测试 128 (最小双字节值)
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(VaruintTest, MinTwoByte) {
    auto encoded = encode_varuint(128);
    ASSERT_EQ(encoded.size(), 2u);
    // 128 = 0x80 | 0x01, 0x00 → [0x80, 0x01] 的低7位编码
    // 实际: byte0 = (128 & 0x7F) | 0x80 = 0x80, byte1 = (128 >> 7) & 0x7F = 0x01
    EXPECT_EQ(encoded[0], 0x80);  // continuation + data bit
    EXPECT_EQ(encoded[1], 0x01);  // no continuation

    BinaryReader br(encoded);
    EXPECT_EQ(br.read_varuint(), 128u);
}

// 测试多字节值
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(VaruintTest, MultiByte) {
    uint64_t values[] = {255, 256, 16383, 16384, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF};

    for (uint64_t v : values) {
        auto encoded = encode_varuint(v);
        BinaryReader br(encoded);
        EXPECT_EQ(br.read_varuint(), v) << "Failed for value: " << v;
    }
}

// ── 有符号 Varint 测试 ──

// 测试 0 的编码解码
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(VarintTest, Zero) {
    auto encoded = encode_varint(0);
    BinaryReader br(encoded);
    EXPECT_EQ(br.read_varint(), 0);
}

// 测试正数
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(VarintTest, Positive) {
    int64_t values[] = {1, 63, 64, 127, 128, 16383, 0x7FFFFFFF};

    for (int64_t v : values) {
        auto encoded = encode_varint(v);
        BinaryReader br(encoded);
        EXPECT_EQ(br.read_varint(), v) << "Failed for positive value: " << v;
    }
}

// 测试负数
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(VarintTest, Negative) {
    int64_t values[] = {-1, -63, -64, -127, -128, -16383, -0x7FFFFFFF};

    for (int64_t v : values) {
        auto encoded = encode_varint(v);
        BinaryReader br(encoded);
        EXPECT_EQ(br.read_varint(), v) << "Failed for negative value: " << v;
    }
}

// 测试边界值
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(VarintTest, EdgeValues) {
    // 最大正数 (6-bit first byte: 63)
    {
        auto encoded = encode_varint(63);
        BinaryReader br(encoded);
        EXPECT_EQ(br.read_varint(), 63);
    }

    // 最小负数 (-63)
    {
        auto encoded = encode_varint(-63);
        BinaryReader br(encoded);
        EXPECT_EQ(br.read_varint(), -63);
    }
}
