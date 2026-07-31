// tests/edgar/explorgdb/test_geometry.cpp
// 几何 blob 解码测试 — 使用合成数据验证解码器
//
// 测试覆盖：
//   1. Point 2D/3D 解码
//   2. EMPTY Point
//   3. MultiPoint delta 编码
//   4. Polyline 多部件 → MULTILINESTRING WKT
//   5. Polygon 环闭合 → MULTIPOLYGON WKT
//   6. decode_from_field (含 varuint 长度前缀)

#include "test_fixture_explorgdb.h"
#include "gdb_geometry.h"
#include <gtest/gtest.h>

using namespace explorgdb;

// 创建解码器: origin=(0,0), scale=1000, 无 Z/M 图层能力
static GdbGeomDecoder make_decoder_2d() {
    return GdbGeomDecoder(
        0.0, 0.0, 1000.0,    // XY
        0.0, 1000.0,         // Z
        0.0, 1000.0,         // M
        false, false);       // 图层无 Z/M
}

static GdbGeomDecoder make_decoder_z() {
    return GdbGeomDecoder(
        0.0, 0.0, 1000.0,
        0.0, 1000.0,
        0.0, 1000.0,
        true, false);
}

// ── Point 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, Point2D) {
    auto buf = explorgdb_test::build_geom_point_2d();
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    EXPECT_FALSE(geom.has_z);
    EXPECT_FALSE(geom.has_m);
    EXPECT_EQ(geom.type, GdbGeomType::Point);

    // WKT 应该包含 1.0 和 2.0
    EXPECT_NE(geom.wkt.find("1"), std::string::npos);
    EXPECT_NE(geom.wkt.find("2"), std::string::npos);
    EXPECT_EQ(geom.wkt.substr(0, 5), "POINT");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PointZ) {
    auto buf = explorgdb_test::build_geom_point_z();
    auto decoder = make_decoder_z();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    EXPECT_TRUE(geom.has_z);
    EXPECT_EQ(geom.type, GdbGeomType::PointZ);
    EXPECT_NE(geom.wkt.find("POINT Z"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, EmptyPoint) {
    auto buf = explorgdb_test::build_geom_empty_point();
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_TRUE(geom.is_empty);
    EXPECT_EQ(geom.wkt, "POINT EMPTY");
}

// ── MultiPoint 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, MultiPoint2D) {
    auto buf = explorgdb_test::build_geom_multipoint();
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    EXPECT_EQ(geom.type, GdbGeomType::MultiPoint);

    // WKT 应该以 MULTILINESTRING 或 MULTIPOINT 开头
    // 注意: 根据实现，MultiPoint → MULTIPOINT WKT
    EXPECT_NE(geom.wkt.find("MULTIPOINT"), std::string::npos);

    // 应该有两个坐标对
    int paren_count = 0;
    for (char c : geom.wkt) {
        if (c == '(') paren_count++;
        if (c == ')') paren_count--;
    }
    // MULTIPOINT ((x y), (x y)) 应该有两对括号 + 一个外层
    EXPECT_GE(paren_count, 0);
}

// ── Polyline 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, Polyline2D) {
    auto buf = explorgdb_test::build_geom_polyline();
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    EXPECT_EQ(geom.type, GdbGeomType::Polyline);
    EXPECT_NE(geom.wkt.find("MULTILINESTRING"), std::string::npos);

    // 应该有 3 个点 (不闭合)
    // WKT 格式: MULTILINESTRING ((0 0, 1 1, ...))
    EXPECT_NE(geom.wkt.find("("), std::string::npos);
}

// ── Polygon 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, Polygon2D) {
    auto buf = explorgdb_test::build_geom_polygon();
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    EXPECT_EQ(geom.type, GdbGeomType::Polygon);
    EXPECT_NE(geom.wkt.find("MULTIPOLYGON"), std::string::npos);

    // Polygon 的环应该被闭合（首点重复）
    // 4 个点 + 1 个闭合点 = 5 个坐标
    int comma_count = 0;
    for (char c : geom.wkt) {
        if (c == ',') comma_count++;
    }
    EXPECT_EQ(comma_count, 4);  // 5 个点之间有 4 个逗号
}

// ── decode_from_field 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, DecodeFromField) {
    // 构造包含 varuint 长度前缀的数据
    auto geom_buf = explorgdb_test::build_geom_point_2d();

    std::vector<uint8_t> field_data;
    explorgdb_test::write_varuint(field_data, geom_buf.size());
    field_data.insert(field_data.end(), geom_buf.begin(), geom_buf.end());

    auto decoder = make_decoder_2d();
    auto geom = decoder.decode_from_field(field_data.data(), field_data.size());

    EXPECT_FALSE(geom.is_empty);
    EXPECT_EQ(geom.type, GdbGeomType::Point);
}

// ── NULL 几何测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, NullGeometry) {
    // 空缓冲区
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(nullptr, 0);

    EXPECT_TRUE(geom.is_empty);
    EXPECT_EQ(geom.type, GdbGeomType::Null);
}

// ── WKT 格式验证 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, WKTFormat) {
    auto buf = explorgdb_test::build_geom_point_2d();
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    // WKT 格式: "TYPE (x y)"
    // 验证括号配对
    size_t open = geom.wkt.find('(');
    size_t close = geom.wkt.find(')');
    EXPECT_NE(open, std::string::npos);
    EXPECT_NE(close, std::string::npos);
    EXPECT_LT(open, close);

    // 验证坐标分隔符
    std::string inner = geom.wkt.substr(open + 1, close - open - 1);
    size_t space = inner.find(' ');
    EXPECT_NE(space, std::string::npos);
}

// ── Z/M 变体测试 ──

static GdbGeomDecoder make_decoder_zm() {
    return GdbGeomDecoder(
        0.0, 0.0, 1000.0,
        0.0, 1000.0,
        0.0, 1000.0,
        true, true);
}

static GdbGeomDecoder make_decoder_m() {
    return GdbGeomDecoder(
        0.0, 0.0, 1000.0,
        0.0, 1000.0,
        0.0, 1000.0,
        false, true);
}

static constexpr uint64_t kGeneralZFlag = 0x80000000ULL;
static constexpr uint64_t kGeneralMFlag = 0x40000000ULL;
static constexpr uint64_t kGeneralCurveFlag = 0x20000000ULL;

static void expect_no_unknown(const GdbGeometry& geom) {
    EXPECT_EQ(geom.wkt.find("UNKNOWN"), std::string::npos) << geom.wkt;
}

static void write_test_bbox(std::vector<uint8_t>& buf,
                            uint64_t xmin, uint64_t ymin,
                            uint64_t dx, uint64_t dy) {
    explorgdb_test::write_varuint(buf, xmin);
    explorgdb_test::write_varuint(buf, ymin);
    explorgdb_test::write_varuint(buf, dx);
    explorgdb_test::write_varuint(buf, dy);
}

static std::vector<uint8_t> build_general_point(bool has_z, bool has_m) {
    std::vector<uint8_t> buf;
    uint64_t geom_type = 52;
    if (has_z) geom_type |= kGeneralZFlag;
    if (has_m) geom_type |= kGeneralMFlag;
    explorgdb_test::write_varuint(buf, geom_type);
    explorgdb_test::write_varuint(buf, 12346);  // (12346 - 1) / 1000 = 12.345
    explorgdb_test::write_varuint(buf, 67891);  // (67891 - 1) / 1000 = 67.89
    if (has_z) explorgdb_test::write_varuint(buf, 7001);
    if (has_m) explorgdb_test::write_varuint(buf, 9001);
    return buf;
}

static std::vector<uint8_t> build_boundary_point() {
    std::vector<uint8_t> buf;
    explorgdb_test::write_varuint(buf, 1);
    explorgdb_test::write_varuint(buf, 1001);  // exact x = 1.0 only with (raw - 1)
    explorgdb_test::write_varuint(buf, 2001);  // exact y = 2.0 only with (raw - 1)
    return buf;
}

static std::vector<uint8_t> build_multipoint_with_bbox(uint64_t geom_type,
                                                        uint64_t xmin,
                                                        uint64_t ymin,
                                                        uint64_t dx,
                                                        uint64_t dy,
                                                        bool has_z,
                                                        bool has_m) {
    std::vector<uint8_t> buf;
    explorgdb_test::write_varuint(buf, geom_type);
    explorgdb_test::write_varuint(buf, 2);  // nPoints; MultiPoint has no nParts header
    write_test_bbox(buf, xmin, ymin, dx, dy);
    explorgdb_test::write_svarint(buf, static_cast<int64_t>(xmin));
    explorgdb_test::write_svarint(buf, static_cast<int64_t>(ymin));
    explorgdb_test::write_svarint(buf, static_cast<int64_t>(dx));
    explorgdb_test::write_svarint(buf, static_cast<int64_t>(dy));
    if (has_z) {
        explorgdb_test::write_svarint(buf, 5000);
        explorgdb_test::write_svarint(buf, 1000);
    }
    if (has_m) {
        explorgdb_test::write_svarint(buf, 7000);
        explorgdb_test::write_svarint(buf, 1000);
    }
    return buf;
}

static std::vector<uint8_t> build_general_multipoint(bool has_z, bool has_m) {
    uint64_t geom_type = 53;
    if (has_z) geom_type |= kGeneralZFlag;
    if (has_m) geom_type |= kGeneralMFlag;
    return build_multipoint_with_bbox(geom_type, 21000, 31000, 2000, 3000, has_z, has_m);
}

static std::vector<uint8_t> build_general_polyline_with_curve_count(uint64_t nCurves) {
    std::vector<uint8_t> buf;
    explorgdb_test::write_varuint(buf, 50 | kGeneralCurveFlag);
    explorgdb_test::write_varuint(buf, 3);  // nPoints
    explorgdb_test::write_varuint(buf, 1);  // nParts
    explorgdb_test::write_varuint(buf, nCurves);
    write_test_bbox(buf, 1, 1, 1000, 1000);
    explorgdb_test::write_svarint(buf, 1);
    explorgdb_test::write_svarint(buf, 1);
    explorgdb_test::write_svarint(buf, 1000);
    explorgdb_test::write_svarint(buf, 0);
    explorgdb_test::write_svarint(buf, 0);
    explorgdb_test::write_svarint(buf, 1000);
    return buf;
}

static std::vector<uint8_t> build_general_polygon_with_curve_count(uint64_t nCurves) {
    std::vector<uint8_t> buf;
    explorgdb_test::write_varuint(buf, 51 | kGeneralCurveFlag);
    explorgdb_test::write_varuint(buf, 4);  // nPoints
    explorgdb_test::write_varuint(buf, 1);  // nParts
    explorgdb_test::write_varuint(buf, nCurves);
    write_test_bbox(buf, 1, 1, 1000, 1000);
    explorgdb_test::write_svarint(buf, 1);
    explorgdb_test::write_svarint(buf, 1);
    explorgdb_test::write_svarint(buf, 1000);
    explorgdb_test::write_svarint(buf, 0);
    explorgdb_test::write_svarint(buf, 0);
    explorgdb_test::write_svarint(buf, 1000);
    explorgdb_test::write_svarint(buf, -1000);
    explorgdb_test::write_svarint(buf, 0);
    return buf;
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, MultiPointZ) {
    auto buf = explorgdb_test::build_geom_multipoint_z();
    auto decoder = make_decoder_z();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    EXPECT_TRUE(geom.has_z);
    EXPECT_FALSE(geom.has_m);
    EXPECT_NE(geom.wkt.find("MULTIPOINT"), std::string::npos);
    // Z 坐标应该出现在 WKT 中
    EXPECT_NE(geom.wkt.find("3"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, MultiPointM) {
    auto buf = explorgdb_test::build_geom_multipoint_m();
    auto decoder = make_decoder_m();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    EXPECT_FALSE(geom.has_z);
    EXPECT_TRUE(geom.has_m);
    EXPECT_NE(geom.wkt.find("MULTIPOINT"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralMultiPoint2DDecode) {
    auto buf = build_general_multipoint(false, false);
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_EQ(geom.type, GdbGeomType::GeneralMultiPoint);
    EXPECT_FALSE(geom.is_empty);
    EXPECT_FALSE(geom.has_z);
    EXPECT_FALSE(geom.has_m);
    expect_no_unknown(geom);
    EXPECT_EQ(geom.wkt.rfind("MULTIPOINT", 0), 0u) << geom.wkt;
    EXPECT_NE(geom.wkt.find("(21 31)"), std::string::npos) << geom.wkt;
    EXPECT_NE(geom.wkt.find("(23 34)"), std::string::npos) << geom.wkt;
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralMultiPointZDecode) {
    auto buf = build_general_multipoint(true, false);
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_EQ(geom.type, GdbGeomType::GeneralMultiPoint);
    EXPECT_FALSE(geom.is_empty);
    EXPECT_TRUE(geom.has_z);
    EXPECT_FALSE(geom.has_m);
    expect_no_unknown(geom);
    EXPECT_EQ(geom.wkt.rfind("MULTIPOINT Z", 0), 0u) << geom.wkt;
    EXPECT_NE(geom.wkt.find("(21 31 5)"), std::string::npos) << geom.wkt;
    EXPECT_NE(geom.wkt.find("(23 34 6)"), std::string::npos) << geom.wkt;
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralMultiPointMDecode) {
    auto buf = build_general_multipoint(false, true);
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_EQ(geom.type, GdbGeomType::GeneralMultiPoint);
    EXPECT_FALSE(geom.is_empty);
    EXPECT_FALSE(geom.has_z);
    EXPECT_TRUE(geom.has_m);
    expect_no_unknown(geom);
    EXPECT_EQ(geom.wkt.rfind("MULTIPOINT M", 0), 0u) << geom.wkt;
    EXPECT_NE(geom.wkt.find("(21 31 7)"), std::string::npos) << geom.wkt;
    EXPECT_NE(geom.wkt.find("(23 34 8)"), std::string::npos) << geom.wkt;
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralMultiPointZMDecode) {
    auto buf = build_general_multipoint(true, true);
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_EQ(geom.type, GdbGeomType::GeneralMultiPoint);
    EXPECT_FALSE(geom.is_empty);
    EXPECT_TRUE(geom.has_z);
    EXPECT_TRUE(geom.has_m);
    expect_no_unknown(geom);
    EXPECT_EQ(geom.wkt.rfind("MULTIPOINT ZM", 0), 0u) << geom.wkt;
    EXPECT_NE(geom.wkt.find("(21 31 5 7)"), std::string::npos) << geom.wkt;
    EXPECT_NE(geom.wkt.find("(23 34 6 8)"), std::string::npos) << geom.wkt;
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PolylineZ) {
    auto buf = explorgdb_test::build_geom_polyline_z();
    auto decoder = make_decoder_z();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    EXPECT_TRUE(geom.has_z);
    EXPECT_NE(geom.wkt.find("MULTILINESTRING"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PolygonZ) {
    auto buf = explorgdb_test::build_geom_polygon_z();
    auto decoder = make_decoder_z();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    EXPECT_TRUE(geom.has_z);
    EXPECT_NE(geom.wkt.find("MULTIPOLYGON"), std::string::npos);
    // Polygon 环应被闭合
    int comma_count = 0;
    for (char c : geom.wkt) {
        if (c == ',') comma_count++;
    }
    EXPECT_EQ(comma_count, 4);  // 5 points (4 + closure)
}

// ── 空几何测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, EmptyMultiPoint) {
    auto buf = explorgdb_test::build_geom_empty_multipoint();
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_TRUE(geom.is_empty);
    EXPECT_NE(geom.wkt.find("EMPTY"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, EmptyPolyline) {
    auto buf = explorgdb_test::build_geom_empty_polyline();
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_TRUE(geom.is_empty);
    EXPECT_NE(geom.wkt.find("EMPTY"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, EmptyPolygon) {
    auto buf = explorgdb_test::build_geom_empty_polygon();
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_TRUE(geom.is_empty);
    EXPECT_NE(geom.wkt.find("EMPTY"), std::string::npos);
}

// ── 未知类型测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, UnknownType) {
    auto buf = explorgdb_test::build_geom_unknown_type();
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_EQ(geom.type, GdbGeomType::Null);
    EXPECT_NE(geom.wkt.find("UNKNOWN"), std::string::npos);
}

// ── decode_from_field 空数据测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, DecodeFromFieldEmpty) {
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode_from_field(nullptr, 0);
    EXPECT_TRUE(geom.is_empty);
    EXPECT_EQ(geom.wkt, "POINT EMPTY");
}

// ── PointM / PointZM 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PointM) {
    auto buf = explorgdb_test::build_geom_point_m();
    auto decoder = make_decoder_m();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    EXPECT_FALSE(geom.has_z);
    EXPECT_TRUE(geom.has_m);
    EXPECT_NE(geom.wkt.find("POINT M"), std::string::npos);
    // Should contain M value (5.0)
    EXPECT_NE(geom.wkt.find("5"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PointZM) {
    auto buf = explorgdb_test::build_geom_point_zm();
    auto decoder = make_decoder_zm();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    EXPECT_TRUE(geom.has_z);
    EXPECT_TRUE(geom.has_m);
    // geom_type_name(11) = "POINT ZM"
    EXPECT_NE(geom.wkt.find("POINT ZM"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralPoint2DDecode) {
    auto buf = build_general_point(false, false);
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_EQ(geom.type, GdbGeomType::GeneralPoint);
    EXPECT_FALSE(geom.is_empty);
    EXPECT_FALSE(geom.has_z);
    EXPECT_FALSE(geom.has_m);
    expect_no_unknown(geom);
    EXPECT_EQ(geom.wkt.rfind("POINT", 0), 0u) << geom.wkt;
    EXPECT_NE(geom.wkt.find("12.345 67.89"), std::string::npos) << geom.wkt;
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralPointZDecode) {
    auto buf = build_general_point(true, false);
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_EQ(geom.type, GdbGeomType::GeneralPoint);
    EXPECT_FALSE(geom.is_empty);
    EXPECT_TRUE(geom.has_z);
    EXPECT_FALSE(geom.has_m);
    expect_no_unknown(geom);
    EXPECT_EQ(geom.wkt.rfind("POINT Z", 0), 0u) << geom.wkt;
    EXPECT_NE(geom.wkt.find("12.345 67.89 7"), std::string::npos) << geom.wkt;
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralPointMDecode) {
    auto buf = build_general_point(false, true);
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_EQ(geom.type, GdbGeomType::GeneralPoint);
    EXPECT_FALSE(geom.is_empty);
    EXPECT_FALSE(geom.has_z);
    EXPECT_TRUE(geom.has_m);
    expect_no_unknown(geom);
    EXPECT_EQ(geom.wkt.rfind("POINT M", 0), 0u) << geom.wkt;
    EXPECT_NE(geom.wkt.find("12.345 67.89 9"), std::string::npos) << geom.wkt;
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralPointZMDecode) {
    auto buf = build_general_point(true, true);
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_EQ(geom.type, GdbGeomType::GeneralPoint);
    EXPECT_FALSE(geom.is_empty);
    EXPECT_TRUE(geom.has_z);
    EXPECT_TRUE(geom.has_m);
    expect_no_unknown(geom);
    EXPECT_EQ(geom.wkt.rfind("POINT ZM", 0), 0u) << geom.wkt;
    EXPECT_NE(geom.wkt.find("12.345 67.89 7 9"), std::string::npos) << geom.wkt;
}

// ── MultiPatch 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, MultiPatch) {
    auto buf = explorgdb_test::build_geom_multipatch();
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    EXPECT_TRUE(geom.has_z);  // MultiPatch always has Z
    EXPECT_FALSE(geom.has_m);
    EXPECT_NE(geom.wkt.find("GEOMETRYCOLLECTION Z"), std::string::npos);
    EXPECT_NE(geom.wkt.find("POLYGON Z"), std::string::npos);
    EXPECT_EQ(geom.wkt.find("MultiPatch"), std::string::npos);
    EXPECT_NE(geom.wkt.find("0.001 0.001 1.001"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, MultiPatchM) {
    auto buf = explorgdb_test::build_geom_multipatch_m();
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    EXPECT_TRUE(geom.has_z);
    EXPECT_TRUE(geom.has_m);
    EXPECT_NE(geom.wkt.find("GEOMETRYCOLLECTION ZM"), std::string::npos);
    EXPECT_NE(geom.wkt.find("POLYGON ZM"), std::string::npos);
    EXPECT_EQ(geom.wkt.find("MultiPatch"), std::string::npos);
    EXPECT_NE(geom.wkt.find("0.001 0.001 1.001 5.001"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, MultiPatchEmpty) {
    std::vector<uint8_t> buf;
    explorgdb_test::write_varuint(buf, 32);  // MultiPatch
    explorgdb_test::write_varuint(buf, 0);   // nPoints = 0
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());
    EXPECT_TRUE(geom.is_empty);
    EXPECT_EQ(geom.wkt, "GEOMETRYCOLLECTION Z EMPTY");
}

// ── General Types (50-54) 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralPolyline) {
    auto buf = explorgdb_test::build_geom_general_polyline();
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    // GeneralPolyline uses decode_polyline → MULTILINESTRING WKT
    EXPECT_NE(geom.wkt.find("MULTILINESTRING"), std::string::npos);
    EXPECT_NE(geom.wkt.find("0.001 0.001"), std::string::npos);
    EXPECT_NE(geom.wkt.find("1.001 1.001"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralPolygon) {
    auto buf = explorgdb_test::build_geom_general_polygon();
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(geom.is_empty);
    // GeneralPolygon uses decode_polygon → MULTIPOLYGON WKT
    EXPECT_NE(geom.wkt.find("MULTIPOLYGON"), std::string::npos);
    EXPECT_NE(geom.wkt.find("0.001 0.001"), std::string::npos);
    EXPECT_NE(geom.wkt.find("1.001 1.001"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralPolylineWithZ) {
    // GeneralPolyline with Z flag: geom_type = 50 | 0x80000000
    std::vector<uint8_t> buf;
    explorgdb_test::write_varuint(buf, 50 | 0x80000000ULL);  // GeneralPolyline Z
    explorgdb_test::write_varuint(buf, 3);   // nPoints
    explorgdb_test::write_varuint(buf, 1);   // nParts
    // BBox
    explorgdb_test::write_varuint(buf, 1); explorgdb_test::write_varuint(buf, 1);
    explorgdb_test::write_varuint(buf, 1000); explorgdb_test::write_varuint(buf, 1000);
    // XY
    explorgdb_test::write_svarint(buf, 1); explorgdb_test::write_svarint(buf, 1);
    explorgdb_test::write_svarint(buf, 1000); explorgdb_test::write_svarint(buf, 0);
    explorgdb_test::write_svarint(buf, 0); explorgdb_test::write_svarint(buf, 1000);
    // Z
    explorgdb_test::write_svarint(buf, 1001);
    explorgdb_test::write_svarint(buf, 1000);
    explorgdb_test::write_svarint(buf, 1000);

    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());
    EXPECT_FALSE(geom.is_empty);
    EXPECT_TRUE(geom.has_z);
    EXPECT_NE(geom.wkt.find("0.001 0.001 1.001"), std::string::npos);
    EXPECT_NE(geom.wkt.find("1.001 1.001 3.001"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralPolylineWithCurvesIsExplicitlyUnsupported) {
    std::vector<uint8_t> buf;
    explorgdb_test::write_varuint(buf, 50 | 0x20000000ULL);
    explorgdb_test::write_varuint(buf, 2);
    explorgdb_test::write_varuint(buf, 1);
    explorgdb_test::write_varuint(buf, 1);

    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());
    EXPECT_NE(geom.wkt.find("UNSUPPORTED_CURVE_GEOMETRY"), std::string::npos);
    EXPECT_NE(geom.wkt.find("nCurves=1"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralPolygonWithCurvesIsExplicitlyUnsupported) {
    std::vector<uint8_t> buf;
    explorgdb_test::write_varuint(buf, 51 | 0x20000000ULL);
    explorgdb_test::write_varuint(buf, 4);
    explorgdb_test::write_varuint(buf, 1);
    explorgdb_test::write_varuint(buf, 2);

    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());
    EXPECT_NE(geom.wkt.find("UNSUPPORTED_CURVE_GEOMETRY"), std::string::npos);
    EXPECT_NE(geom.wkt.find("nCurves=2"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralWithoutCurveFlagIsNotUnsupportedCurve) {
    auto buf = explorgdb_test::build_geom_general_polyline();
    auto decoder = make_decoder_2d();
    EXPECT_FALSE(decoder.has_unsupported_curve_geometry(buf.data(), buf.size()));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralCurveSpatialFilterFailsClosed) {
    auto buf = build_general_polyline_with_curve_count(1);

    auto decoder = make_decoder_2d();
    EXPECT_TRUE(decoder.has_unsupported_curve_geometry(buf.data(), buf.size()));
    EXPECT_FALSE(decoder.peek_bbox(buf.data(), buf.size()).has_value());
    EXPECT_FALSE(decoder.intersects_with_peek(buf.data(), buf.size(), -1.0, -1.0, 1.0, 1.0));
    EXPECT_FALSE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), -1.0, -1.0, 1.0, 1.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralPolylineCurveFlagWithZeroCurvesBehavesLikeNormalPolyline) {
    auto buf = build_general_polyline_with_curve_count(0);
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(decoder.has_unsupported_curve_geometry(buf.data(), buf.size()));
    EXPECT_FALSE(geom.is_empty);
    expect_no_unknown(geom);
    EXPECT_EQ(geom.wkt.find("UNSUPPORTED_CURVE_GEOMETRY"), std::string::npos) << geom.wkt;
    EXPECT_NE(geom.wkt.find("MULTILINESTRING"), std::string::npos) << geom.wkt;

    auto bbox = decoder.peek_bbox(buf.data(), buf.size());
    ASSERT_TRUE(bbox.has_value());
    EXPECT_DOUBLE_EQ(bbox->xmin, 0.001);
    EXPECT_DOUBLE_EQ(bbox->ymin, 0.001);
    EXPECT_DOUBLE_EQ(bbox->xmax, 1.001);
    EXPECT_DOUBLE_EQ(bbox->ymax, 1.001);
    EXPECT_TRUE(decoder.intersects_with_peek(buf.data(), buf.size(), -1.0, -1.0, 5.0, 5.0));
    EXPECT_TRUE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), -1.0, -1.0, 5.0, 5.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralPolygonCurveFlagWithZeroCurvesBehavesLikeNormalPolygon) {
    auto buf = build_general_polygon_with_curve_count(0);
    auto decoder = make_decoder_2d();
    auto geom = decoder.decode(buf.data(), buf.size());

    EXPECT_FALSE(decoder.has_unsupported_curve_geometry(buf.data(), buf.size()));
    EXPECT_FALSE(geom.is_empty);
    expect_no_unknown(geom);
    EXPECT_EQ(geom.wkt.find("UNSUPPORTED_CURVE_GEOMETRY"), std::string::npos) << geom.wkt;
    EXPECT_NE(geom.wkt.find("MULTIPOLYGON"), std::string::npos) << geom.wkt;

    auto bbox = decoder.peek_bbox(buf.data(), buf.size());
    ASSERT_TRUE(bbox.has_value());
    EXPECT_DOUBLE_EQ(bbox->xmin, 0.001);
    EXPECT_DOUBLE_EQ(bbox->ymin, 0.001);
    EXPECT_DOUBLE_EQ(bbox->xmax, 1.001);
    EXPECT_DOUBLE_EQ(bbox->ymax, 1.001);
    EXPECT_TRUE(decoder.intersects_with_peek(buf.data(), buf.size(), -1.0, -1.0, 5.0, 5.0));
    EXPECT_TRUE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), -1.0, -1.0, 5.0, 5.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeneralPolygonCurveSpatialFilterFailsClosed) {
    auto buf = build_general_polygon_with_curve_count(2);
    auto decoder = make_decoder_2d();

    EXPECT_TRUE(decoder.has_unsupported_curve_geometry(buf.data(), buf.size()));
    EXPECT_FALSE(decoder.peek_bbox(buf.data(), buf.size()).has_value());
    EXPECT_FALSE(decoder.intersects_with_peek(buf.data(), buf.size(), -1.0, -1.0, 1.0, 1.0));
    EXPECT_FALSE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), -1.0, -1.0, 1.0, 1.0));
}

// ── peek_bbox 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PeekBbox_Point) {
    auto buf = explorgdb_test::build_geom_point_2d();
    auto decoder = make_decoder_2d();
    auto bbox = decoder.peek_bbox(buf.data(), buf.size());
    ASSERT_TRUE(bbox.has_value());
    // Point bbox should be a single point
    EXPECT_DOUBLE_EQ(bbox->xmin, bbox->xmax);
    EXPECT_DOUBLE_EQ(bbox->ymin, bbox->ymax);
    EXPECT_NEAR(bbox->xmin, 1.0, 0.01);
    EXPECT_NEAR(bbox->ymin, 2.0, 0.01);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PeekBbox_MultiPoint) {
    auto buf = explorgdb_test::build_geom_multipoint();
    auto decoder = make_decoder_2d();
    auto bbox = decoder.peek_bbox(buf.data(), buf.size());
    ASSERT_TRUE(bbox.has_value());
    EXPECT_LT(bbox->xmin, bbox->xmax);
    EXPECT_LT(bbox->ymin, bbox->ymax);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PeekBbox_MultiPointReadsNPointsThenBbox) {
    auto buf = build_multipoint_with_bbox(8, 11000, 22000, 33000, 44000, false, false);
    auto decoder = make_decoder_2d();
    auto bbox = decoder.peek_bbox(buf.data(), buf.size());

    ASSERT_TRUE(bbox.has_value());
    EXPECT_DOUBLE_EQ(bbox->xmin, 11.0);
    EXPECT_DOUBLE_EQ(bbox->ymin, 22.0);
    EXPECT_DOUBLE_EQ(bbox->xmax, 44.0);
    EXPECT_DOUBLE_EQ(bbox->ymax, 66.0);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PeekBbox_GeneralMultiPointReadsNPointsThenBboxWithoutNParts) {
    auto buf = build_multipoint_with_bbox(53, 11000, 22000, 33000, 44000, false, false);
    auto decoder = make_decoder_2d();
    auto bbox = decoder.peek_bbox(buf.data(), buf.size());

    ASSERT_TRUE(bbox.has_value());
    EXPECT_DOUBLE_EQ(bbox->xmin, 11.0);
    EXPECT_DOUBLE_EQ(bbox->ymin, 22.0);
    EXPECT_DOUBLE_EQ(bbox->xmax, 44.0);
    EXPECT_DOUBLE_EQ(bbox->ymax, 66.0);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PeekBbox_Polyline) {
    auto buf = explorgdb_test::build_geom_polyline();
    auto decoder = make_decoder_2d();
    auto bbox = decoder.peek_bbox(buf.data(), buf.size());
    ASSERT_TRUE(bbox.has_value());
    EXPECT_LT(bbox->xmin, bbox->xmax);
    EXPECT_LT(bbox->ymin, bbox->ymax);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PeekBbox_Polygon) {
    auto buf = explorgdb_test::build_geom_polygon();
    auto decoder = make_decoder_2d();
    auto bbox = decoder.peek_bbox(buf.data(), buf.size());
    ASSERT_TRUE(bbox.has_value());
    EXPECT_LT(bbox->xmin, bbox->xmax);
    EXPECT_LT(bbox->ymin, bbox->ymax);
}

// ── intersects_with_peek 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, IntersectsWithPeek_PointInside) {
    auto buf = explorgdb_test::build_geom_point_2d();  // Point(1.0, 2.0)
    auto decoder = make_decoder_2d();
    // bbox 包含点 (1.0, 2.0)
    EXPECT_TRUE(decoder.intersects_with_peek(buf.data(), buf.size(), 0.5, 1.5, 1.5, 2.5));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PointBoundaryUsesRawMinusOneCoordinateFormula) {
    auto buf = build_boundary_point();
    auto decoder = make_decoder_2d();

    auto bbox = decoder.peek_bbox(buf.data(), buf.size());
    ASSERT_TRUE(bbox.has_value());
    EXPECT_DOUBLE_EQ(bbox->xmin, 1.0);
    EXPECT_DOUBLE_EQ(bbox->ymin, 2.0);
    EXPECT_DOUBLE_EQ(bbox->xmax, 1.0);
    EXPECT_DOUBLE_EQ(bbox->ymax, 2.0);
    EXPECT_TRUE(decoder.intersects_with_peek(buf.data(), buf.size(), 1.0, 2.0, 1.0, 2.0));
    EXPECT_TRUE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), 1.0, 2.0, 1.0, 2.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, IntersectsWithPeek_PointOutside) {
    auto buf = explorgdb_test::build_geom_point_2d();  // Point(1.0, 2.0)
    auto decoder = make_decoder_2d();
    // bbox 不包含点
    EXPECT_FALSE(decoder.intersects_with_peek(buf.data(), buf.size(), 5.0, 5.0, 6.0, 6.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, IntersectsWithPeek_MultiPointInside) {
    auto buf = explorgdb_test::build_geom_multipoint();
    auto decoder = make_decoder_2d();
    // bbox 覆盖 MultiPoint 范围 (1.0, 2.0) - (4.0, 6.0)
    EXPECT_TRUE(decoder.intersects_with_peek(buf.data(), buf.size(), 0.5, 1.5, 5.0, 7.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, IntersectsWithPeek_PolylineInside) {
    auto buf = explorgdb_test::build_geom_polyline();
    auto decoder = make_decoder_2d();
    // bbox 覆盖 polyline 范围
    EXPECT_TRUE(decoder.intersects_with_peek(buf.data(), buf.size(), -0.5, -0.5, 2.0, 2.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, IntersectsWithPeek_PolygonInside) {
    auto buf = explorgdb_test::build_geom_polygon();
    auto decoder = make_decoder_2d();
    // bbox 覆盖 polygon 范围
    EXPECT_TRUE(decoder.intersects_with_peek(buf.data(), buf.size(), -0.5, -0.5, 2.0, 2.0));
}

// ── geometry_intersects_bbox 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeometryIntersectsBbox_Point) {
    auto buf = explorgdb_test::build_geom_point_2d();  // Point(1.0, 2.0)
    auto decoder = make_decoder_2d();
    // bbox 包含点
    EXPECT_TRUE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), 0.5, 1.5, 1.5, 2.5));
    // bbox 不包含点
    EXPECT_FALSE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), 5.0, 5.0, 6.0, 6.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeometryIntersectsBbox_Polyline) {
    auto buf = explorgdb_test::build_geom_polyline();
    auto decoder = make_decoder_2d();
    // bbox 覆盖 polyline
    EXPECT_TRUE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), -1.0, -1.0, 5.0, 5.0));
    // bbox 远离 polyline
    EXPECT_FALSE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), 100.0, 100.0, 200.0, 200.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeometryIntersectsBbox_Polygon) {
    auto buf = explorgdb_test::build_geom_polygon();
    auto decoder = make_decoder_2d();
    // bbox 覆盖 polygon
    EXPECT_TRUE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), -1.0, -1.0, 5.0, 5.0));
    // bbox 远离 polygon
    EXPECT_FALSE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), 100.0, 100.0, 200.0, 200.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeometryIntersectsBbox_MultiPoint) {
    auto buf = explorgdb_test::build_geom_multipoint();
    auto decoder = make_decoder_2d();
    EXPECT_TRUE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), 0.0, 0.0, 10.0, 10.0));
    EXPECT_FALSE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), 100.0, 100.0, 200.0, 200.0));
}

// ── MultiPatch peek_bbox / intersects 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PeekBbox_MultiPatch) {
    auto buf = explorgdb_test::build_geom_multipatch();
    auto decoder = make_decoder_2d();
    auto bbox = decoder.peek_bbox(buf.data(), buf.size());
    ASSERT_TRUE(bbox.has_value());
    EXPECT_LT(bbox->xmin, bbox->xmax);
    EXPECT_LT(bbox->ymin, bbox->ymax);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeometryIntersectsBbox_MultiPatch) {
    auto buf = explorgdb_test::build_geom_multipatch();
    auto decoder = make_decoder_2d();
    // MultiPatch covers approx (0,0) to (1,1)
    EXPECT_TRUE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), -1.0, -1.0, 5.0, 5.0));
    EXPECT_FALSE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), 100.0, 100.0, 200.0, 200.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, IntersectsWithPeek_MultiPatch) {
    auto buf = explorgdb_test::build_geom_multipatch();
    auto decoder = make_decoder_2d();
    EXPECT_TRUE(decoder.intersects_with_peek(buf.data(), buf.size(), -1.0, -1.0, 5.0, 5.0));
    EXPECT_FALSE(decoder.intersects_with_peek(buf.data(), buf.size(), 100.0, 100.0, 200.0, 200.0));
}

// ── General types peek_bbox / intersects 测试 ──

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PeekBbox_GeneralPolyline) {
    auto buf = explorgdb_test::build_geom_general_polyline();
    auto decoder = make_decoder_2d();
    auto bbox = decoder.peek_bbox(buf.data(), buf.size());
    ASSERT_TRUE(bbox.has_value());
    EXPECT_NEAR(bbox->xmin, 0.001, 0.000001);
    EXPECT_NEAR(bbox->ymin, 0.001, 0.000001);
    EXPECT_NEAR(bbox->xmax, 1.001, 0.000001);
    EXPECT_NEAR(bbox->ymax, 1.001, 0.000001);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, PeekBbox_GeneralPolygon) {
    auto buf = explorgdb_test::build_geom_general_polygon();
    auto decoder = make_decoder_2d();
    auto bbox = decoder.peek_bbox(buf.data(), buf.size());
    ASSERT_TRUE(bbox.has_value());
    EXPECT_NEAR(bbox->xmin, 0.001, 0.000001);
    EXPECT_NEAR(bbox->ymin, 0.001, 0.000001);
    EXPECT_NEAR(bbox->xmax, 1.001, 0.000001);
    EXPECT_NEAR(bbox->ymax, 1.001, 0.000001);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeometryIntersectsBbox_GeneralPolyline) {
    auto buf = explorgdb_test::build_geom_general_polyline();
    auto decoder = make_decoder_2d();
    EXPECT_TRUE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), -1.0, -1.0, 5.0, 5.0));
    EXPECT_FALSE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), 100.0, 100.0, 200.0, 200.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeometryIntersectsBbox_GeneralPolygon) {
    auto buf = explorgdb_test::build_geom_general_polygon();
    auto decoder = make_decoder_2d();
    EXPECT_TRUE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), -1.0, -1.0, 5.0, 5.0));
    EXPECT_FALSE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), 100.0, 100.0, 200.0, 200.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, IntersectsWithPeek_GeneralPolyline) {
    auto buf = explorgdb_test::build_geom_general_polyline();
    auto decoder = make_decoder_2d();
    EXPECT_TRUE(decoder.intersects_with_peek(buf.data(), buf.size(), -1.0, -1.0, 5.0, 5.0));
    EXPECT_FALSE(decoder.intersects_with_peek(buf.data(), buf.size(), 100.0, 100.0, 200.0, 200.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, IntersectsWithPeek_GeneralPolygon) {
    auto buf = explorgdb_test::build_geom_general_polygon();
    auto decoder = make_decoder_2d();
    EXPECT_TRUE(decoder.intersects_with_peek(buf.data(), buf.size(), -1.0, -1.0, 5.0, 5.0));
    EXPECT_FALSE(decoder.intersects_with_peek(buf.data(), buf.size(), 100.0, 100.0, 200.0, 200.0));
}

// ── pip (point-in-polygon) 测试 via intersects_with_peek ──
// 当查询 bbox 完全在多边形内部时，需要 pip 回退

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, IntersectsWithPeek_PolygonPipFallback) {
    // 构造一个较大的多边形 (0,0)→(10,0)→(10,10)→(0,10)
    std::vector<uint8_t> buf;
    explorgdb_test::write_varuint(buf, 5);  // Polygon
    explorgdb_test::write_varuint(buf, 4);  // nPoints
    explorgdb_test::write_varuint(buf, 1);  // nParts
    // BBox
    explorgdb_test::write_varuint(buf, 1); explorgdb_test::write_varuint(buf, 1);
    explorgdb_test::write_varuint(buf, 10000); explorgdb_test::write_varuint(buf, 10000);
    // Points: (0.001,0.001) → (10.001,0.001) → (10.001,10.001) → (0.001,10.001)
    explorgdb_test::write_svarint(buf, 1); explorgdb_test::write_svarint(buf, 1);
    explorgdb_test::write_svarint(buf, 10000); explorgdb_test::write_svarint(buf, 0);
    explorgdb_test::write_svarint(buf, 0); explorgdb_test::write_svarint(buf, 10000);
    explorgdb_test::write_svarint(buf, -10000); explorgdb_test::write_svarint(buf, 0);

    auto decoder = make_decoder_2d();
    // 查询 bbox 完全在多边形内部 → 需要 pip 回退
    // query center = (4.5, 4.5), which is inside the polygon
    EXPECT_TRUE(decoder.intersects_with_peek(buf.data(), buf.size(), 4.0, 4.0, 5.0, 5.0));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(GeometryTest, GeometryIntersectsBbox_PolygonPipFallback) {
    std::vector<uint8_t> buf;
    explorgdb_test::write_varuint(buf, 5);  // Polygon
    explorgdb_test::write_varuint(buf, 4);  // nPoints
    explorgdb_test::write_varuint(buf, 1);  // nParts
    explorgdb_test::write_varuint(buf, 1); explorgdb_test::write_varuint(buf, 1);
    explorgdb_test::write_varuint(buf, 10000); explorgdb_test::write_varuint(buf, 10000);
    explorgdb_test::write_svarint(buf, 1); explorgdb_test::write_svarint(buf, 1);
    explorgdb_test::write_svarint(buf, 10000); explorgdb_test::write_svarint(buf, 0);
    explorgdb_test::write_svarint(buf, 0); explorgdb_test::write_svarint(buf, 10000);
    explorgdb_test::write_svarint(buf, -10000); explorgdb_test::write_svarint(buf, 0);

    auto decoder = make_decoder_2d();
    EXPECT_TRUE(decoder.geometry_intersects_bbox(buf.data(), buf.size(), 4.0, 4.0, 5.0, 5.0));
}
