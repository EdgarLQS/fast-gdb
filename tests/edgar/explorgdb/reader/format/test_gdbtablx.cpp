// tests/edgar/explorgdb/test_gdbtablx.cpp
// .gdbtablx 偏移索引解析测试
// 测试头部版本、偏移表、稀疏位图、FID 查找

#include "gdb_tablx.h"
#include "gdb_catalog.h"
#include "gdb_table.h"
#include "test_paths.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <gtest/gtest.h>

using namespace explorgdb;

static const auto SPX_GDB_PATH =
    explorgdb_test_paths::test_data_path("test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb");
static const auto V4_DENSE_GDB_PATH =
    explorgdb_test_paths::test_data_path(
        "test_data/gdb/synthetic_v4_dense_gdal_reference.gdb");

// ── 头部解析测试 ──

// 当前 ArcGIS 真实夹具使用 v3；密集 v4 GDAL 参考覆盖无位图布局，
// ArcGIS 稀疏/高位 64 位 ObjectID 映射仍待真实样本验收。
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GdbTablxTest, HeaderVersion) {
    std::string tablx_path = SPX_GDB_PATH.string();
    tablx_path += "/a00000001.gdbtablx";

    GdbTablxParser parser(tablx_path);
    ASSERT_TRUE(parser.parse());

    // 当前真实夹具应为受支持的版本 3。
    uint32_t ver = parser.header().version;
    EXPECT_EQ(ver, 3u);
    // size_tablx_offsets 应该是 4、5 或 6
    uint32_t offset_size = parser.header().size_tablx_offsets;
    EXPECT_TRUE(offset_size == 4 || offset_size == 5 || offset_size == 6);
}

// 测试无效文件返回 false
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GdbTablxTest, InvalidFile) {
    GdbTablxParser parser("/nonexistent/a00000001.gdbtablx");
    EXPECT_FALSE(parser.parse());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GdbTablxTest, RejectsTruncatedOrInvalidOffsetTables) {
    const std::string truncated_path = "/tmp/test_truncated_table.gdbtablx";
    const std::string invalid_width_path = "/tmp/test_invalid_width.gdbtablx";
    auto write_u32 = [](std::ofstream& stream, uint32_t value) {
        const uint8_t bytes[] = {
            static_cast<uint8_t>(value & 0xff),
            static_cast<uint8_t>((value >> 8) & 0xff),
            static_cast<uint8_t>((value >> 16) & 0xff),
            static_cast<uint8_t>((value >> 24) & 0xff)};
        stream.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    };

    {
        std::ofstream stream(truncated_path, std::ios::binary);
        write_u32(stream, 3);
        write_u32(stream, 1);
        write_u32(stream, 1024);
        write_u32(stream, 4);
    }
    {
        std::ofstream stream(invalid_width_path, std::ios::binary);
        write_u32(stream, 3);
        write_u32(stream, 0);
        write_u32(stream, 0);
        write_u32(stream, 7);
    }

    EXPECT_FALSE(GdbTablxParser(truncated_path).parse());
    EXPECT_FALSE(GdbTablxParser(invalid_width_path).parse());
    std::filesystem::remove(truncated_path);
    std::filesystem::remove(invalid_width_path);
}

TEST(GdbTablxTest, OpenSnapshotRejectsInPlaceRewrite) {
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() /
        ("test_tablx_snapshot_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".gdbtablx");
    {
        std::ofstream stream(path, std::ios::binary);
        const uint8_t data[] = {
            3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        stream.write(reinterpret_cast<const char*>(data), sizeof(data));
        ASSERT_TRUE(stream.good());
    }

    GdbTablxParser parser(path.string());
    ASSERT_TRUE(parser.open_snapshot());
    ASSERT_TRUE(parser.parse_opened());
    ASSERT_TRUE(parser.source_is_current());
    const auto original_mtime = fs::last_write_time(path);
    // Ensure the in-place rewrite crosses the filesystem timestamp quantum.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(stream.is_open());
        const uint8_t replacement[] = {9, 8, 7, 6};
        stream.seekp(24);
        stream.write(reinterpret_cast<const char*>(replacement), sizeof(replacement));
        ASSERT_TRUE(stream.good());
    }
    fs::last_write_time(path, original_mtime);
    EXPECT_FALSE(parser.source_is_current());
    EXPECT_FALSE(parser.parse_opened());
    fs::remove(path);
}

// ── 偏移表测试 ──

// 测试偏移表条目数
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GdbTablxTest, IsBlockActive) {
    std::string tablx_path = SPX_GDB_PATH.string();
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
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

TEST(GdbTablxTest, CountsWithoutMaterializingValidFids) {
    const auto path = V4_DENSE_GDB_PATH / "a00000009.gdbtablx";
    GdbTablxParser parser(path.string());
    ASSERT_TRUE(parser.open_snapshot());
    ASSERT_TRUE(parser.parse_opened(false));
    EXPECT_EQ(parser.feature_count(), 4u);
    EXPECT_TRUE(parser.valid_fids().empty());
    EXPECT_NE(parser.get_offset(0), 0u);
    EXPECT_NE(parser.get_offset(3), 0u);
}

// ── 合成文件测试（覆盖 v4、6字节偏移、错误路径）──

namespace fs = std::filesystem;

static void append_u32(std::vector<uint8_t>& data, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        data.push_back(static_cast<uint8_t>((value >> shift) & 0xffU));
}

static void append_u64(std::vector<uint8_t>& data, uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        data.push_back(static_cast<uint8_t>((value >> shift) & 0xffU));
}

static void set_u32_at(std::vector<uint8_t>& data, size_t offset,
                       uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        data[offset + shift / 8] =
            static_cast<uint8_t>((value >> shift) & 0xffU);
}

static std::string write_synthetic_tablx(
    const std::string& filename, const std::vector<uint8_t>& data) {
    const auto path = fs::temp_directory_path() / filename;
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    return path.string();
}

// 合成带未知位图布局的 v4 .gdbtablx，验证解析器不会猜测稀疏 ID 映射。
static std::string build_v4_unknown_bitmap_tablx() {
    const auto path = fs::temp_directory_path() / "test_v4_unknown_bitmap.gdbtablx";
    std::vector<uint8_t> data;

    auto push_u32 = [&](uint32_t v) {
        data.push_back(v & 0xFF);
        data.push_back((v >> 8) & 0xFF);
        data.push_back((v >> 16) & 0xFF);
        data.push_back((v >> 24) & 0xFF);
    };
    auto push_u64 = [&](uint64_t v) {
        for (int i = 0; i < 8; i++) data.push_back((v >> (i*8)) & 0xFF);
    };

    push_u32(4);
    push_u64(1);     // 一个物理 1024 行块
    push_u32(4);     // 4 字节偏移
    data.resize(16 + 1024 * 4, 0);
    data[16] = 1;    // 一个活动记录
    push_u64(1);     // 总逻辑记录数
    push_u32(8);     // 非零、当前不支持的 bitmap_section_size
    data.resize((data.size() + 15U) & ~size_t{15U}, 0);

    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<char*>(data.data()), data.size());
    return path.string();
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GdbTablxTest, V4DenseGdalReferenceParses) {
    const auto path = V4_DENSE_GDB_PATH / "a00000009.gdbtablx";
    GdbTablxParser parser(path.string());
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.header().version, 4u);
    EXPECT_EQ(parser.header().n1024blocks_v4, 1u);
    EXPECT_EQ(parser.header().n_total_record_count_v4, 4u);
    EXPECT_EQ(parser.header().bitmap_section_size_v4, 0u);
    ASSERT_EQ(parser.offsets().size(), 1024u);
    ASSERT_EQ(parser.valid_fids().size(), 4u);
    for (uint32_t row_slot = 0; row_slot < 4; ++row_slot) {
        EXPECT_EQ(parser.valid_fids()[row_slot], row_slot);
        EXPECT_NE(parser.get_offset(row_slot), 0u);
    }
    EXPECT_EQ(parser.get_offset(4), 0u);
}

TEST(GdbTablxTest, V4UnknownBitmapFailsClosed) {
    const auto path = build_v4_unknown_bitmap_tablx();
    GdbTablxParser parser(path);
    EXPECT_FALSE(parser.parse());
    EXPECT_EQ(parser.header().version, 0u);
    EXPECT_TRUE(parser.offsets().empty());
    EXPECT_EQ(parser.feature_count(), 0u);
    fs::remove(path);
}

TEST(GdbTablxTest, V3CompressedBlockMapResolvesLogicalRows) {
    std::vector<uint8_t> data;
    append_u32(data, 3);
    append_u32(data, 2);     // 两个物理块
    append_u32(data, 2050);  // 逻辑行槽覆盖到第三个块
    append_u32(data, 4);
    data.resize(16 + 2048 * 4, 0);
    set_u32_at(data, 16, 100);
    set_u32_at(data, 16 + 1023 * 4, 200);
    set_u32_at(data, 16 + 1024 * 4, 300);
    set_u32_at(data, 16 + 1025 * 4, 400);
    append_u32(data, 1);  // bitmap word count
    append_u32(data, 3);  // three logical blocks
    append_u32(data, 2);  // two physical blocks
    append_u32(data, 2);
    data.push_back(0b00000101);  // logical blocks 0 and 2 are present
    const auto path = write_synthetic_tablx(
        "test_v3_compressed_block_map.gdbtablx", data);

    GdbTablxParser parser(path);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.logical_row_count(), 2050u);
    EXPECT_EQ(parser.get_offset(0), 100u);
    EXPECT_EQ(parser.get_offset(1023), 200u);
    EXPECT_EQ(parser.get_offset(1024), 0u);
    EXPECT_EQ(parser.get_offset(2048), 300u);
    EXPECT_EQ(parser.get_offset(2049), 400u);
    EXPECT_EQ(parser.get_offset(2050), 0u);
    EXPECT_TRUE(parser.is_block_active(0));
    EXPECT_FALSE(parser.is_block_active(1));
    EXPECT_TRUE(parser.is_block_active(2));
    EXPECT_FALSE(parser.is_block_active(3));
    EXPECT_EQ(parser.valid_fids(),
              (std::vector<RowSlot>{0, 1023, 2048, 2049}));

    GdbTableParser table(
        (SPX_GDB_PATH / "a00000001.gdbtable").string());
    ASSERT_TRUE(table.load_tablx(path));
    EXPECT_EQ(table.feature_count(), 2050u);
    EXPECT_EQ(table.active_feature_count(), 4u);
    EXPECT_FALSE(table.active_feature_fids_are_dense());
    EXPECT_EQ(table.active_feature_fid_at(0), 0u);
    EXPECT_EQ(table.active_feature_fid_at(1), 1023u);
    EXPECT_EQ(table.active_feature_fid_at(2), 2048u);
    EXPECT_EQ(table.active_feature_fid_at(3), 2049u);
    EXPECT_EQ(table.feature_offset(1024), 0u);
    EXPECT_EQ(table.feature_offset(2048), 300u);
    EXPECT_TRUE(table.has_feature(2049));

    GdbTableParser cached_table(
        (SPX_GDB_PATH / "a00000001.gdbtable").string());
    ASSERT_TRUE(cached_table.load_tablx(path));
    EXPECT_EQ(cached_table.feature_offset(2048), 300u);
    fs::remove(path);
}

TEST(GdbTablxTest, V4RecognizedBitmapResolvesLogicalRows) {
    constexpr size_t kBitmapBytes = 32768;
    constexpr uint32_t kBitmapSectionSize = 22 + kBitmapBytes + 52;
    std::vector<uint8_t> data;
    append_u32(data, 4);
    append_u64(data, 2);  // two physical blocks
    append_u32(data, 4);
    data.resize(16 + 2048 * 4, 0);
    set_u32_at(data, 16, 100);
    set_u32_at(data, 16 + 1023 * 4, 200);
    set_u32_at(data, 16 + 1024 * 4, 300);
    set_u32_at(data, 16 + 1025 * 4, 400);
    append_u64(data, 2050);
    append_u32(data, kBitmapSectionSize);
    const size_t section_offset = data.size();
    data.resize(section_offset + kBitmapSectionSize, 0);
    data[section_offset] = 1;
    data[section_offset + 2] = 1;
    data[section_offset + 22] = 0b00000101;
    data[section_offset + 22 + kBitmapBytes] = 1;
    const size_t padded_size = (data.size() + 15U) & ~size_t{15U};
    data.resize(padded_size, 0);
    const auto path = write_synthetic_tablx(
        "test_v4_recognized_bitmap.gdbtablx", data);

    GdbTablxParser parser(path);
    ASSERT_TRUE(parser.parse());
    EXPECT_EQ(parser.header().bitmap_section_size_v4, kBitmapSectionSize);
    EXPECT_EQ(parser.logical_row_count(), 2050u);
    EXPECT_EQ(parser.get_offset(0), 100u);
    EXPECT_EQ(parser.get_offset(1023), 200u);
    EXPECT_EQ(parser.get_offset(1024), 0u);
    EXPECT_EQ(parser.get_offset(2048), 300u);
    EXPECT_EQ(parser.get_offset(2049), 400u);
    EXPECT_EQ(parser.get_offset(2050), 0u);
    EXPECT_EQ(parser.valid_fids(),
              (std::vector<RowSlot>{0, 1023, 2048, 2049}));

    GdbTableParser table(
        (V4_DENSE_GDB_PATH / "a00000009.gdbtable").string());
    ASSERT_TRUE(table.load_tablx(path));
    EXPECT_EQ(table.feature_count(), 2050u);
    EXPECT_EQ(table.active_feature_count(), 4u);
    EXPECT_EQ(table.feature_offset(1024), 0u);
    EXPECT_EQ(table.feature_offset(2048), 300u);
    EXPECT_TRUE(table.has_feature(2049));
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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
