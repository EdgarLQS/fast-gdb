/**
 * feature_test.cpp — GdbFeature 要素抽象测试
 *
 * 覆盖：FID/Geometry/Fields 设置与读取、get/setField、toJson、fromNative
 */

#include "test_fixture.h"
#include "datasource.h"
#include "datasets.h"
#include "recordset.h"
#include "field.h"
#include "feature.h"

// ===== FID 测试 =====

/**
 * T_Feature_DefaultFid: 默认 FID 为 OGRNullFID。
 */
TEST_F(GdbTutorialFixture, T_Feature_DefaultFid) {
    GdbFeature f;
    EXPECT_EQ(f.getFid(), OGRNullFID);
}

/**
 * T_Feature_SetFid: 设置 FID。
 */
TEST_F(GdbTutorialFixture, T_Feature_SetFid) {
    GdbFeature f(42);
    EXPECT_EQ(f.getFid(), 42);
}

// ===== 几何测试 =====

/**
 * T_Feature_Geometry: 设置和读取几何。
 */
TEST_F(GdbTutorialFixture, T_Feature_Geometry) {
    GdbFeature f;
    OGRPoint pt(10.0, 20.0);
    f.setGeometry(std::unique_ptr<OGRGeometry>(pt.clone()));

    const OGRGeometry* geom = f.getGeometry();
    ASSERT_NE(geom, nullptr);
    auto* readPt = dynamic_cast<const OGRPoint*>(geom);
    ASSERT_NE(readPt, nullptr);
    EXPECT_DOUBLE_EQ(readPt->getX(), 10.0);
    EXPECT_DOUBLE_EQ(readPt->getY(), 20.0);
}

/**
 * T_Feature_NoGeometry: 无几何的要素。
 */
TEST_F(GdbTutorialFixture, T_Feature_NoGeometry) {
    GdbFeature f;
    EXPECT_EQ(f.getGeometry(), nullptr);
}

// ===== 字段测试 =====

/**
 * T_Feature_SetGetFields: 按名称和索引设置/读取字段。
 */
TEST_F(GdbTutorialFixture, T_Feature_SetGetFields) {
    GdbFeature f;
    f.setField("name", GdbField(std::string("Alice")));
    f.setField("age", GdbField(30));
    f.setField("score", GdbField(95.5));

    EXPECT_EQ(f.getField("name").asString(), "Alice");
    EXPECT_EQ(f.getField("age").asInteger(), 30);
    EXPECT_NEAR(f.getField("score").asDouble(), 95.5, 0.01);
}

/**
 * T_Feature_FieldCount: 字段计数和名称。
 */
TEST_F(GdbTutorialFixture, T_Feature_FieldCount) {
    GdbFeature f;
    EXPECT_EQ(f.getFieldCount(), 0);

    f.setField("a", GdbField(1));
    f.setField("b", GdbField(2.0));
    EXPECT_EQ(f.getFieldCount(), 2);
    EXPECT_EQ(f.getFieldName(0), "a");
    EXPECT_EQ(f.getFieldName(1), "b");
}

// ===== toJson =====

/**
 * T_Feature_ToJson: toJson 输出包含 FID、几何、字段。
 */
TEST_F(GdbTutorialFixture, T_Feature_ToJson) {
    GdbFeature f(1);
    f.setField("name", GdbField(std::string("test")));
    f.setField("value", GdbField(42));

    std::string json = f.toJson();
    EXPECT_NE(json.find("name"), std::string::npos);
    EXPECT_NE(json.find("test"), std::string::npos);
    EXPECT_NE(json.find("value"), std::string::npos);
    EXPECT_NE(json.find("42"), std::string::npos);
}

// ===== fromNative =====

/**
 * T_Feature_FromNative: 从 OGRFeature 构造 GdbFeature，字段值一致。
 */
TEST_F(GdbTutorialFixture, T_Feature_FromNative) {
    const char* path = "/tmp/tutorial_feature_native.gdb";
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    GDALDeleteDataset(drv, path);
    GDALDataset* ds = (GDALDataset*)drv->Create(path, 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("people", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    layer->CreateField(new OGRFieldDefn("name", OFTString));
    layer->CreateField(new OGRFieldDefn("age", OFTInteger));

    OGRFeature feat(layer->GetLayerDefn());
    feat.SetField("name", "Bob");
    feat.SetField("age", 25);
    OGRPoint pt(5.0, 10.0);
    feat.SetGeometry(&pt);
    layer->CreateFeature(&feat);

    // 读取并转为 GdbFeature
    layer->ResetReading();
    OGRFeature* readFeat = layer->GetNextFeature();
    ASSERT_NE(readFeat, nullptr);

    GdbFeature gFeat = GdbFeature::fromNative(readFeat);
    EXPECT_EQ(gFeat.getField("name").asString(), "Bob");
    EXPECT_EQ(gFeat.getField("age").asInteger(), 25);

    const OGRGeometry* geom = gFeat.getGeometry();
    ASSERT_NE(geom, nullptr);
    auto* gPt = dynamic_cast<const OGRPoint*>(geom);
    ASSERT_NE(gPt, nullptr);
    EXPECT_DOUBLE_EQ(gPt->getX(), 5.0);
    EXPECT_DOUBLE_EQ(gPt->getY(), 10.0);

    OGRFeature::DestroyFeature(readFeat);
    GDALClose(ds);
}

// ===== Recordset integration =====

/**
 * T_Feature_RecordsetRead: 通过 Recordset 读取要素为 GdbFeature。
 */
TEST_F(GdbTutorialFixture, T_Feature_RecordsetRead) {
    const char* path = "/tmp/tutorial_feature_rs.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("cities", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    layer->CreateField(new OGRFieldDefn("name", OFTString));
    layer->CreateField(new OGRFieldDefn("pop", OFTInteger));

    OGRFeature feat(layer->GetLayerDefn());
    feat.SetField("name", "Tokyo");
    feat.SetField("pop", 37400000);
    OGRPoint pt(139.7, 35.7);
    feat.SetGeometry(&pt);
    layer->CreateFeature(&feat);

    // 通过组件 API 读取
    GdbDatasource gdb(ds);
    GdbDataset cities = gdb.getDatasets().get("cities");
    ASSERT_TRUE(cities.isValid());

    // 使用新的 getFeature() 方法
    GdbRecordset rs = cities.getRecordset();
    ASSERT_TRUE(rs.moveNext());
    GdbFeature gFeat = rs.getFeature();
    EXPECT_EQ(gFeat.getField("name").asString(), "Tokyo");
    EXPECT_EQ(gFeat.getField("pop").asInteger(), 37400000);
}
