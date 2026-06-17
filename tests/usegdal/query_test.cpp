/**
 * query_test.cpp — GdbQueryParameter 属性查询 + 空间查询 测试
 *
 * 覆盖：QueryParameter 构建、属性过滤（等于、比较、LIKE、AND/OR、无匹配）、
 *       空间过滤（几何对象、矩形范围）、
 *       组合过滤、要素计数、能力检测
 */

#include "test_fixture.h"
#include "datasource.h"
#include "datasets.h"
#include "recordset.h"
#include "query_parameter.h"

// 辅助：创建一个带 name(int) + x(double) + y(double) 字段 + Point 几何的测试 GDB
static GDALDataset* createGdbRaw(const char* path) {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!drv) return nullptr;
    GDALDeleteDataset(drv, path);
    return (GDALDataset*)drv->Create(path, 0, 0, 0, GDT_Unknown, nullptr);
}

static void createQueryTestData(const char* path) {
    GDALDataset* ds = createGdbRaw(path);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("cities", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn nameField("name", OFTString);
    nameField.SetWidth(20);
    layer->CreateField(&nameField);

    OGRFieldDefn popField("population", OFTInteger);
    layer->CreateField(&popField);

    OGRFieldDefn xField("lon", OFTReal);
    layer->CreateField(&xField);

    OGRFieldDefn yField("lat", OFTReal);
    layer->CreateField(&yField);

    struct City { const char* name; int pop; double lon, lat; };
    City cities[] = {
        {"Beijing", 21540000, 116.4, 39.9},
        {"Shanghai", 24240000, 121.5, 31.2},
        {"Guangzhou", 15300000, 113.3, 23.1},
        {"Shenzhen", 13020000, 114.1, 22.5},
        {"Chengdu", 16580000, 104.1, 30.6},
        {"Hangzhou", 10360000, 120.2, 30.3},
    };

    for (const auto& c : cities) {
        OGRFeature feat(layer->GetLayerDefn());
        feat.SetField("name", c.name);
        feat.SetField("population", c.pop);
        feat.SetField("lon", c.lon);
        feat.SetField("lat", c.lat);
        OGRPoint pt(c.lon, c.lat);
        feat.SetGeometry(&pt);
        layer->CreateFeature(&feat);
    }
    GDALClose(ds);
}

// ===== GdbQueryParameter 构建测试 =====

/**
 * T_QueryParam_Defaults: 验证默认值为空。
 */
TEST_F(GdbTutorialFixture, T_QueryParam_Defaults) {
    GdbQueryParameter param;
    EXPECT_TRUE(param.isEmpty());
    EXPECT_TRUE(param.getAttributeFilter().empty());
    EXPECT_EQ(param.getSpatialFilter(), nullptr);
    EXPECT_TRUE(param.hasGeometry()); // 默认返回几何
    EXPECT_TRUE(param.getResultFields().empty());
}

/**
 * T_QueryParam_BuildAttribute: 设置属性过滤后 isEmpty=false。
 */
TEST_F(GdbTutorialFixture, T_QueryParam_BuildAttribute) {
    GdbQueryParameter param;
    param.setAttributeFilter("population > 10000000");
    EXPECT_FALSE(param.isEmpty());
    EXPECT_STREQ(param.getAttributeFilter().c_str(), "population > 10000000");
}

/**
 * T_QueryParam_BuildSpatial: 设置空间过滤后 isEmpty=false。
 */
TEST_F(GdbTutorialFixture, T_QueryParam_BuildSpatial) {
    GdbQueryParameter param;
    OGRPoint pt(0.0, 0.0);
    param.setSpatialFilter(&pt);
    EXPECT_FALSE(param.isEmpty());
    EXPECT_NE(param.getSpatialFilter(), nullptr);
}

/**
 * T_QueryParam_ResultFields: 设置结果字段集合。
 */
TEST_F(GdbTutorialFixture, T_QueryParam_ResultFields) {
    GdbQueryParameter param;
    param.setResultFields({"name", "population"});
    EXPECT_EQ(param.getResultFields().size(), 2);
    EXPECT_STREQ(param.getResultFields()[0].c_str(), "name");
    EXPECT_STREQ(param.getResultFields()[1].c_str(), "population");
}

// ===== 属性查询测试 =====

/**
 * T_Query_AttributeEquality: WHERE name = 'Beijing' → 只返回 1 条。
 */
TEST_F(GdbTutorialFixture, T_Query_AttributeEquality) {
    const char* path = "/tmp/tutorial_query_equality.gdb";
    createQueryTestData(path);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset cities = gdb.getDatasets().get("cities");
    ASSERT_TRUE(cities.isValid());

    GdbQueryParameter param;
    param.setAttributeFilter("name = 'Beijing'");
    GdbRecordset rs = cities.query(param);
    ASSERT_TRUE(rs.isValid());

    int count = 0;
    while (rs.moveNext()) {
        count++;
        EXPECT_STREQ(rs.getFieldAsString("name").c_str(), "Beijing");
        EXPECT_EQ(rs.getFieldAsInteger("population"), 21540000);
    }
    EXPECT_EQ(count, 1);
}

/**
 * T_Query_AttributeComparison: WHERE population > 15000000 → 返回 4 条。
 */
TEST_F(GdbTutorialFixture, T_Query_AttributeComparison) {
    const char* path = "/tmp/tutorial_query_comparison.gdb";
    createQueryTestData(path);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset cities = gdb.getDatasets().get("cities");

    GdbQueryParameter param;
    param.setAttributeFilter("population > 15000000");
    GdbRecordset rs = cities.query(param);
    ASSERT_TRUE(rs.isValid());

    int count = 0;
    while (rs.moveNext()) {
        count++;
        EXPECT_GT(rs.getFieldAsInteger("population"), 15000000);
    }
    EXPECT_EQ(count, 4); // Beijing, Shanghai, Guangzhou, Chengdu
}

/**
 * T_Query_AttributeLike: WHERE name LIKE 'S%' → 返回 Shanghai, Shenzhen。
 */
TEST_F(GdbTutorialFixture, T_Query_AttributeLike) {
    const char* path = "/tmp/tutorial_query_like.gdb";
    createQueryTestData(path);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset cities = gdb.getDatasets().get("cities");

    GdbQueryParameter param;
    param.setAttributeFilter("name LIKE 'S%'");
    GdbRecordset rs = cities.query(param);
    ASSERT_TRUE(rs.isValid());

    int count = 0;
    while (rs.moveNext()) {
        count++;
        std::string name = rs.getFieldAsString("name");
        EXPECT_TRUE(name[0] == 'S');
    }
    EXPECT_EQ(count, 2);
}

/**
 * T_Query_AttributeAndOr: 复合条件 population > 14000000 AND name LIKE 'S%' → Shanghai。
 */
TEST_F(GdbTutorialFixture, T_Query_AttributeAndOr) {
    const char* path = "/tmp/tutorial_query_andor.gdb";
    createQueryTestData(path);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset cities = gdb.getDatasets().get("cities");

    GdbQueryParameter param;
    param.setAttributeFilter("population > 14000000 AND name LIKE 'S%'");
    GdbRecordset rs = cities.query(param);
    ASSERT_TRUE(rs.isValid());

    int count = 0;
    while (rs.moveNext()) {
        count++;
        EXPECT_STREQ(rs.getFieldAsString("name").c_str(), "Shanghai");
    }
    EXPECT_EQ(count, 1);
}

/**
 * T_Query_AttributeNoMatch: WHERE name = 'Nonexistent' → 空结果集。
 */
TEST_F(GdbTutorialFixture, T_Query_AttributeNoMatch) {
    const char* path = "/tmp/tutorial_query_nomatch.gdb";
    createQueryTestData(path);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset cities = gdb.getDatasets().get("cities");

    GdbQueryParameter param;
    param.setAttributeFilter("name = 'Nonexistent'");
    GdbRecordset rs = cities.query(param);
    ASSERT_TRUE(rs.isValid());

    EXPECT_FALSE(rs.moveNext());
    EXPECT_TRUE(rs.isEOF());
}

// ===== 空间查询测试 =====

/**
 * T_Query_SpatialIntersects: 空间过滤 — 只返回与过滤矩形相交的要素。
 */
TEST_F(GdbTutorialFixture, T_Query_SpatialIntersects) {
    const char* path = "/tmp/tutorial_query_spatial.gdb";
    createQueryTestData(path);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset cities = gdb.getDatasets().get("cities");
    ASSERT_TRUE(cities.isValid());

    // 矩形过滤：覆盖珠三角区域（Guangzhou + Shenzhen）
    OGRPolygon bbox;
    OGRLinearRing ring;
    ring.addPoint(112.0, 21.0);
    ring.addPoint(115.0, 21.0);
    ring.addPoint(115.0, 24.0);
    ring.addPoint(112.0, 24.0);
    ring.closeRings();
    bbox.addRing(&ring);

    GdbQueryParameter param;
    param.setSpatialFilter(&bbox);
    GdbRecordset rs = cities.query(param);
    ASSERT_TRUE(rs.isValid());

    int count = 0;
    while (rs.moveNext()) {
        count++;
    }
    EXPECT_EQ(count, 2); // Guangzhou, Shenzhen
}

/**
 * T_Query_SpatialRect: 矩形范围空间过滤（便捷方法）。
 */
TEST_F(GdbTutorialFixture, T_Query_SpatialRect) {
    const char* path = "/tmp/tutorial_query_spatial_rect.gdb";
    createQueryTestData(path);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset cities = gdb.getDatasets().get("cities");

    GdbQueryParameter param;
    param.setSpatialFilterRect(119.0, 29.0, 123.0, 33.0);
    GdbRecordset rs = cities.query(param);
    ASSERT_TRUE(rs.isValid());

    int count = 0;
    while (rs.moveNext()) {
        count++;
    }
    EXPECT_EQ(count, 2); // Shanghai, Hangzhou
}

// ===== 组合查询测试 =====

/**
 * T_Query_Combined: 属性 + 空间组合过滤。
 * 空间：珠三角矩形 + 属性：population > 14000000 → 只有 Guangzhou。
 */
TEST_F(GdbTutorialFixture, T_Query_Combined) {
    const char* path = "/tmp/tutorial_query_combined.gdb";
    createQueryTestData(path);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset cities = gdb.getDatasets().get("cities");

    OGRPolygon bbox;
    OGRLinearRing ring;
    ring.addPoint(112.0, 21.0);
    ring.addPoint(115.0, 21.0);
    ring.addPoint(115.0, 24.0);
    ring.addPoint(112.0, 24.0);
    ring.closeRings();
    bbox.addRing(&ring);

    GdbQueryParameter param;
    param.setAttributeFilter("population > 14000000");
    param.setSpatialFilter(&bbox);
    GdbRecordset rs = cities.query(param);
    ASSERT_TRUE(rs.isValid());

    int count = 0;
    while (rs.moveNext()) {
        count++;
        EXPECT_STREQ(rs.getFieldAsString("name").c_str(), "Guangzhou");
    }
    EXPECT_EQ(count, 1);
}

// ===== 要素计数测试 =====

/**
 * T_Query_FeatureCount: 过滤后要素计数验证。
 */
TEST_F(GdbTutorialFixture, T_Query_FeatureCount) {
    const char* path = "/tmp/tutorial_query_count.gdb";
    createQueryTestData(path);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset cities = gdb.getDatasets().get("cities");

    // 无过滤计数
    EXPECT_EQ(cities.getFeatureCount(), 6);

    // 属性过滤后计数
    GdbQueryParameter param;
    param.setAttributeFilter("population > 20000000");
    EXPECT_EQ(cities.getFeatureCountFiltered(param), 2); // Beijing, Shanghai

    // 空参数 = 全部
    GdbQueryParameter emptyParam;
    EXPECT_EQ(cities.getFeatureCountFiltered(emptyParam), 6);
}

// ===== 能力检测 =====

/**
 * T_Query_Capability: 查询能力检测。
 */
TEST_F(GdbTutorialFixture, T_Query_Capability) {
    const char* path = "/tmp/tutorial_query_capability.gdb";
    createQueryTestData(path);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset cities = gdb.getDatasets().get("cities");

    // OpenFileGDB 支持属性过滤和快速空间过滤
    EXPECT_TRUE(cities.supportsAttributeFilter());
    // 空间过滤能力取决于是否有 .spx 或内存 SPI
    bool hasSpatial = cities.supportsFastSpatialFilter();
    (void)hasSpatial;

    // 快速计数能力
    bool hasFastCount = cities.supportsFastFeatureCount();
    (void)hasFastCount;
}
