/**
 * datasets_write_test.cpp — GdbDatasets/GdbDataset 图层管理与写入测试
 *
 * 覆盖：createLayer、remove、空几何图层创建
 */

#include "test_fixture.h"
#include "datasource.h"
#include "datasets.h"
#include "recordset.h"

// ===== 图层创建测试 =====

/**
 * T_Datasets_CreateLayer: create 创建新图层后验证名称和几何类型。
 */
TEST_F(GdbTutorialFixture, T_Datasets_CreateLayer) {
    GDALDataset* ds = createGdb("/tmp/tutorial_datasets_create.gdb");
    ASSERT_NE(ds, nullptr);

    GdbDatasource gdb(ds);
    GdbDatasets datasets = gdb.getDatasets();

    // 初始 0 个图层
    EXPECT_EQ(datasets.getCount(), 0);

    // 创建 Point 图层
    GdbDataset newLayer = datasets.create("my_points", wkbPoint);
    ASSERT_TRUE(newLayer.isValid());
    EXPECT_STREQ(newLayer.getName().c_str(), "my_points");
    EXPECT_EQ(newLayer.getGeometryType(), wkbPoint);

    // 创建 Polygon 图层（OpenFileGDB 自动转为 multipolygon）
    GdbDataset polyLayer = datasets.create("my_polygons", wkbPolygon);
    ASSERT_TRUE(polyLayer.isValid());
    auto polyType = polyLayer.getGeometryType();
    EXPECT_TRUE(polyType == wkbPolygon || polyType == wkbMultiPolygon);

    // 验证图层计数
    EXPECT_EQ(datasets.getCount(), 2);

    // 通过名称查找
    GdbDataset found = datasets.get("my_points");
    EXPECT_TRUE(found.isValid());
    EXPECT_STREQ(found.getName().c_str(), "my_points");

    // 查找不存在的图层
    GdbDataset notFound = datasets.get("nonexistent");
    EXPECT_FALSE(notFound.isValid());
}

/**
 * T_Datasets_CreateWithSrs: create 时指定空间参考。
 */
TEST_F(GdbTutorialFixture, T_Datasets_CreateWithSrs) {
    GDALDataset* ds = createGdb("/tmp/tutorial_datasets_create_srs.gdb");
    ASSERT_NE(ds, nullptr);

    GdbDatasource gdb(ds);
    GdbDatasets datasets = gdb.getDatasets();

    OGRSpatialReference srs;
    srs.importFromEPSG(4326);

    GdbDataset layer = datasets.create("geo_points", wkbPoint, &srs);
    ASSERT_TRUE(layer.isValid());

    // 验证图层 SRS
    OGRLayer* native = layer.getNative();
    ASSERT_NE(native, nullptr);
    OGRSpatialReference* layerSrs = native->GetSpatialRef();
    ASSERT_NE(layerSrs, nullptr);
    EXPECT_EQ(layerSrs->GetAuthorityCode(nullptr), std::string("4326"));
}

// ===== 图层删除测试 =====

/**
 * T_Datasets_RemoveLayer: remove 删除图层后验证不存在。
 */
TEST_F(GdbTutorialFixture, T_Datasets_RemoveLayer) {
    GDALDataset* ds = createGdb("/tmp/tutorial_datasets_remove.gdb");
    ASSERT_NE(ds, nullptr);

    // 创建两个图层
    ds->CreateLayer("keep_me", nullptr, wkbPoint, nullptr);
    ds->CreateLayer("remove_me", nullptr, wkbLineString, nullptr);

    GdbDatasource gdb(ds);
    GdbDatasets datasets = gdb.getDatasets();
    EXPECT_EQ(datasets.getCount(), 2);

    // 删除 remove_me
    EXPECT_TRUE(datasets.remove("remove_me"));
    EXPECT_EQ(datasets.getCount(), 1);

    // 验证 remove_me 不存在
    GdbDataset gone = datasets.get("remove_me");
    EXPECT_FALSE(gone.isValid());

    // 验证 keep_me 仍存在
    GdbDataset kept = datasets.get("keep_me");
    EXPECT_TRUE(kept.isValid());

    // 删除不存在的图层应失败
    EXPECT_FALSE(datasets.remove("nonexistent"));
}

// ===== 写入后持久化验证 =====

/**
 * T_Datasets_WriteAndReopen: 创建图层 → 写入要素 → 关闭 → 重开验证。
 */
TEST_F(GdbTutorialFixture, T_Datasets_WriteAndReopen) {
    const char* path = "/tmp/tutorial_datasets_reopen.gdb";
    // 直接用 fixture 创建 GDB，但不通过 GdbDatasource 持有 ds
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    // 创建图层并添加字段
    OGRLayer* layer = ds->CreateLayer("persistent_points", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);
    layer->CreateField(new OGRFieldDefn("label", OFTString));
    layer->CreateField(new OGRFieldDefn("priority", OFTInteger));

    // 写入要素（通过原生 API，因为我们要手动关闭）
    OGRFeature f1(layer->GetLayerDefn());
    f1.SetField("label", "High");
    f1.SetField("priority", 1);
    OGRPoint p1(1.0, 2.0);
    f1.SetGeometry(&p1);
    layer->CreateFeature(&f1);

    OGRFeature f2(layer->GetLayerDefn());
    f2.SetField("label", "Low");
    f2.SetField("priority", 5);
    OGRPoint p2(3.0, 4.0);
    f2.SetGeometry(&p2);
    layer->CreateFeature(&f2);

    // 关闭 GDB
    GDALClose(ds);
    // 清除 fixture 的自动清理路径（因为我们已手动关闭）
    m_gdbPath.clear();

    // 用组件 API 重开验证
    GdbDatasource gdb2;
    ASSERT_TRUE(gdb2.openExisting(path));
    GdbDataset layer2 = gdb2.getDatasets().get("persistent_points");
    ASSERT_TRUE(layer2.isValid());
    EXPECT_EQ(layer2.getFeatureCount(), 2);
    EXPECT_EQ(layer2.getFieldCount(), 2);

    GdbRecordset rs = layer2.getRecordset();
    std::vector<std::string> labels;
    std::vector<int> priorities;
    while (rs.moveNext()) {
        labels.push_back(rs.getFieldAsString("label"));
        priorities.push_back(rs.getFieldAsInteger("priority"));
    }
    EXPECT_EQ(labels.size(), 2);
    EXPECT_EQ(labels[0], "High");
    EXPECT_EQ(labels[1], "Low");
    EXPECT_EQ(priorities[0], 1);
    EXPECT_EQ(priorities[1], 5);
}
