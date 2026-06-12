// tests/edgar/explorgdb/test_binary_reader.cpp
// BinaryReader 单元测试
// 测试边界检查、字节序、seek/tell、非标准宽度整数读取

#include "binary_reader.h"
#include <gtest/gtest.h>
#include <cstring>

using namespace explorgdb;

// ── 基础读取测试 ──

// 测试 u8/u16/u32/u64 小端读取
TEST(BinaryReaderTest, BasicIntegerReads) {
    // 构造小端数据：0x01, 0x0201, 0x04030201, 0x0807060504030201
    std::vector<uint8_t> data = {
        0x01,                               // u8
        0x01, 0x02,                         // u16 LE = 0x0201
        0x01, 0x02, 0x03, 0x04,             // u32 LE = 0x04030201
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08  // u64 LE
    };

    BinaryReader br(data);

    EXPECT_EQ(br.read_u8(), 0x01);
    EXPECT_EQ(br.read_u16(), 0x0201);
    EXPECT_EQ(br.read_u32(), 0x04030201);
    EXPECT_EQ(br.read_u64(), 0x0807060504030201ULL);
}

// 测试有符号整数读取
TEST(BinaryReaderTest, SignedIntegerReads) {
    std::vector<uint8_t> data = {
        0xFF, 0xFF,   // i16 LE = -1
        0xFF, 0xFF, 0xFF, 0xFF,  // i32 LE = -1
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF  // i64 LE = -1
    };

    BinaryReader br(data);

    EXPECT_EQ(br.read_i16(), -1);
    EXPECT_EQ(br.read_i32(), -1);
    EXPECT_EQ(br.read_i64(), -1);
}

// 测试 i64 正值
TEST(BinaryReaderTest, ReadI64Positive) {
    std::vector<uint8_t> data(8, 0);
    int64_t val = 0x0102030405060708LL;
    std::memcpy(data.data(), &val, 8);

    BinaryReader br(data);
    EXPECT_EQ(br.read_i64(), 0x0102030405060708LL);
}

// 测试浮点数读取
TEST(BinaryReaderTest, FloatReads) {
    std::vector<uint8_t> data(12, 0);

    // float 1.5f
    float f = 1.5f;
    std::memcpy(data.data(), &f, 4);

    // double 3.14159
    double d = 3.14159;
    std::memcpy(data.data() + 4, &d, 8);

    BinaryReader br(data);

    EXPECT_FLOAT_EQ(br.read_f32(), 1.5f);
    EXPECT_NEAR(br.read_f64(), 3.14159, 1e-5);
}

// ── 非标准宽度整数读取 ──

// 测试 uint40 (5 字节) 读取
TEST(BinaryReaderTest, UInt40Read) {
    // 值 = 0x0102030405, 小端: 05 04 03 02 01
    std::vector<uint8_t> data = {0x05, 0x04, 0x03, 0x02, 0x01};
    BinaryReader br(data);

    uint40_t result = br.read_u40();
    EXPECT_EQ(result.value, 0x0102030405ULL);
}

// 测试 uint48 (6 字节) 读取
TEST(BinaryReaderTest, UInt48Read) {
    // 值 = 0x010203040506, 小端: 06 05 04 03 02 01
    std::vector<uint8_t> data = {0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    BinaryReader br(data);

    uint48_t result = br.read_u48();
    EXPECT_EQ(result.value, 0x010203040506ULL);
}

// ── 边界检查测试 ──

// 测试越界读取抛出异常
TEST(BinaryReaderTest, OutOfRangeThrows) {
    std::vector<uint8_t> data = {0x01};
    BinaryReader br(data);

    br.read_u8();  // 正常读取

    EXPECT_THROW(br.read_u8(), std::out_of_range);
    EXPECT_THROW(br.read_u16(), std::out_of_range);
    EXPECT_THROW(br.read_u32(), std::out_of_range);
    EXPECT_THROW(br.read_u64(), std::out_of_range);
}

// 测试 can_read 正确判断可读性
TEST(BinaryReaderTest, CanReadCheck) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    BinaryReader br(data);

    EXPECT_TRUE(br.can_read(3));
    EXPECT_TRUE(br.can_read(1));
    EXPECT_FALSE(br.can_read(4));

    br.read_u8();
    EXPECT_TRUE(br.can_read(2));
    EXPECT_FALSE(br.can_read(3));
}

// ── Seek/Tell 测试 ──

// 测试 seek 和 tell 的光标移动
TEST(BinaryReaderTest, SeekAndTell) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05};
    BinaryReader br(data);

    EXPECT_EQ(br.tell(), 0u);

    br.read_u8();
    EXPECT_EQ(br.tell(), 1u);

    br.seek(3);
    EXPECT_EQ(br.tell(), 3u);
    EXPECT_EQ(br.read_u8(), 0x04);

    // seek 越界应抛异常
    EXPECT_THROW(br.seek(10), std::out_of_range);
}

// 测试 skip 跳过指定字节
TEST(BinaryReaderTest, SkipBytes) {
    std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05};
    BinaryReader br(data);

    br.skip(2);
    EXPECT_EQ(br.tell(), 2u);
    EXPECT_EQ(br.read_u8(), 0x03);

    // skip 越界应抛异常
    EXPECT_THROW(br.skip(10), std::out_of_range);
}

// ── 零拷贝切片测试 ──

// 测试 data() 返回原始指针用于切片
TEST(BinaryReaderTest, DataPointer) {
    std::vector<uint8_t> data = {0xAA, 0xBB, 0xCC};
    BinaryReader br(data);

    EXPECT_EQ(br.data()[0], 0xAA);
    EXPECT_EQ(br.data()[2], 0xCC);
}

// ── read_bytes 测试 ──

// 测试读取原始字节序列
TEST(BinaryReaderTest, ReadBytes) {
    std::vector<uint8_t> data = {0xDE, 0xAD, 0xBE, 0xEF};
    BinaryReader br(data);

    auto result = br.read_bytes(2);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], 0xDE);
    EXPECT_EQ(result[1], 0xAD);

    // 剩余 2 字节
    EXPECT_EQ(br.read_bytes(2).size(), 2u);
    EXPECT_THROW(br.read_bytes(1), std::out_of_range);
}
