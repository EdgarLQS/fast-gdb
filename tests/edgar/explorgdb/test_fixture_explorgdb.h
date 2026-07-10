// tests/edgar/explorgdb/test_fixture_explorgdb.h
// explorgdb 自包含测试 fixture — 不依赖外部 .gdb 文件
//
// 设计目标：
//   - 在临时目录中构造最小合法的 .gdb 二进制文件
//   - 所有测试使用已知内容的数据，结果可精确验证
//   - 测试结束自动清理临时文件
//   - 不依赖 GDAL，不依赖外部路径
//
// 构造的 GDB 结构：
//   - gdb 文件：version=5, magic=0xDEADBEEF
//   - timestamps 文件：384 字节填充数据
//   - a00000001.gdbtable：v3 头部 + 2 个字段（ObjectId + Int32 "value"）
//   - a00000001.gdbtablx：v3 头部 + 偏移表 + 稀疏位图

#ifndef TEST_FIXTURE_EXPLORGDB_H
#define TEST_FIXTURE_EXPLORGDB_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace explorgdb_test {

namespace fs = std::filesystem;

// ── 小端写辅助函数 ──

inline void write_u8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

inline void write_u16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
}

inline void write_u32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(v & 0xFF);
    buf.push_back((v >> 8) & 0xFF);
    buf.push_back((v >> 16) & 0xFF);
    buf.push_back((v >> 24) & 0xFF);
}

inline void write_i32(std::vector<uint8_t>& buf, int32_t v) {
    write_u32(buf, static_cast<uint32_t>(v));
}

inline void write_i64(std::vector<uint8_t>& buf, int64_t v) {
    uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i)
        buf.push_back((u >> (i * 8)) & 0xFF);
}

inline void write_u64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        buf.push_back((v >> (i * 8)) & 0xFF);
}

inline void write_f64(std::vector<uint8_t>& buf, double v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    for (int i = 0; i < 8; ++i)
        buf.push_back(p[i]);
}

// ── UTF-16LE 编码写入 ──

inline void write_utf16(std::vector<uint8_t>& buf, const std::string& s) {
    for (char c : s) {
        uint16_t ch = static_cast<uint16_t>(static_cast<unsigned char>(c));
        buf.push_back(ch & 0xFF);
        buf.push_back((ch >> 8) & 0xFF);
    }
}

inline void write_utf16_len(std::vector<uint8_t>& buf, const std::string& s) {
    uint8_t len = static_cast<uint8_t>(s.size());
    write_u8(buf, len);
    write_utf16(buf, s);
}

// ── Varuint 编码 ──

inline void write_varuint(std::vector<uint8_t>& buf, uint64_t v) {
    do {
        uint8_t byte = v & 0x7F;
        v >>= 7;
        if (v != 0) byte |= 0x80;
        buf.push_back(byte);
    } while (v != 0);
}

// Signed varint: bit6=sign, bits0-5=magnitude, bit7=continuation
inline void write_svarint(std::vector<uint8_t>& buf, int64_t v) {
    int64_t sign = (v < 0) ? -1 : 1;
    int64_t mag = v * sign;  // absolute value
    uint8_t first = static_cast<uint8_t>(mag & 0x3F);
    if (sign < 0) first |= 0x40;
    mag >>= 6;
    if (mag != 0) first |= 0x80;
    buf.push_back(first);
    while (mag != 0) {
        uint8_t byte = mag & 0x7F;
        mag >>= 7;
        if (mag != 0) byte |= 0x80;
        buf.push_back(byte);
    }
}

// ── 构造一个最小 .gdb 目录 ──
// 返回临时目录路径，测试结束后调用 cleanup_synthetic_gdb 清理

inline std::string create_synthetic_gdb(const std::string& dir_name = "synthetic_test.gdb") {
    std::string dir_path = fs::temp_directory_path().string() + "/" + dir_name;
    fs::create_directories(dir_path);

    // ── 1. 构造 gdb magic 文件（8 字节） ──
    {
        std::vector<uint8_t> buf;
        write_u32(buf, 5);                // version = 5 (LE: [05 00 00 00])
        // Magic: 0xDEADBEEF stored as BE bytes in real GDB files
        // LE reader interprets [DE AD BE EF] as 0xEFBEADDE
        buf.push_back(0xDE);
        buf.push_back(0xAD);
        buf.push_back(0xBE);
        buf.push_back(0xEF);
        std::ofstream ofs(dir_path + "/gdb", std::ios::binary);
        ofs.write(reinterpret_cast<char*>(buf.data()), buf.size());
    }

    // ── 2. 构造 timestamps 文件（384 字节） ──
    {
        std::vector<uint8_t> buf(384, 0xAB);  // 填充 0xAB
        std::ofstream ofs(dir_path + "/timestamps", std::ios::binary);
        ofs.write(reinterpret_cast<char*>(buf.data()), buf.size());
    }

    // ── 3. 构造 a00000001.gdbtable（v3） ──
    // 字段: ObjectId (implicit) + Int32 "value" (nullable)
    // 包含 2 条记录
    {
        std::vector<uint8_t> buf;

        // === 表头部 (v3, 40 字节) ===
        uint64_t field_desc_offset = 40;  // 字段区紧接头部
        write_u32(buf, 3);                     // version = 3
        write_u32(buf, 2);                     // nfeatures_v3 = 2
        write_u32(buf, 64);                    // largest_size_record
        write_u32(buf, 5);                     // unknown_role = 5
        write_u32(buf, 0);                     // unknown_16
        write_u32(buf, 0);                     // unknown_20
        write_u64(buf, 256);                   // file_size（近似值）
        write_u64(buf, field_desc_offset);     // field_desc_offset

        // === 字段描述符区 ===
        uint64_t record_area_offset = buf.size();  // 记录区起始位置（用于调试）
        (void)record_area_offset;

        // Section Header
        uint32_t section_version = 3;
        uint32_t geom_type_full = 0;  // 无几何图层
        uint16_t nfields = 2;  // ObjectId + Int32 "value"

        // 计算 section 长度（不含自身 4 字节）
        // Section: length(4) + section_version(4) + geom_type_full(4) + nfields(2) + fields...
        // 我们先占位长度，后面回填
        size_t section_start = buf.size();
        write_u32(buf, 0);  // 占位 length
        write_u32(buf, section_version);
        write_u32(buf, geom_type_full);
        write_u16(buf, nfields);

        // 字段 0: ObjectId
        write_utf16_len(buf, "ObjectId");  // name
        write_utf16_len(buf, "");          // alias (空)
        write_u8(buf, 6);                  // type = ObjectId
        write_u8(buf, 4);                  // width = 4
        write_u8(buf, 0);                  // flag = 0 (not nullable)

        // 字段 1: Int32 "value"
        write_utf16_len(buf, "value");     // name
        write_utf16_len(buf, "");          // alias (空)
        write_u8(buf, 1);                  // type = Int32
        write_u8(buf, 4);                  // width = 4
        write_u8(buf, 1);                  // flag = 1 (nullable)

        // 回填 section length
        size_t section_len = buf.size() - section_start - 4;
        std::memcpy(buf.data() + section_start, &section_len, 4);

        // === 记录区 ===
        // 记录 0: FID=0, value=42
        size_t rec0_offset = buf.size();
        write_u32(buf, 12);                // blob_len
        // nullable flags: value 字段 nullable, 1 bit, 值=0（非 null）
        write_u8(buf, 0);                  // nullable bitmap (1 byte, bit 0 = 0)
        // ObjectId 不存储在记录中（implicit = FID+1 = 1）
        write_u32(buf, 42);                // value = 42
        (void)rec0_offset;

        // 记录 1: FID=1, value=-7
        size_t rec1_offset = buf.size();
        write_u32(buf, 12);                // blob_len
        write_u8(buf, 0);                  // nullable bitmap
        write_u32(buf, static_cast<uint32_t>(-7));  // value = -7
        (void)rec1_offset;

        // 填充到合理大小
        while (buf.size() < 256) buf.push_back(0);

        std::ofstream ofs(dir_path + "/a00000001.gdbtable", std::ios::binary);
        ofs.write(reinterpret_cast<char*>(buf.data()), buf.size());
    }

    // ── 4. 构造 a00000001.gdbtablx（v3） ──
    {
        std::vector<uint8_t> buf;

        // 头部 (v3, 16 字节)
        write_u32(buf, 3);                     // version = 3
        write_u32(buf, 1);                     // n1024blocks_v3 = 1
        write_u32(buf, 1024);                  // nfeatures_v3 = 1024
        write_u32(buf, 4);                     // size_tablx_offsets = 4

        // 偏移表: 1024 个条目 × 4 字节
        // FID=0 → offset=48 (记录 0 在 .gdbtable 中的位置)
        // FID=1 → offset=60 (记录 1 在 .gdbtable 中的位置)
        // FID=2..1023 → offset=0 (null 要素)
        for (int i = 0; i < 1024; ++i) {
            if (i == 0) write_u32(buf, 48);
            else if (i == 1) write_u32(buf, 60);
            else write_u32(buf, 0);
        }

        // 稀疏块位图元数据
        write_u32(buf, 1);                     // n_bitmap_int32 = 1
        write_u32(buf, 1);                     // n_bits_for_block_map = 1
        write_u32(buf, 1);                     // n1024blocks_bis = 1
        write_u32(buf, 1);                     // n_leading_nonzero = 1
        // 位图: 1 bit, block 0 = active (1)
        write_u8(buf, 0x01);

        std::ofstream ofs(dir_path + "/a00000001.gdbtablx", std::ios::binary);
        ofs.write(reinterpret_cast<char*>(buf.data()), buf.size());
    }

    // ── 5. 构造 a00000001.gdbindexes ──
    {
        std::vector<uint8_t> buf;

        write_i32(buf, 1);  // nindexes = 1

        // 索引 0: 名称 "PK_UID", magic (1, 2, 0), known magic → magic4, col_name "ObjectId"
        write_i32(buf, 6);                // name_len = 6
        write_utf16(buf, "PK_UID");
        write_u16(buf, 1);                // magic1
        write_i32(buf, 2);                // magic2 = 2 (known)
        write_u16(buf, 0);                // magic3 = 0 (known)
        write_i32(buf, 0);                // magic4 (known magic)
        write_i32(buf, 8);                // col_name_len = 8
        write_utf16(buf, "ObjectId");
        write_u16(buf, 0);                // magic5

        std::ofstream ofs(dir_path + "/a00000001.gdbindexes", std::ios::binary);
        ofs.write(reinterpret_cast<char*>(buf.data()), buf.size());
    }

    return dir_path;
}

// ── 清理合成的 .gdb 目录 ──

inline void cleanup_synthetic_gdb(const std::string& dir_path) {
    std::error_code ec;
    fs::remove_all(dir_path, ec);
}

// ── 内存中构造 .gdbtable 文件内容（不写磁盘） ──

// 构造一个最小 v3 .gdbtable 二进制缓冲区
// 包含: 2 字段 (ObjectId + Int32 "score")，2 条记录
// 返回: (file_data, field_desc_offset, rec0_offset, rec1_offset)
struct TableBuilderResult {
    std::vector<uint8_t> data;
    uint64_t field_desc_offset;
    size_t rec0_offset;
    size_t rec1_offset;
};

inline TableBuilderResult build_minimal_table() {
    std::vector<uint8_t> buf;

    // 头部 (v3, 40 字节)
    write_u32(buf, 3);                     // version = 3
    write_u32(buf, 2);                     // nfeatures = 2
    write_u32(buf, 32);                    // largest_size_record
    write_u32(buf, 5);                     // unknown_role
    write_u32(buf, 0);                     // unknown_16
    write_u32(buf, 0);                     // unknown_20
    write_u64(buf, 256);                   // file_size
    uint64_t fdo = 40;
    write_u64(buf, fdo);                   // field_desc_offset

    // 字段区起始
    size_t section_start = buf.size();
    write_u32(buf, 0);                     // length 占位
    write_u32(buf, 3);                     // section_version
    write_u32(buf, 0);                     // geom_type_full (无几何)
    write_u16(buf, 2);                     // nfields = 2

    // 字段 0: ObjectId
    write_utf16_len(buf, "ObjectId");
    write_utf16_len(buf, "");
    write_u8(buf, 6);  // type = ObjectId
    write_u8(buf, 4);  // width
    write_u8(buf, 0);  // flag

    // 字段 1: Int32 "score"
    write_utf16_len(buf, "score");
    write_utf16_len(buf, "");
    write_u8(buf, 1);  // type = Int32
    write_u8(buf, 4);  // width
    write_u8(buf, 1);  // flag = 1 (nullable)

    // 回填 section length
    size_t slen = buf.size() - section_start - 4;
    std::memcpy(buf.data() + section_start, &slen, 4);

    // 记录区
    TableBuilderResult result;
    result.rec0_offset = buf.size();
    write_u32(buf, 5);                     // blob_len: bitmap + score
    write_u8(buf, 0);                      // nullable bitmap (score 不 null)
    write_u32(buf, 100);                   // score = 100

    result.rec1_offset = buf.size();
    write_u32(buf, 1);                     // blob_len: bitmap only
    write_u8(buf, 1);                      // nullable bitmap (score = null)
    // ObjectId implicit, no data for null score

    // 填充
    while (buf.size() < 256) buf.push_back(0);

    result.data = std::move(buf);
    result.field_desc_offset = fdo;
    return result;
}

// ── 内存中构造 .gdbtablx 文件内容 ──

inline std::vector<uint8_t> build_minimal_tablx(uint32_t rec0_off, uint32_t rec1_off) {
    std::vector<uint8_t> buf;

    // 头部 (v3, 16 字节)
    write_u32(buf, 3);                     // version = 3
    write_u32(buf, 1);                     // n1024blocks = 1
    write_u32(buf, 1024);                  // nfeatures = 1024
    write_u32(buf, 4);                     // size_tablx_offsets = 4

    // 偏移表: 1024 条目
    for (int i = 0; i < 1024; ++i) {
        if (i == 0) write_u32(buf, rec0_off);
        else if (i == 1) write_u32(buf, rec1_off);
        else write_u32(buf, 0);
    }

    // 稀疏位图
    write_u32(buf, 1);                     // n_bitmap_int32
    write_u32(buf, 1);                     // n_bits = 1
    write_u32(buf, 1);                     // n1024blocks_bis
    write_u32(buf, 1);                     // n_leading_nonzero
    write_u8(buf, 0x01);                   // bitmap: block 0 active

    return buf;
}

// ── 内存中构造 .gdbindexes 文件内容 ──

inline std::vector<uint8_t> build_minimal_indexes() {
    std::vector<uint8_t> buf;

    write_i32(buf, 1);  // nindexes = 1

    // 索引: "PK_score" (8 个字符), known magic (2, 0)
    write_i32(buf, 8);
    write_utf16(buf, "PK_score");
    write_u16(buf, 1);
    write_i32(buf, 2);                // magic2 = 2 (known)
    write_u16(buf, 0);                // magic3 = 0 (known)
    write_i32(buf, 0);                // magic4
    write_i32(buf, 5);                // col_name_len = 5
    write_utf16(buf, "score");
    write_u16(buf, 0);                // magic5

    return buf;
}

// ── 内存中构造几何 blob（不含长度前缀） ──

// 构造 Point(1.0, 2.0) 的几何 blob
// 坐标系: origin=(0,0), scale=1000 → raw = (coord - origin) * scale + 1
inline std::vector<uint8_t> build_geom_point_2d() {
    std::vector<uint8_t> buf;
    // geom_type = 1 (Point)
    write_varuint(buf, 1);
    // x = (1.0 - 0) * 1000 + 1 = 1001
    write_varuint(buf, 1001);
    // y = (2.0 - 0) * 1000 + 1 = 2001
    write_varuint(buf, 2001);
    return buf;
}

// 构造 PointZ(1.0, 2.0, 3.0) 的几何 blob
inline std::vector<uint8_t> build_geom_point_z() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 9);  // PointZ
    write_varuint(buf, 1001);  // x
    write_varuint(buf, 2001);  // y
    write_varuint(buf, 3001);  // z = 3.0 * 1000 + 1
    return buf;
}

// 构造 EMPTY Point (raw=0)
inline std::vector<uint8_t> build_geom_empty_point() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 1);
    write_varuint(buf, 0);  // x=0 → EMPTY
    write_varuint(buf, 0);  // y=0
    return buf;
}

// 构造 MultiPoint: 2 points at (1.0, 2.0), (4.0, 6.0)
// XY delta 编码: 首个点是绝对值，后续是 delta
inline std::vector<uint8_t> build_geom_multipoint() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 8);  // MultiPoint

    // nPoints
    write_varuint(buf, 2);

    // BBox: xmin=1001, ymin=2001, dx=3000, dy=4000 (raw values)
    write_varuint(buf, 1001);
    write_varuint(buf, 2001);
    write_varuint(buf, 3000);
    write_varuint(buf, 4000);

    // XY 数组 (delta 编码)
    // Point 0: x=1001, y=2001 (绝对值)
    write_svarint(buf, 1001);
    write_svarint(buf, 2001);
    // Point 1: dx=3000, dy=4000 (delta)
    write_svarint(buf, 3000);
    write_svarint(buf, 4000);

    return buf;
}

// 构造 Polyline: 1 part with 3 points
// (0,0), (1,0), (1,1) → scale=1000, origin=0
inline std::vector<uint8_t> build_geom_polyline() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 3);  // Polyline

    // nPoints
    write_varuint(buf, 3);
    // nParts
    write_varuint(buf, 1);

    // BBox
    write_varuint(buf, 1);     // xmin
    write_varuint(buf, 1);     // ymin
    write_varuint(buf, 1000);  // dx
    write_varuint(buf, 1000);  // dy

    // part_sizes: 1 part with 3 points
    // (only nParts-1 values, last = total - sum)
    // nParts=1, so no part_size values written

    // XY delta 编码 (累积)
    // Point 0: (0,0) → raw=1
    write_svarint(buf, 1);   // x
    write_svarint(buf, 1);   // y
    // Point 1: (1000,0) → delta=(999, -1)
    write_svarint(buf, 999); // dx
    write_svarint(buf, -1);  // dy (0→0, but raw stays at 1, delta=-1+1000=999... wait)

    // Let me recalculate:
    // raw coords: 1, 1, 1001, 1, 1001, 1001
    // deltas:     1, 1, 1000, 0, 1000, 1000
    // Actually delta from cumulative:
    // cumulative: 1, 1 → delta(1,1)
    // cumulative: 1001, 1 → delta(1000, 0)
    // cumulative: 1001, 1001 → delta(0, 1000)

    // Clear and redo
    buf.clear();
    write_varuint(buf, 3);
    write_varuint(buf, 3);  // nPoints
    write_varuint(buf, 1);  // nParts
    write_varuint(buf, 1); write_varuint(buf, 1);
    write_varuint(buf, 1000); write_varuint(buf, 1000);

    // No part_sizes for nParts=1
    // Point 0: cum=(1, 1)
    write_svarint(buf, 1); write_svarint(buf, 1);
    // Point 1: cum=(1001, 1) → delta=(1000, 0)
    write_svarint(buf, 1000); write_svarint(buf, 0);
    // Point 2: cum=(1001, 1001) → delta=(0, 1000)
    write_svarint(buf, 0); write_svarint(buf, 1000);

    return buf;
}

// 构造 Polygon: 1 ring with 4 points (square)
// (0,0), (1,0), (1,1), (0,1) → decoder will close the ring
inline std::vector<uint8_t> build_geom_polygon() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 5);  // Polygon

    write_varuint(buf, 4);  // nPoints
    write_varuint(buf, 1);  // nParts

    // BBox
    write_varuint(buf, 1); write_varuint(buf, 1);
    write_varuint(buf, 1000); write_varuint(buf, 1000);

    // Points (delta 编码，累积)
    // (0,0) → raw=1
    write_svarint(buf, 1); write_svarint(buf, 1);
    // (1000,0) → delta=(1000, 0)  cum=(1001, 1)
    write_svarint(buf, 1000); write_svarint(buf, 0);
    // (1000,1000) → delta=(0, 1000) cum=(1001, 1001)
    write_svarint(buf, 0); write_svarint(buf, 1000);
    // (0,1000) → delta=(-1000, 0) cum=(1, 1001)
    write_svarint(buf, -1000); write_svarint(buf, 0);

    return buf;
}

// ── Z/M 变体几何 blob ──

// 构造 MultiPointZ: 2 points with Z
// type=20 (MultiPoint Z), layer_has_z=true
inline std::vector<uint8_t> build_geom_multipoint_z() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 20);  // MultiPoint Z
    write_varuint(buf, 2);   // nPoints
    // BBox
    write_varuint(buf, 1001); write_varuint(buf, 2001);
    write_varuint(buf, 3000); write_varuint(buf, 4000);
    // XY delta
    write_svarint(buf, 1001); write_svarint(buf, 2001);
    write_svarint(buf, 3000); write_svarint(buf, 4000);
    // Z delta (absolute first, then deltas)
    write_svarint(buf, 3001);  // z0 = 3.0
    write_svarint(buf, 1000);  // z1 = 3.0 + 1.0 = 4.0
    return buf;
}

// 构造 MultiPointM: 2 points with M
// type=28 (MultiPoint M), layer_has_m=true
inline std::vector<uint8_t> build_geom_multipoint_m() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 28);  // MultiPoint M
    write_varuint(buf, 2);   // nPoints
    // BBox
    write_varuint(buf, 1001); write_varuint(buf, 2001);
    write_varuint(buf, 3000); write_varuint(buf, 4000);
    // XY delta
    write_svarint(buf, 1001); write_svarint(buf, 2001);
    write_svarint(buf, 3000); write_svarint(buf, 4000);
    // M delta (first is absolute)
    write_svarint(buf, 5001);  // m0 = 5.0
    write_svarint(buf, 1000);  // m1 = 5.0 + 1.0 = 6.0
    return buf;
}

// 构造 PolylineZ: 1 part, 3 points with Z
// type=10 (Polyline Z)
inline std::vector<uint8_t> build_geom_polyline_z() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 10);  // Polyline Z
    write_varuint(buf, 3);   // nPoints
    write_varuint(buf, 1);   // nParts
    // BBox
    write_varuint(buf, 1); write_varuint(buf, 1);
    write_varuint(buf, 1000); write_varuint(buf, 1000);
    // XY delta
    write_svarint(buf, 1); write_svarint(buf, 1);
    write_svarint(buf, 1000); write_svarint(buf, 0);
    write_svarint(buf, 0); write_svarint(buf, 1000);
    // Z delta
    write_svarint(buf, 1001);  // z0 = 1.0
    write_svarint(buf, 1000);  // z1 = 2.0
    write_svarint(buf, 1000);  // z2 = 3.0
    return buf;
}

// 构造 PolygonZ: 1 ring, 4 points with Z
// type=19 (Polygon Z)
inline std::vector<uint8_t> build_geom_polygon_z() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 19);  // Polygon Z
    write_varuint(buf, 4);   // nPoints
    write_varuint(buf, 1);   // nParts
    // BBox
    write_varuint(buf, 1); write_varuint(buf, 1);
    write_varuint(buf, 1000); write_varuint(buf, 1000);
    // XY delta
    write_svarint(buf, 1); write_svarint(buf, 1);
    write_svarint(buf, 1000); write_svarint(buf, 0);
    write_svarint(buf, 0); write_svarint(buf, 1000);
    write_svarint(buf, -1000); write_svarint(buf, 0);
    // Z delta
    write_svarint(buf, 1001);  // z0
    write_svarint(buf, 0);     // z1 same
    write_svarint(buf, 0);     // z2 same
    write_svarint(buf, 0);     // z3 same
    return buf;
}

// 构造空 MultiPoint (nPoints=0)
inline std::vector<uint8_t> build_geom_empty_multipoint() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 8);  // MultiPoint
    write_varuint(buf, 0);  // nPoints = 0
    return buf;
}

// 构造空 Polyline (nPoints=0)
inline std::vector<uint8_t> build_geom_empty_polyline() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 3);  // Polyline
    write_varuint(buf, 0);  // nPoints = 0
    return buf;
}

// 构造空 Polygon (nPoints=0)
inline std::vector<uint8_t> build_geom_empty_polygon() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 5);  // Polygon
    write_varuint(buf, 0);  // nPoints = 0
    return buf;
}

// 构造未知类型几何 (type=99)
inline std::vector<uint8_t> build_geom_unknown_type() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 99);  // Unknown type
    write_varuint(buf, 0);
    return buf;
}

// 构造 PointM (type=21): x_raw, y_raw, m_raw
// layer_has_m=true, scale=1000, origin=0
// Point: (1.0, 2.0) with M=5.0
inline std::vector<uint8_t> build_geom_point_m() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 21);  // Point M
    // x_raw=1001 → (1001-1)/1000+0 = 1.0
    write_varuint(buf, 1001);
    // y_raw=2001 → 2.0
    write_varuint(buf, 2001);
    // m_raw=5001 → 5.0
    write_varuint(buf, 5001);
    return buf;
}

// 构造 PointZM (type=11): x_raw, y_raw, z_raw, m_raw
// Point: (1.0, 2.0, 3.0, 5.0)
inline std::vector<uint8_t> build_geom_point_zm() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 11);  // Point ZM
    write_varuint(buf, 1001);  // x → 1.0
    write_varuint(buf, 2001);  // y → 2.0
    write_varuint(buf, 3001);  // z → 3.0
    write_varuint(buf, 5001);  // m → 5.0
    return buf;
}

// 构造 MultiPatch (type=32): 1 part with 3 points (TriangleStrip)
// nPoints=3, magic=0, nParts=1, part_type=0 (TriangleStrip)
inline std::vector<uint8_t> build_geom_multipatch() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 32);  // MultiPatch
    write_varuint(buf, 3);   // nPoints
    write_varuint(buf, 0);   // magic
    write_varuint(buf, 1);   // nParts
    // BBox
    write_varuint(buf, 1); write_varuint(buf, 1);
    write_varuint(buf, 1000); write_varuint(buf, 1000);
    // part_sizes: 1 part, so no values needed (last = nPoints - sum)
    // part_types: 1 value
    write_varuint(buf, 0);   // TriangleStrip
    // XY: 3 points delta encoded
    write_svarint(buf, 1); write_svarint(buf, 1);       // cum(1,1)
    write_svarint(buf, 1000); write_svarint(buf, 0);    // cum(1001,1)
    write_svarint(buf, 0); write_svarint(buf, 1000);    // cum(1001,1001)
    // Z: 3 values
    write_svarint(buf, 1001);  // z0 = 1.0
    write_svarint(buf, 1000);  // z1 = 2.0
    write_svarint(buf, 1000);  // z2 = 3.0
    return buf;
}

// 构造 MultiPatchM (type=31): like MultiPatch but also has M
inline std::vector<uint8_t> build_geom_multipatch_m() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 31);  // MultiPatch M
    write_varuint(buf, 3);   // nPoints
    write_varuint(buf, 0);   // magic
    write_varuint(buf, 1);   // nParts
    // BBox
    write_varuint(buf, 1); write_varuint(buf, 1);
    write_varuint(buf, 1000); write_varuint(buf, 1000);
    // part_types
    write_varuint(buf, 2);   // OuterRing
    // XY
    write_svarint(buf, 1); write_svarint(buf, 1);
    write_svarint(buf, 1000); write_svarint(buf, 0);
    write_svarint(buf, 0); write_svarint(buf, 1000);
    // Z
    write_svarint(buf, 1001);
    write_svarint(buf, 1000);
    write_svarint(buf, 1000);
    // M
    write_svarint(buf, 5001);
    write_svarint(buf, 1000);
    write_svarint(buf, 1000);
    return buf;
}

// 构造 GeneralPolyline (type=50): like Polyline; no curve flag means no nCurves header
// Z/M flags from high bits of geom_type
// Plain GeneralPolyline (no Z/M): geom_type = 50
inline std::vector<uint8_t> build_geom_general_polyline() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 50);  // GeneralPolyline (no Z/M)
    write_varuint(buf, 3);   // nPoints
    write_varuint(buf, 1);   // nParts
    // BBox
    write_varuint(buf, 1); write_varuint(buf, 1);
    write_varuint(buf, 1000); write_varuint(buf, 1000);
    // XY
    write_svarint(buf, 1); write_svarint(buf, 1);
    write_svarint(buf, 1000); write_svarint(buf, 0);
    write_svarint(buf, 0); write_svarint(buf, 1000);
    return buf;
}

// 构造 GeneralPolygon (type=51): like Polygon; no curve flag means no nCurves header
inline std::vector<uint8_t> build_geom_general_polygon() {
    std::vector<uint8_t> buf;
    write_varuint(buf, 51);  // GeneralPolygon (no Z/M)
    write_varuint(buf, 4);   // nPoints
    write_varuint(buf, 1);   // nParts
    // BBox
    write_varuint(buf, 1); write_varuint(buf, 1);
    write_varuint(buf, 1000); write_varuint(buf, 1000);
    // XY
    write_svarint(buf, 1); write_svarint(buf, 1);
    write_svarint(buf, 1000); write_svarint(buf, 0);
    write_svarint(buf, 0); write_svarint(buf, 1000);
    write_svarint(buf, -1000); write_svarint(buf, 0);
    return buf;
}

} // namespace explorgdb_test

#endif // TEST_FIXTURE_EXPLORGDB_H
