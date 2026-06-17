/**
 * datasets_test.cpp — GdbDatasets / GdbDataset 单元测试
 *
 * 覆盖：图层枚举、按名查找、字段元信息、几何类型
 */

#include "test_fixture.h"
#include "datasource.h"
#include "datasets.h"

/**
 * T_Datasets_EnumerateLayers: 验证枚举所有图层。
 */
TEST_F(GdbTutorialFixture, T_Datasets_EnumerateLayers) {
    const char* path = "/tmp/tutorial_datasets_enum.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    ds->CreateLayer("layer_a", nullptr, wkbPoint, nullptr);
    ds->CreateLayer("layer_b", nullptr, wkbLineString, nullptr);
    ds->CreateLayer("layer_c", nullptr, wkbPolygon, nullptr);
    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDatasets datasets = gdb.getDatasets();
    EXPECT_EQ(datasets.getCount(), 3);

    EXPECT_STREQ(datasets.get(0).getName().c_str(), "layer_a");
    EXPECT_STREQ(datasets.get(1).getName().c_str(), "layer_b");
    EXPECT_STREQ(datasets.get(2).getName().c_str(), "layer_c");

    // 几何类型验证
    // OpenFileGDB 驱动会将线/面存储为 multi 类型
    EXPECT_EQ(datasets.get(0).getGeometryType(), wkbPoint);
    auto lineType = datasets.get(1).getGeometryType();
    EXPECT_TRUE(lineType == wkbLineString || lineType == wkbMultiLineString);
    auto polyType = datasets.get(2).getGeometryType();
    EXPECT_TRUE(polyType == wkbPolygon || polyType == wkbMultiPolygon);
}

/**
 * T_Datasets_GetByName: 按名称查找图层。
 */
TEST_F(GdbTutorialFixture, T_Datasets_GetByName) {
    const char* path = "/tmp/tutorial_datasets_byname.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    ds->CreateLayer("target_layer", nullptr, wkbPoint, nullptr);
    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDatasets datasets = gdb.getDatasets();

    // 查找存在的图层
    GdbDataset found = datasets.get("target_layer");
    EXPECT_TRUE(found.isValid());
    EXPECT_STREQ(found.getName().c_str(), "target_layer");

    // 查找不存在的图层
    GdbDataset notFound = datasets.get("nonexistent");
    EXPECT_FALSE(notFound.isValid());
}

/**
 * T_Datasets_FieldMetadata: 验证字段名称、数量、类型。
 */
TEST_F(GdbTutorialFixture, T_Datasets_FieldMetadata) {
    const char* path = "/tmp/tutorial_datasets_fields.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("meta", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn strField("name", OFTString);
    strField.SetWidth(50);
    layer->CreateField(&strField);

    OGRFieldDefn intField("count", OFTInteger);
    layer->CreateField(&intField);

    OGRFieldDefn realField("value", OFTReal);
    layer->CreateField(&realField);

    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset meta = gdb.getDatasets().get("meta");
    ASSERT_TRUE(meta.isValid());

    EXPECT_EQ(meta.getFieldCount(), 3);
    EXPECT_STREQ(meta.getFieldName(0).c_str(), "name");
    EXPECT_STREQ(meta.getFieldName(1).c_str(), "count");
    EXPECT_STREQ(meta.getFieldName(2).c_str(), "value");

    EXPECT_EQ(meta.getFieldType(0), OFTString);
    EXPECT_EQ(meta.getFieldType(1), OFTInteger);
    EXPECT_EQ(meta.getFieldType(2), OFTReal);

    // 类型名称映射
    EXPECT_STREQ(meta.getFieldTypeName(OFTString).c_str(), "String");
    EXPECT_STREQ(meta.getFieldTypeName(OFTInteger).c_str(), "Integer");
    EXPECT_STREQ(meta.getFieldTypeName(OFTReal).c_str(), "Real");
}

/**
 * T_Datasets_EmptyState: 空状态验证。
 */
TEST_F(GdbTutorialFixture, T_Datasets_EmptyState) {
    GdbDatasource gdb;
    GdbDatasets datasets = gdb.getDatasets();
    EXPECT_EQ(datasets.getCount(), 0);
    EXPECT_FALSE(datasets.get(0).isValid());
}
