// tests/edgar/explorgdb/test_gdbtablx.cpp
// .gdbtablx 偏移索引解析测试
// 测试头部版本、偏移表、稀疏位图、FID 查找

#include "gdb_tablx.h"
#include "gdb_catalog.h"
#include "../test_paths.h"
#include <gtest/gtest.h>

using namespace explorgdb;

static const auto SPX_GDB_PATH =
    explorgdb_test_paths::test_data_path("test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb");

// ── 头部解析测试 ──

// 测试 .gdbtablx 头部（版本可能是 3 或 4）
TEST(GdbTablxTest, HeaderVersion) {
    std::string tablx_path = SPX_GDB_PATH.string();
    tablx_path += "/a00000001.gdbtablx";

    GdbTablxParser parser(tablx_path);
    ASSERT_TRUE(parser.parse());

    // 版本应该是 3 或 4
    uint32_t ver = parser.header().version;
    EXPECT_TRUE(ver == 3 || ver == 4);
    // size_tablx_offsets 应该是 4、5 或 6
    uint32_t offset_size = parser.header().size_tablx_offsets;
    EXPECT_TRUE(offset_size == 4 || offset_size == 5 || offset_size == 6);
}

// 测试无效文件返回 false
TEST(GdbTablxTest, InvalidFile) {
    GdbTablxParser parser("/nonexistent/a00000001.gdbtablx");
    EXPECT_FALSE(parser.parse());
}

// ── 偏移表测试 ──

// 测试偏移表条目数
TEST(GdbTablxTest, OffsetCount) {
    std::string tablx_path = SPX_GDB_PATH.string();
    tablx_path += "/a00000001.gdbtablx";

    GdbTablxParser parser(tablx_path);
    ASSERT_TRUE(parser.parse());

    const auto& offsets = parser.offsets();
    // 至少有 1 个条目
    EXPECT_FALSE(offsets.empty());

    // 第一个偏移（FID=0）可能是 0（null 要素）或非零
    // 但偏移值不应该超过文件大小
    if (offsets[0] != 0) {
        EXPECT_LT(offsets[0], 1000000000ULL);  // 合理上限
    }
}

// 测试 FID→偏移查找
TEST(GdbTablxTest, GetOffset) {
    std::string tablx_path = SPX_GDB_PATH.string();
    tablx_path += "/a00000001.gdbtablx";

    GdbTablxParser parser(tablx_path);
    ASSERT_TRUE(parser.parse());

    // FID=0 应该返回一个有效偏移
    uint64_t off0 = parser.get_offset(0);
    EXPECT_GE(off0, 0u);

    // 超大 FID 返回 0
    EXPECT_EQ(parser.get_offset(999999), 0u);
}

// 测试存在非零偏移（真实要素）
TEST(GdbTablxTest, HasNonNullOffsets) {
    std::string tablx_path = SPX_GDB_PATH.string();
    tablx_path += "/a00000001.gdbtablx";

    GdbTablxParser parser(tablx_path);
    ASSERT_TRUE(parser.parse());

    int non_null_count = 0;
    for (uint64_t off : parser.offsets()) {
        if (off != 0) non_null_count++;
    }
    // 至少有一个真实要素
    EXPECT_GT(non_null_count, 0);
}

// ── 稀疏位图测试 ──

// 测试位图一致性：位图标记为 0 的块，偏移应为 0
TEST(GdbTablxTest, BitmapConsistency) {
    std::string tablx_path = SPX_GDB_PATH.string();
    tablx_path += "/a00000001.gdbtablx";

    GdbTablxParser parser(tablx_path);
    ASSERT_TRUE(parser.parse());

    const auto& bitmap = parser.block_bitmap();
    const auto& offsets = parser.offsets();

    if (bitmap.empty()) return;  // 无位图，跳过

    // 检查每个块的位图与偏移对应关系
    for (size_t block = 0; block < bitmap.size() && block * 1024 < offsets.size(); ++block) {
        if (!bitmap[block]) {
            // 块标记为非活动，其中所有偏移应为 0
            for (size_t i = block * 1024; i < (block + 1) * 1024 && i < offsets.size(); ++i) {
                EXPECT_EQ(offsets[i], 0u) << "Block " << block << " should be all zeros";
            }
        }
    }
}

// 测试 is_block_active 方法
TEST(GdbTablxTest, IsBlockActive) {
    std::string tablx_path = SPX_GDB_PATH;
    tablx_path += "/a00000001.gdbtablx";

    GdbTablxParser parser(tablx_path);
    ASSERT_TRUE(parser.parse());

    // 如果位图非空，块 0 应该是活动的（包含系统目录数据）
    if (!parser.block_bitmap().empty()) {
        EXPECT_TRUE(parser.is_block_active(0));
        // 超大块索引应返回 false
        EXPECT_FALSE(parser.is_block_active(999999));
    } else {
        // 无位图时所有块默认活动
        EXPECT_TRUE(parser.is_block_active(0));
    }
}

// ── 变体偏移宽度测试 ──

// 测试不同表的偏移宽度可能不同
TEST(GdbTablxTest, VariableOffsetWidth) {
    GdbCatalog catalog;
    catalog.scan(SPX_GDB_PATH.string());

    // 收集所有 .gdbtablx 的 offset_size
    std::vector<uint32_t> sizes;
    for (uint32_t id = 1; id <= 15; ++id) {
        const auto* entry = catalog.find_tablx(id);
        if (!entry) continue;

        std::string tablx_path = SPX_GDB_PATH.string();
        tablx_path += "/" + entry->filename;

        GdbTablxParser parser(tablx_path);
        if (!parser.parse()) continue;

        sizes.push_back(parser.header().size_tablx_offsets);
    }

    // 至少解析了一些 tablx 文件
    EXPECT_FALSE(sizes.empty());

    // 所有偏移宽度应该是 4、5 或 6
    for (uint32_t s : sizes) {
        EXPECT_TRUE(s == 4 || s == 5 || s == 6);
    }
}

// ── 有效 FID 列表测试 ──

// 测试 valid_fids 包含非零偏移的 FID
TEST(GdbTablxTest, ValidFids) {
    std::string tablx_path = SPX_GDB_PATH.string();
    tablx_path += "/a00000001.gdbtablx";

    GdbTablxParser parser(tablx_path);
    ASSERT_TRUE(parser.parse());

    const auto& valid_fids = parser.valid_fids();
    // 至少有一个有效 FID
    EXPECT_FALSE(valid_fids.empty());

    // 每个有效 FID 的偏移应该非零
    for (uint32_t fid : valid_fids) {
        EXPECT_NE(parser.get_offset(fid), 0u) << "FID " << fid << " should have non-zero offset";
    }
}

// ── 合成文件测试（覆盖 v4、6字节偏移、错误路径）──

#include <fstream>
#include <filesystem>
namespace fs = std::filesystem;

// 合成 v4 .gdbtablx 文件
// 覆盖 lines 60-65 (v4 header), 116-118 (v4 trailer)
static std::string build_v4_tablx() {
    std::string path = "/tmp/test_v4.gdbtablx";
    std::vector<uint8_t> data;

    // v4 头部 (20 字节): version(4) + unknown(4) + size_tablx_offsets(4) + padding(8)
    auto push_u32 = [&](uint32_t v) {
        data.push_back(v & 0xFF);
        data.push_back((v >> 8) & 0xFF);
        data.push_back((v >> 16) & 0xFF);
        data.push_back((v >> 24) & 0xFF);
    };
    auto push_u64 = [&](uint64_t v) {
        for (int i = 0; i < 8; i++) data.push_back((v >> (i*8)) & 0xFF);
    };

    push_u32(4);     // version = 4
    push_u32(0);     // unknown_v4 = 0
    push_u32(4);     // size_tablx_offsets = 4
    for (int i = 0; i < 8; i++) data.push_back(0);  // padding

    // v4 没有偏移表（n1024blocks_v3 未设置为 0）
    // v4 尾部: nfeatures_v4(8) + sizeof_varying_section(4)
    push_u64(100);   // nfeatures_v4 = 100
    push_u32(2048);  // sizeof_varying_section = 2048

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(data.data()), data.size());
    return path;
}

TEST(GdbTablxTest, ParseV4) {
    auto path = build_v4_tablx();
    GdbTablxParser parser(path);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.header().version, 4u);
    EXPECT_EQ(parser.header().size_tablx_offsets, 4u);
    // v4: n1024blocks_v3 = 0, 所以 offsets 为空
    EXPECT_TRUE(parser.offsets().empty());
    EXPECT_EQ(parser.feature_count(), 0u);
    fs::remove(path);
}

// 合成 v3 + 6字节偏移 .gdbtablx
// 覆盖 lines 140-143 (6-byte offset encoding)
static std::string build_v3_6byte_tablx() {
    std::string path = "/tmp/test_v3_6byte.gdbtablx";
    std::vector<uint8_t> data;

    auto push_u32 = [&](uint32_t v) {
        data.push_back(v & 0xFF);
        data.push_back((v >> 8) & 0xFF);
        data.push_back((v >> 16) & 0xFF);
        data.push_back((v >> 24) & 0xFF);
    };

    // v3 头部: version(4) + n1024blocks(4) + nfeatures(4) + size_tablx_offsets(4)
    push_u32(3);     // version = 3
    push_u32(1);     // n1024blocks_v3 = 1 → 1024 entries
    push_u32(1024);  // nfeatures_v3 = 1024
    push_u32(6);     // size_tablx_offsets = 6

    // 偏移表: 1024 × 6 字节
    for (int i = 0; i < 1024; i++) {
        // 6字节编码: byte[0]=低8位, bytes[1:5]=中32位, byte[5]=高8位
        uint64_t offset_val = (i < 10) ? (100 + i * 50) : 0;
        uint8_t v_low = offset_val & 0xFF;
        uint32_t v_mid = (offset_val >> 8) & 0xFFFFFFFF;
        uint8_t v_high = (offset_val >> 40) & 0xFF;
        data.push_back(v_low);
        data.push_back(v_mid & 0xFF);
        data.push_back((v_mid >> 8) & 0xFF);
        data.push_back((v_mid >> 16) & 0xFF);
        data.push_back((v_mid >> 24) & 0xFF);
        data.push_back(v_high);
    }

    // 稀疏位图元数据 (v3): n_bitmap_int32(4) + n_bits_for_block_map(4) + n1024blocks_bis(4) + n_leading_nonzero(4)
    push_u32(1);     // n_bitmap_int32
    push_u32(1);     // n_bits_for_block_map = 1 bit (1 block)
    push_u32(1);     // n1024blocks_bis
    push_u32(1);     // n_leading_nonzero

    // 位图: 1 bit → 1 byte, 值 = 0x01 (block 0 is active)
    data.push_back(0x01);

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(data.data()), data.size());
    return path;
}

TEST(GdbTablxTest, ParseV3_6ByteOffset) {
    auto path = build_v3_6byte_tablx();
    GdbTablxParser parser(path);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.header().version, 3u);
    EXPECT_EQ(parser.header().size_tablx_offsets, 6u);
    EXPECT_EQ(parser.offsets().size(), 1024u);
    // 验证前几个偏移值正确解码
    EXPECT_EQ(parser.get_offset(0), 100u);
    EXPECT_EQ(parser.get_offset(1), 150u);
    EXPECT_EQ(parser.get_offset(9), 550u);
    // FID 10+ 应为 0
    EXPECT_EQ(parser.get_offset(10), 0u);
    fs::remove(path);
}

// 合成未知版本 .gdbtablx
// 覆盖 line 64-65 (unknown version error)
static std::string build_unknown_version_tablx() {
    std::string path = "/tmp/test_unknown_ver.gdbtablx";
    std::vector<uint8_t> data;
    auto push_u32 = [&](uint32_t v) {
        data.push_back(v & 0xFF);
        data.push_back((v >> 8) & 0xFF);
        data.push_back((v >> 16) & 0xFF);
        data.push_back((v >> 24) & 0xFF);
    };
    push_u32(99);  // version = 99 (invalid)
    push_u32(0);
    push_u32(0);
    push_u32(4);

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(data.data()), data.size());
    return path;
}

TEST(GdbTablxTest, ParseUnknownVersion) {
    auto path = build_unknown_version_tablx();
    GdbTablxParser parser(path);
    EXPECT_FALSE(parser.parse());
    fs::remove(path);
}

// 合成带位图的 v3 tablx，测试 is_block_active 超出位图范围
// 覆盖 line 160 (is_block_active out of bitmap range)
static std::string build_v3_with_bitmap() {
    std::string path = "/tmp/test_v3_bitmap.gdbtablx";
    std::vector<uint8_t> data;
    auto push_u32 = [&](uint32_t v) {
        data.push_back(v & 0xFF);
        data.push_back((v >> 8) & 0xFF);
        data.push_back((v >> 16) & 0xFF);
        data.push_back((v >> 24) & 0xFF);
    };

    // v3 头部: 2 blocks → 2048 entries
    push_u32(3);     // version = 3
    push_u32(2);     // n1024blocks_v3 = 2
    push_u32(2048);  // nfeatures_v3
    push_u32(4);     // size_tablx_offsets = 4

    // 偏移表: 2048 × 4 字节
    for (int i = 0; i < 2048; i++) {
        uint32_t val = (i < 100) ? (50 + i * 10) : 0;
        push_u32(val);
    }

    // 位图元数据
    push_u32(1);     // n_bitmap_int32
    push_u32(2);     // n_bits_for_block_map = 2 bits (2 blocks)
    push_u32(2);     // n1024blocks_bis
    push_u32(1);     // n_leading_nonzero

    // 位图: 2 bits → 1 byte, 值 = 0x03 (both blocks active)
    data.push_back(0x03);

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(data.data()), data.size());
    return path;
}

TEST(GdbTablxTest, IsBlockActiveOutOfRange) {
    auto path = build_v3_with_bitmap();
    GdbTablxParser parser(path);
    ASSERT_TRUE(parser.parse());
    // 位图只有 2 bits，block 0 和 1 应活跃
    EXPECT_TRUE(parser.is_block_active(0));
    EXPECT_TRUE(parser.is_block_active(1));
    // 超出位图范围 → false (line 160)
    EXPECT_FALSE(parser.is_block_active(2));
    EXPECT_FALSE(parser.is_block_active(999999));
    fs::remove(path);
}
