/**
 * recordset_test.cpp — GdbRecordset 单元测试
 *
 * 覆盖：顺序游标、字段类型化读取、几何读取/克隆
 */

#include "test_fixture.h"
#include "datasource.h"
#include "datasets.h"
#include "recordset.h"

/**
 * T_Recordset_SequentialCursor: 验证 moveFirst/moveNext/isEOF 游标行为。
 */
TEST_F(GdbTutorialFixture, T_Recordset_SequentialCursor) {
    const char* path = "/tmp/tutorial_recordset_cursor.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("cursor_test", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    // 写入 3 个要素
    for (int i = 0; i < 3; i++) {
        OGRFeature feat(layer->GetLayerDefn());
        OGRPoint pt(i * 10.0, i * 20.0);
        feat.SetGeometry(&pt);
        layer->CreateFeature(&feat);
    }
    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset ds_cursor = gdb.getDatasets().get("cursor_test");
    ASSERT_TRUE(ds_cursor.isValid());

    GdbRecordset rs = ds_cursor.getRecordset();

    // moveFirst 到第一条
    EXPECT_TRUE(rs.moveFirst());
    EXPECT_FALSE(rs.isEOF());

    // moveNext 第二条
    EXPECT_TRUE(rs.moveNext());
    EXPECT_FALSE(rs.isEOF());

    // moveNext 第三条
    EXPECT_TRUE(rs.moveNext());
    EXPECT_FALSE(rs.isEOF());

    // moveNext 结束
    EXPECT_FALSE(rs.moveNext());
    EXPECT_TRUE(rs.isEOF());
}

/**
 * T_Recordset_ReadFields: 验证类型化字段读取。
 */
TEST_F(GdbTutorialFixture, T_Recordset_ReadFields) {
    const char* path = "/tmp/tutorial_recordset_fields.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("records", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn nameField("name", OFTString);
    nameField.SetWidth(20);
    layer->CreateField(&nameField);

    OGRFieldDefn ageField("age", OFTInteger);
    layer->CreateField(&ageField);

    OGRFieldDefn scoreField("score", OFTReal);
    layer->CreateField(&scoreField);

    // 写入 2 条记录
    OGRFeature feat1(layer->GetLayerDefn());
    feat1.SetField("name", "Alice");
    feat1.SetField("age", 30);
    feat1.SetField("score", 95.5);
    OGRPoint pt1(1.0, 2.0);
    feat1.SetGeometry(&pt1);
    layer->CreateFeature(&feat1);

    OGRFeature feat2(layer->GetLayerDefn());
    feat2.SetField("name", "Bob");
    feat2.SetField("age", 25);
    feat2.SetField("score", 88.0);
    OGRPoint pt2(3.0, 4.0);
    feat2.SetGeometry(&pt2);
    layer->CreateFeature(&feat2);

    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset records = gdb.getDatasets().get("records");
    ASSERT_TRUE(records.isValid());

    // 字段元信息通过 Recordset 获取
    GdbRecordset rs = records.getRecordset();
    EXPECT_EQ(rs.getFieldCount(), 3);
    EXPECT_EQ(rs.getFieldIndex("name"), 0);
    EXPECT_EQ(rs.getFieldIndex("age"), 1);
    EXPECT_EQ(rs.getFieldIndex("score"), 2);
    EXPECT_EQ(rs.getFieldIndex("nonexistent"), -1);

    // 第一条记录
    ASSERT_TRUE(rs.moveFirst());
    EXPECT_STREQ(rs.getFieldAsString("name").c_str(), "Alice");
    EXPECT_EQ(rs.getFieldAsInteger("age"), 30);
    EXPECT_NEAR(rs.getFieldAsDouble("score"), 95.5, 0.001);

    // 第二条记录
    ASSERT_TRUE(rs.moveNext());
    EXPECT_STREQ(rs.getFieldAsString("name").c_str(), "Bob");
    EXPECT_EQ(rs.getFieldAsInteger("age"), 25);
    EXPECT_NEAR(rs.getFieldAsDouble("score"), 88.0, 0.001);

    // 按索引读取
    ASSERT_TRUE(rs.moveFirst());
    EXPECT_EQ(rs.getFieldAsInteger(1), 30);
    EXPECT_STREQ(rs.getFieldAsString(0).c_str(), "Alice");
}

/**
 * T_Recordset_ReadGeometry: 验证几何读取和克隆。
 */
TEST_F(GdbTutorialFixture, T_Recordset_ReadGeometry) {
    const char* path = "/tmp/tutorial_recordset_geom.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("geom_test", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(42.5, -12.3);
    feat.SetGeometry(&pt);
    layer->CreateFeature(&feat);

    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset geom_ds = gdb.getDatasets().get("geom_test");
    ASSERT_TRUE(geom_ds.isValid());
    EXPECT_EQ(geom_ds.getGeometryType(), wkbPoint);

    GdbRecordset rs = geom_ds.getRecordset();
    ASSERT_TRUE(rs.moveFirst());

    // const 指针读取
    const OGRGeometry* geom = rs.getGeometry();
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbPoint);

    // 克隆持有
    auto cloned = rs.cloneGeometry();
    ASSERT_NE(cloned, nullptr);
    auto* clonedPt = dynamic_cast<OGRPoint*>(cloned.get());
    ASSERT_NE(clonedPt, nullptr);
    EXPECT_NEAR(clonedPt->getX(), 42.5, 0.001);
    EXPECT_NEAR(clonedPt->getY(), -12.3, 0.001);
}

/**
 * T_Recordset_Fid: 验证 FID 读取。
 */
TEST_F(GdbTutorialFixture, T_Recordset_Fid) {
    const char* path = "/tmp/tutorial_recordset_fid.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("fid_test", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(0.0, 0.0);
    feat.SetGeometry(&pt);
    layer->CreateFeature(&feat);

    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset fid_ds = gdb.getDatasets().get("fid_test");
    GdbRecordset rs = fid_ds.getRecordset();

    // 未移动时 FID 为 -1
    EXPECT_EQ(rs.getFid(), -1);

    ASSERT_TRUE(rs.moveFirst());
    // GDAL 自动分配的 FID >= 0
    EXPECT_GE(rs.getFid(), 0);
}

/**
 * T_Recordset_EmptyState: 空状态验证。
 */
TEST_F(GdbTutorialFixture, T_Recordset_EmptyState) {
    GdbRecordset rs;
    EXPECT_FALSE(rs.isValid());
    EXPECT_FALSE(rs.moveFirst());
    EXPECT_FALSE(rs.moveNext());
    EXPECT_TRUE(rs.isEOF());
    EXPECT_EQ(rs.getFid(), -1);
    EXPECT_EQ(rs.getFieldCount(), 0);
    EXPECT_EQ(rs.getGeometry(), nullptr);
}
