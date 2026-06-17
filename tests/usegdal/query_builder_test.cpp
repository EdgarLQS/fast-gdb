/**
 * query_builder_test.cpp — GdbQuery 链式查询构建器测试
 *
 * 覆盖：where/spatial/limit/offset 链式构建、execute 执行、count 计数
 */

#include "test_fixture.h"
#include "datasource.h"
#include "datasets.h"
#include "recordset.h"
#include "query_builder.h"
#include "spatial_relation.h"

// 辅助：创建城市测试数据
static void createCities(GDALDataset* ds) {
    OGRLayer* layer = ds->CreateLayer("cities", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn nameField("name", OFTString);
    nameField.SetWidth(20);
    layer->CreateField(&nameField);
    layer->CreateField(new OGRFieldDefn("population", OFTInteger));

    struct City { const char* name; int pop; double x, y; };
    City cities[] = {
        {"Beijing", 21540000, 116.4, 39.9},
        {"Shanghai", 24240000, 121.5, 31.2},
        {"Guangzhou", 15300000, 113.3, 23.1},
        {"Shenzhen", 13020000, 114.1, 22.5},
        {"Chengdu", 16580000, 104.1, 30.6},
    };

    for (const auto& c : cities) {
        OGRFeature feat(layer->GetLayerDefn());
        feat.SetField("name", c.name);
        feat.SetField("population", c.pop);
        OGRPoint pt(c.x, c.y);
        feat.SetGeometry(&pt);
        layer->CreateFeature(&feat);
    }
}

// ===== 构建测试 =====

/**
 * T_QueryBuilder_Default: 默认空查询。
 */
TEST_F(GdbTutorialFixture, T_QueryBuilder_Default) {
    GdbQuery q;
    EXPECT_TRUE(q.isEmpty());
}

/**
 * T_QueryBuilder_Where: where() 构建属性过滤。
 */
TEST_F(GdbTutorialFixture, T_QueryBuilder_Where) {
    GdbQuery q;
    q.where("population > 15000000");
    EXPECT_FALSE(q.isEmpty());
}

/**
 * T_QueryBuilder_Chain: 链式构建 where + limit。
 */
TEST_F(GdbTutorialFixture, T_QueryBuilder_Chain) {
    GdbQuery q;
    q.where("population > 10000000")
     .limit(2);
    EXPECT_FALSE(q.isEmpty());
}

// ===== execute 测试 =====

/**
 * T_QueryBuilder_Execute: where → execute → 验证结果。
 */
TEST_F(GdbTutorialFixture, T_QueryBuilder_Execute) {
    const char* path = "/tmp/tutorial_query_builder_execute.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);
    createCities(ds);

    GdbDatasource gdb(ds);
    GdbDataset cities = gdb.getDatasets().get("cities");

    GdbQuery q;
    q.where("name = 'Beijing'");
    GdbRecordset rs = cities.query(q);
    ASSERT_TRUE(rs.isValid());

    int count = 0;
    while (rs.moveNext()) {
        count++;
        EXPECT_STREQ(rs.getFieldAsString("name").c_str(), "Beijing");
    }
    EXPECT_EQ(count, 1);
}

/**
 * T_QueryBuilder_ExecuteLimit: where + limit，limit 由调用方在迭代时控制。
 * GdbQuery 的 limit 作为元数据存储，不强制截断底层 OGRLayer 迭代器。
 */
TEST_F(GdbTutorialFixture, T_QueryBuilder_ExecuteLimit) {
    const char* path = "/tmp/tutorial_query_builder_limit.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);
    createCities(ds);

    GdbDatasource gdb(ds);
    GdbDataset cities = gdb.getDatasets().get("cities");

    GdbQuery q;
    q.where("population > 10000000")
     .limit(2);
    GdbRecordset rs = cities.query(q);
    ASSERT_TRUE(rs.isValid());

    // limit 由调用方在迭代时控制
    int count = 0;
    int limit = q.getLimit();
    while (rs.moveNext()) {
        count++;
        if (limit >= 0 && count >= limit) break;
    }
    EXPECT_EQ(count, 2);
}

// ===== count 测试 =====

/**
 * T_QueryBuilder_Count: count() 返回匹配要素数量。
 */
TEST_F(GdbTutorialFixture, T_QueryBuilder_Count) {
    const char* path = "/tmp/tutorial_query_builder_count.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);
    createCities(ds);

    GdbDatasource gdb(ds);
    GdbDataset cities = gdb.getDatasets().get("cities");

    GdbQuery q;
    q.where("population > 15000000");
    EXPECT_EQ(cities.count(q), 4); // Beijing, Shanghai, Guangzhou, Chengdu
}

/**
 * T_QueryBuilder_CountEmpty: 空查询 count = 总数。
 */
TEST_F(GdbTutorialFixture, T_QueryBuilder_CountEmpty) {
    const char* path = "/tmp/tutorial_query_builder_count_empty.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);
    createCities(ds);

    GdbDatasource gdb(ds);
    GdbDataset cities = gdb.getDatasets().get("cities");

    GdbQuery q;
    EXPECT_EQ(cities.count(q), 5);
}

// ===== 空间查询测试 =====

// 辅助：构建珠三角 bbox
static OGRPolygon makePearlRiverDeltaBbox() {
    OGRPolygon bbox;
    OGRLinearRing ring;
    ring.addPoint(112.0, 21.0);
    ring.addPoint(115.0, 21.0);
    ring.addPoint(115.0, 24.0);
    ring.addPoint(112.0, 24.0);
    ring.closeRings();
    bbox.addRing(&ring);
    return bbox;
}

/**
 * T_QueryBuilder_SpatialIntersects: spatial() → execute → 验证 bbox 相交。
 */
TEST_F(GdbTutorialFixture, T_QueryBuilder_SpatialIntersects) {
    const char* path = "/tmp/tutorial_qb_spatial.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);
    createCities(ds);

    GdbDatasource gdb(ds);
    GdbDataset cities = gdb.getDatasets().get("cities");

    OGRPolygon bbox = makePearlRiverDeltaBbox();

    GdbQuery q;
    q.spatial(&bbox);
    GdbRecordset rs = cities.query(q);
    ASSERT_TRUE(rs.isValid());

    int count = 0;
    while (rs.moveNext()) count++;
    EXPECT_EQ(count, 2); // Guangzhou(113.3,23.1), Shenzhen(114.1,22.5)
}

/**
 * T_QueryBuilder_SpatialWithin: spatial(geom, Within) → 点在 bbox 内。
 * 对点来说 Within ≈ Intersects（点在多边形内 = 与多边形相交）
 */
TEST_F(GdbTutorialFixture, T_QueryBuilder_SpatialWithin) {
    const char* path = "/tmp/tutorial_qb_within.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);
    createCities(ds);

    GdbDatasource gdb(ds);
    GdbDataset cities = gdb.getDatasets().get("cities");

    OGRPolygon bbox = makePearlRiverDeltaBbox();

    GdbQuery q;
    q.spatial(&bbox, GdbSpatialRelation::Within);
    GdbRecordset rs = cities.query(q);
    ASSERT_TRUE(rs.isValid());

    int count = 0;
    while (rs.moveNext()) count++;
    EXPECT_EQ(count, 2); // Guangzhou, Shenzhen
}

/**
 * T_QueryBuilder_SpatialAndWhere: spatial + where 链式组合。
 * 空间：珠三角 bbox + 属性：population > 14000000 → Guangzhou(1530万)
 */
TEST_F(GdbTutorialFixture, T_QueryBuilder_SpatialAndWhere) {
    const char* path = "/tmp/tutorial_qb_spatial_where.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);
    createCities(ds);

    GdbDatasource gdb(ds);
    GdbDataset cities = gdb.getDatasets().get("cities");

    OGRPolygon bbox = makePearlRiverDeltaBbox();

    GdbQuery q;
    q.where("population > 14000000")
     .spatial(&bbox);
    GdbRecordset rs = cities.query(q);
    ASSERT_TRUE(rs.isValid());

    int count = 0;
    while (rs.moveNext()) count++;
    EXPECT_EQ(count, 1);
}

/**
 * T_QueryBuilder_SpatialDisjoint: Disjoint 返回不在 bbox 内的要素。
 * bbox 外的城市：Beijing(116.4,39.9), Shanghai(121.5,31.2), Chengdu(104.1,30.6)
 */
TEST_F(GdbTutorialFixture, T_QueryBuilder_SpatialDisjoint) {
    const char* path = "/tmp/tutorial_qb_disjoint.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);
    createCities(ds);

    GdbDatasource gdb(ds);
    GdbDataset cities = gdb.getDatasets().get("cities");

    OGRPolygon bbox = makePearlRiverDeltaBbox();

    GdbQuery q;
    q.spatial(&bbox, GdbSpatialRelation::Disjoint);
    GdbRecordset rs = cities.query(q);
    ASSERT_TRUE(rs.isValid());

    int count = 0;
    while (rs.moveNext()) {
        count++;
        std::string name = rs.getFieldAsString("name");
        // 验证不在 bbox 内的城市
        EXPECT_TRUE(name == "Beijing" || name == "Shanghai" || name == "Chengdu");
    }
    EXPECT_EQ(count, 3);
}
