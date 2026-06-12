// tests/edgar/explorgdb/test_gdb_attribute_index.cpp
// GdbAttributeIndexParser 正确性单元测试
// 由于没有 .atx 测试数据，使用合成文件

#include <gtest/gtest.h>
#include "gdb_attribute_index.h"
#include <filesystem>
#include <fstream>
#include <cstring>

namespace fs = std::filesystem;
using namespace explorgdb;

// ── 合成 .atx 文件工具 ──

// 构建一个最小数值型 .atx（tree_depth=1，单叶子页面）
// 包含 3 条记录: fid=1→10.0, fid=2→20.0, fid=3→30.0
static std::string build_numeric_atx() {
    std::string path = "/tmp/test_numeric.atx";
    std::vector<uint8_t> page(4096, 0);

    // 页面头
    // next_page_id = 0 (无后续叶子)
    page[0] = 0; page[1] = 0; page[2] = 0; page[3] = 0;
    // entry_count = 3
    page[4] = 3; page[5] = 0; page[6] = 0; page[7] = 0;
    // unused = 0
    page[8] = 0; page[9] = 0; page[10] = 0; page[11] = 0;

    // mpp = (4096-12)/(4+8) = 340
    // FID 数组: 从 offset 12 开始，3 × 4 字节
    uint32_t fids[] = {1, 2, 3};
    std::memcpy(page.data() + 12, fids, 12);

    // 值数组: 从 offset 12 + 340*4 = 1372 开始，3 × 8 字节 (double)
    double values[] = {10.0, 20.0, 30.0};
    std::memcpy(page.data() + 1372, values, 24);

    // Trailer (22 字节)
    std::vector<uint8_t> trailer(22, 0);
    trailer[0] = 8;        // value_size = 8
    trailer[1] = 0x40;     // flags: is_numeric
    // magic1 = 1
    trailer[2] = 1; trailer[3] = 0; trailer[4] = 0; trailer[5] = 0;
    // tree_depth = 1
    trailer[6] = 1; trailer[7] = 0; trailer[8] = 0; trailer[9] = 0;
    // total_value_count = 3
    trailer[10] = 3; trailer[11] = 0; trailer[12] = 0; trailer[13] = 0;

    // 写入文件
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(page.data()), page.size());
    ofs.write(reinterpret_cast<char*>(trailer.data()), trailer.size());
    return path;
}

// 构建一个最小字符串型 .atx（tree_depth=1，单叶子页面）
// 包含 3 条记录: fid=1→"apple", fid=2→"banana", fid=3→"cherry"
static std::string build_string_atx() {
    std::string path = "/tmp/test_string.atx";
    const uint8_t value_size = 20;  // 10 字符 × 2 字节 (UTF-16LE)
    std::vector<uint8_t> page(4096, 0);

    // 页面头
    page[4] = 3;  // entry_count = 3

    // mpp = (4096-12)/(4+20) = 4084/24 = 170
    int mpp = 170;

    // FID 数组: offset 12
    uint32_t fids[] = {1, 2, 3};
    std::memcpy(page.data() + 12, fids, 12);

    // 值数组: offset 12 + 170*4 = 692
    size_t values_off = 12 + mpp * 4;

    // "apple"  (UTF-16LE, 空格填充到 10 字符)
    std::u16string s1 = u"apple     ";
    std::memcpy(page.data() + values_off, s1.data(), value_size);

    // "banana    "
    std::u16string s2 = u"banana    ";
    std::memcpy(page.data() + values_off + value_size, s2.data(), value_size);

    // "cherry    "
    std::u16string s3 = u"cherry    ";
    std::memcpy(page.data() + values_off + value_size * 2, s3.data(), value_size);

    // Trailer
    std::vector<uint8_t> trailer(22, 0);
    trailer[0] = value_size;  // value_size = 20
    trailer[1] = 0x20;        // flags: is_string
    trailer[2] = 1;           // magic1 = 1
    trailer[6] = 1;           // tree_depth = 1
    trailer[10] = 3;          // total_value_count = 3

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(page.data()), page.size());
    ofs.write(reinterpret_cast<char*>(trailer.data()), trailer.size());
    return path;
}

// ── 解析测试 ──

TEST(AttributeIndexTest, ParseValidNumeric) {
    auto path = build_numeric_atx();
    GdbAttributeIndexParser parser(path);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.trailer().value_size, 8);
    EXPECT_TRUE(parser.trailer().is_numeric);
    EXPECT_FALSE(parser.trailer().is_string);
    EXPECT_EQ(parser.trailer().tree_depth, 1u);
    EXPECT_EQ(parser.all_entries().size(), 3u);
    fs::remove(path);
}

TEST(AttributeIndexTest, ParseValidString) {
    auto path = build_string_atx();
    GdbAttributeIndexParser parser(path);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.trailer().value_size, 20);
    EXPECT_TRUE(parser.trailer().is_string);
    EXPECT_EQ(parser.all_entries().size(), 3u);
    // 验证字符串值
    EXPECT_EQ(parser.all_entries()[0].string_value, "apple");
    EXPECT_EQ(parser.all_entries()[1].string_value, "banana");
    EXPECT_EQ(parser.all_entries()[2].string_value, "cherry");
    fs::remove(path);
}

TEST(AttributeIndexTest, ParseNonexistent) {
    GdbAttributeIndexParser parser("/nonexistent/path/fake.atx");
    EXPECT_FALSE(parser.parse());
}

// ── 数值查询测试（6 种运算符） ──

class NumericQueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = build_numeric_atx();
        parser_ = std::make_unique<GdbAttributeIndexParser>(path_);
        ASSERT_TRUE(parser_->parse());
    }
    void TearDown() override {
        parser_.reset();
        fs::remove(path_);
    }
    std::string path_;
    std::unique_ptr<GdbAttributeIndexParser> parser_;
};

TEST_F(NumericQueryTest, QueryDoubleEq) {
    auto fids = parser_->query_double(20.0, AttrOp::Eq);
    ASSERT_EQ(fids.size(), 1u);
    EXPECT_EQ(fids[0], 1u);  // 0-based: fid=2 → index 1
}

TEST_F(NumericQueryTest, QueryDoubleNe) {
    auto fids = parser_->query_double(20.0, AttrOp::Ne);
    EXPECT_EQ(fids.size(), 2u);  // 10.0 和 30.0
}

TEST_F(NumericQueryTest, QueryDoubleLt) {
    auto fids = parser_->query_double(20.0, AttrOp::Lt);
    ASSERT_EQ(fids.size(), 1u);
    EXPECT_EQ(fids[0], 0u);  // 10.0 → fid=1 → 0-based: 0
}

TEST_F(NumericQueryTest, QueryDoubleLe) {
    auto fids = parser_->query_double(20.0, AttrOp::Le);
    EXPECT_EQ(fids.size(), 2u);  // 10.0 和 20.0
}

TEST_F(NumericQueryTest, QueryDoubleGt) {
    auto fids = parser_->query_double(20.0, AttrOp::Gt);
    ASSERT_EQ(fids.size(), 1u);
    EXPECT_EQ(fids[0], 2u);  // 30.0 → fid=3 → 0-based: 2
}

TEST_F(NumericQueryTest, QueryDoubleGe) {
    auto fids = parser_->query_double(20.0, AttrOp::Ge);
    EXPECT_EQ(fids.size(), 2u);  // 20.0 和 30.0
}

// ── 字符串查询测试 ──

class StringQueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        path_ = build_string_atx();
        parser_ = std::make_unique<GdbAttributeIndexParser>(path_);
        ASSERT_TRUE(parser_->parse());
    }
    void TearDown() override {
        parser_.reset();
        fs::remove(path_);
    }
    std::string path_;
    std::unique_ptr<GdbAttributeIndexParser> parser_;
};

TEST_F(StringQueryTest, QueryStringEq) {
    auto fids = parser_->query_string("banana", AttrOp::Eq);
    ASSERT_EQ(fids.size(), 1u);
    EXPECT_EQ(fids[0], 1u);  // fid=2 → 0-based: 1
}

TEST_F(StringQueryTest, QueryStringRange) {
    // > "apple"  → "banana" 和 "cherry"
    auto fids = parser_->query_string("apple", AttrOp::Gt);
    EXPECT_EQ(fids.size(), 2u);
}

TEST_F(StringQueryTest, QueryStringNe) {
    // != "banana" → "apple" 和 "cherry"
    auto fids = parser_->query_string("banana", AttrOp::Ne);
    EXPECT_EQ(fids.size(), 2u);
}

TEST_F(StringQueryTest, QueryStringLe) {
    // <= "banana" → "apple" 和 "banana"
    auto fids = parser_->query_string("banana", AttrOp::Le);
    EXPECT_EQ(fids.size(), 2u);
}

TEST_F(StringQueryTest, QueryStringGe) {
    // >= "banana" → "banana" 和 "cherry"
    auto fids = parser_->query_string("banana", AttrOp::Ge);
    EXPECT_EQ(fids.size(), 2u);
}

TEST_F(StringQueryTest, QueryStringLt) {
    // < "cherry" → "apple" 和 "banana"
    auto fids = parser_->query_string("cherry", AttrOp::Lt);
    EXPECT_EQ(fids.size(), 2u);
}

// ── 数值类型变体测试 ──

// 构建 value_size=2 (INT16) 的 .atx
static std::string build_int16_atx() {
    std::string path = "/tmp/test_int16.atx";
    std::vector<uint8_t> page(4096, 0);
    page[4] = 3;  // entry_count = 3

    // mpp = (4096-12)/(4+2) = 680
    int mpp = 680;

    // FID 数组
    uint32_t fids[] = {1, 2, 3};
    std::memcpy(page.data() + 12, fids, 12);

    // 值数组: offset 12 + 680*4 = 2732, 3 × 2 字节 (int16 LE)
    size_t values_off = 12 + mpp * 4;
    // 100 → 0x0064
    page[values_off + 0] = 0x64; page[values_off + 1] = 0x00;
    // 200 → 0x00C8
    page[values_off + 2] = 0xC8; page[values_off + 3] = 0x00;
    // 300 → 0x012C
    page[values_off + 4] = 0x2C; page[values_off + 5] = 0x01;

    // Trailer
    std::vector<uint8_t> trailer(22, 0);
    trailer[0] = 2;        // value_size = 2
    trailer[1] = 0x40;     // is_numeric
    trailer[2] = 1;        // magic1 = 1
    trailer[6] = 1;        // tree_depth = 1
    trailer[10] = 3;       // total_value_count = 3

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(page.data()), page.size());
    ofs.write(reinterpret_cast<char*>(trailer.data()), trailer.size());
    return path;
}

// 构建 value_size=4 (INT32) 的 .atx
static std::string build_int32_atx() {
    std::string path = "/tmp/test_int32.atx";
    std::vector<uint8_t> page(4096, 0);
    page[4] = 3;  // entry_count = 3

    // mpp = (4096-12)/(4+4) = 510
    int mpp = 510;

    uint32_t fids[] = {1, 2, 3};
    std::memcpy(page.data() + 12, fids, 12);

    // 值数组: offset 12 + 510*4 = 2052, 3 × 4 字节
    size_t values_off = 12 + mpp * 4;
    int32_t vals[] = {1000, 2000, 3000};
    std::memcpy(page.data() + values_off, vals, 12);

    std::vector<uint8_t> trailer(22, 0);
    trailer[0] = 4;        // value_size = 4
    trailer[1] = 0x40;     // is_numeric
    trailer[2] = 1;        // magic1 = 1
    trailer[6] = 1;        // tree_depth = 1
    trailer[10] = 3;       // total_value_count = 3

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(page.data()), page.size());
    ofs.write(reinterpret_cast<char*>(trailer.data()), trailer.size());
    return path;
}

// 构建 value_size=3 (未知宽度，走 default 分支) 的 .atx
static std::string build_unknown_size_atx() {
    std::string path = "/tmp/test_unknown.atx";
    std::vector<uint8_t> page(4096, 0);
    page[4] = 2;  // entry_count = 2

    // mpp = (4096-12)/(4+3) = 583

    uint32_t fids[] = {1, 2};
    std::memcpy(page.data() + 12, fids, 8);

    // 值数组: offset 12 + 583*4 = 2344
    // (值内容不重要，default 分支会返回 0)

    std::vector<uint8_t> trailer(22, 0);
    trailer[0] = 3;        // value_size = 3 (不常见)
    trailer[1] = 0x40;     // is_numeric
    trailer[2] = 1;        // magic1 = 1
    trailer[6] = 1;        // tree_depth = 1
    trailer[10] = 2;       // total_value_count = 2

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(page.data()), page.size());
    ofs.write(reinterpret_cast<char*>(trailer.data()), trailer.size());
    return path;
}

TEST(AttributeIndexTest, DecodeNumericInt16) {
    auto path = build_int16_atx();
    GdbAttributeIndexParser parser(path);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.all_entries().size(), 3u);
    // value_size=2 解码为 double(int16)
    EXPECT_DOUBLE_EQ(parser.all_entries()[0].numeric_value, 100.0);
    EXPECT_DOUBLE_EQ(parser.all_entries()[1].numeric_value, 200.0);
    EXPECT_DOUBLE_EQ(parser.all_entries()[2].numeric_value, 300.0);

    // 查询验证
    auto fids = parser.query_double(200.0, AttrOp::Eq);
    ASSERT_EQ(fids.size(), 1u);
    EXPECT_EQ(fids[0], 1u);
    fs::remove(path);
}

TEST(AttributeIndexTest, DecodeNumericInt32) {
    auto path = build_int32_atx();
    GdbAttributeIndexParser parser(path);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.all_entries().size(), 3u);
    EXPECT_DOUBLE_EQ(parser.all_entries()[0].numeric_value, 1000.0);
    EXPECT_DOUBLE_EQ(parser.all_entries()[1].numeric_value, 2000.0);
    EXPECT_DOUBLE_EQ(parser.all_entries()[2].numeric_value, 3000.0);
    fs::remove(path);
}

TEST(AttributeIndexTest, DecodeNumericUnknownSize) {
    auto path = build_unknown_size_atx();
    GdbAttributeIndexParser parser(path);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.all_entries().size(), 2u);
    // value_size=3 走 default 分支 → numeric_value = 0
    EXPECT_DOUBLE_EQ(parser.all_entries()[0].numeric_value, 0.0);
    EXPECT_DOUBLE_EQ(parser.all_entries()[1].numeric_value, 0.0);
    fs::remove(path);
}

// ── Unicode 字符串测试 ──

// 构建包含 CJK 字符的字符串 .atx
static std::string build_unicode_string_atx() {
    std::string path = "/tmp/test_unicode.atx";
    const uint8_t value_size = 20;  // 10 字符 × 2 字节
    std::vector<uint8_t> page(4096, 0);
    page[4] = 2;  // entry_count = 2

    int mpp = 170;  // (4096-12)/(4+20) = 170

    uint32_t fids[] = {1, 2};
    std::memcpy(page.data() + 12, fids, 8);

    size_t values_off = 12 + mpp * 4;

    // "中" = U+4E2D → UTF-16LE: 0x2D 0x4E, 然后空格填充
    // fid=1: "中文" → U+4E2D U+6587
    std::vector<uint8_t> val1(value_size, 0);
    val1[0] = 0x2D; val1[1] = 0x4E;  // 中
    val1[2] = 0x87; val1[3] = 0x65;  // 文
    // 剩余为 0x0000 (null terminator)
    std::memcpy(page.data() + values_off, val1.data(), value_size);

    // fid=2: "日" → U+65E5
    std::vector<uint8_t> val2(value_size, 0);
    val2[0] = 0xE5; val2[1] = 0x65;  // 日
    std::memcpy(page.data() + values_off + value_size, val2.data(), value_size);

    std::vector<uint8_t> trailer(22, 0);
    trailer[0] = value_size;
    trailer[1] = 0x20;     // is_string
    trailer[2] = 1;        // magic1 = 1
    trailer[6] = 1;        // tree_depth = 1
    trailer[10] = 2;       // total_value_count = 2

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(page.data()), page.size());
    ofs.write(reinterpret_cast<char*>(trailer.data()), trailer.size());
    return path;
}

TEST(AttributeIndexTest, DecodeStringUnicode) {
    auto path = build_unicode_string_atx();
    GdbAttributeIndexParser parser(path);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.all_entries().size(), 2u);
    // "中文" 应该被正确解码为 UTF-8
    EXPECT_EQ(parser.all_entries()[0].string_value, "中文");
    // "日" 应该被正确解码
    EXPECT_EQ(parser.all_entries()[1].string_value, "日");

    // 字符串查询
    auto fids = parser.query_string("中文", AttrOp::Eq);
    ASSERT_EQ(fids.size(), 1u);
    EXPECT_EQ(fids[0], 0u);
    fs::remove(path);
}

// ── 多层 B+ 树测试（tree_depth=2）──

// 构建 tree_depth=2 的数值 .atx（1 个非叶子页 + 1 个叶子页）
static std::string build_multilevel_atx() {
    std::string path = "/tmp/test_multilevel.atx";

    // Page 1 (page_id=1): 非叶子页面（内部节点）
    std::vector<uint8_t> nonleaf(4096, 0);
    // next_page_id = 0 (offset 0)
    // n_subpages = 1 (offset 4)
    nonleaf[4] = 1;  // n_subpages = 1
    // 子页面 ID 数组: offset 8 (紧跟 next_page_id + n_subpages)
    uint32_t child_id = 2;  // 指向 page_id=2
    std::memcpy(nonleaf.data() + 8, &child_id, 4);
    // 分隔值: offset 12 + mpp*4 (mpp for value_size=8: (4096-12)/12 = 340)
    // 跳过分隔值 (8 字节)

    // Page 2 (page_id=2): 叶子页面
    std::vector<uint8_t> leaf(4096, 0);
    // next_page_id = 0
    leaf[4] = 2;  // entry_count = 2
    // FID 数组
    uint32_t fids[] = {1, 2};
    std::memcpy(leaf.data() + 12, fids, 8);
    // 值数组: offset 12 + 340*4 = 1372
    double values[] = {10.0, 20.0};
    std::memcpy(leaf.data() + 1372, values, 16);

    // Trailer (22 字节)
    std::vector<uint8_t> trailer(22, 0);
    trailer[0] = 8;        // value_size = 8
    trailer[1] = 0x40;     // is_numeric
    trailer[2] = 1;        // magic1 = 1
    trailer[6] = 2;        // tree_depth = 2 (关键！)
    trailer[10] = 2;       // total_value_count = 2

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(nonleaf.data()), nonleaf.size());
    ofs.write(reinterpret_cast<char*>(leaf.data()), leaf.size());
    ofs.write(reinterpret_cast<char*>(trailer.data()), trailer.size());
    return path;
}

TEST(AttributeIndexTest, ParseMultiLevelTree) {
    auto path = build_multilevel_atx();
    GdbAttributeIndexParser parser(path);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.trailer().tree_depth, 2u);
    // 应该遍历到叶子页面并获取 2 条记录
    EXPECT_EQ(parser.all_entries().size(), 2u);
    EXPECT_DOUBLE_EQ(parser.all_entries()[0].numeric_value, 10.0);
    EXPECT_DOUBLE_EQ(parser.all_entries()[1].numeric_value, 20.0);

    // 查询验证
    auto fids = parser.query_double(10.0, AttrOp::Eq);
    ASSERT_EQ(fids.size(), 1u);
    EXPECT_EQ(fids[0], 0u);
    fs::remove(path);
}

// ── 错误路径测试 ──

TEST(AttributeIndexTest, ParseTruncatedFile) {
    std::string path = "/tmp/test_truncated.atx";
    // 写入少于 kTrailerSize (22) 字节
    std::vector<uint8_t> data(10, 0);
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(data.data()), data.size());
    ofs.close();

    GdbAttributeIndexParser parser(path);
    EXPECT_FALSE(parser.parse());
    fs::remove(path);
}

TEST(AttributeIndexTest, ParseBadMagic) {
    std::string path = "/tmp/test_badmagic.atx";
    std::vector<uint8_t> page(4096, 0);
    // Trailer with bad magic
    std::vector<uint8_t> trailer(22, 0);
    trailer[0] = 8;        // value_size = 8
    trailer[1] = 0x40;     // is_numeric
    trailer[2] = 99;       // magic1 = 99 (错误! 应该是 1)
    trailer[6] = 1;        // tree_depth = 1
    trailer[10] = 0;

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(page.data()), page.size());
    ofs.write(reinterpret_cast<char*>(trailer.data()), trailer.size());
    ofs.close();

    GdbAttributeIndexParser parser(path);
    EXPECT_FALSE(parser.parse());
    fs::remove(path);
}
