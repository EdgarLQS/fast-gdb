/**
 * recordset_write_test.cpp — GdbRecordset 写入操作测试
 */

#include "test_fixture.h"
#include "datasource.h"
#include "datasets.h"
#include "recordset.h"

// 辅助：在已有的 GDB 上创建带预置数据的 points 图层
static void populatePointsLayer(GDALDataset* ds) {
    OGRLayer* layer = ds->CreateLayer("points", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn nameField("name", OFTString);
    nameField.SetWidth(32);
    layer->CreateField(&nameField);

    OGRFieldDefn xField("x", OFTReal);
    layer->CreateField(&xField);

    OGRFieldDefn yField("y", OFTReal);
    layer->CreateField(&yField);

    struct Pt { const char* name; double x, y; };
    Pt pts[] = {
        {"Alpha", 1.0, 2.0},
        {"Beta", 3.0, 4.0},
        {"Gamma", 5.0, 6.0},
    };
    for (const auto& p : pts) {
        OGRFeature feat(layer->GetLayerDefn());
        feat.SetField("name", p.name);
        feat.SetField("x", p.x);
        feat.SetField("y", p.y);
        OGRPoint pt(p.x, p.y);
        feat.SetGeometry(&pt);
        layer->CreateFeature(&feat);
    }
}

// ===== 添加要素测试 =====

/**
 * T_Write_AddFeature: addNew 创建新要素后遍历验证。
 */
TEST_F(GdbTutorialFixture, T_Write_AddFeature) {
    GDALDataset* ds = createGdb("/tmp/tutorial_write_add.gdb");
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("new_points", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn nameField("name", OFTString);
    nameField.SetWidth(32);
    layer->CreateField(&nameField);

    OGRFieldDefn valField("value", OFTInteger);
    layer->CreateField(&valField);

    GdbDatasource gdb(ds);
    GdbDataset dataset = gdb.getDatasets().get("new_points");
    ASSERT_TRUE(dataset.isValid());

    OGRPoint p1(10.0, 20.0);
    EXPECT_TRUE(dataset.addNew(&p1, {{"name", "First"}, {"value", "100"}}));

    OGRPoint p2(30.0, 40.0);
    EXPECT_TRUE(dataset.addNew(&p2, {{"name", "Second"}, {"value", "200"}}));

    OGRPoint p3(50.0, 60.0);
    EXPECT_TRUE(dataset.addNew(&p3, {{"name", "Third"}, {"value", "300"}}));

    // 验证计数（同一个 ds 上的 getFeatureCount）
    EXPECT_EQ(dataset.getFeatureCount(), 3);

    // 遍历验证
    GdbRecordset rs = dataset.getRecordset();
    std::vector<std::string> names;
    while (rs.moveNext()) {
        names.push_back(rs.getFieldAsString("name"));
    }
    EXPECT_EQ(names.size(), 3);
    EXPECT_EQ(names[0], "First");
    EXPECT_EQ(names[1], "Second");
    EXPECT_EQ(names[2], "Third");

    // 不手动 GDALClose，让 TearDown 自动清理
}

// ===== 编辑更新测试 =====

/**
 * T_Write_EditUpdate: edit → setField → update 修改字段值。
 */
TEST_F(GdbTutorialFixture, T_Write_EditUpdate) {
    GDALDataset* ds = createGdb("/tmp/tutorial_write_edit.gdb");
    ASSERT_NE(ds, nullptr);
    populatePointsLayer(ds);

    GdbDatasource gdb(ds);
    GdbDataset points = gdb.getDatasets().get("points");
    ASSERT_TRUE(points.isValid());
    EXPECT_EQ(points.getFeatureCount(), 3);

    GdbRecordset rs = points.getRecordset();
    bool found = false;
    bool updateOk = false;
    while (rs.moveNext()) {
        if (rs.getFieldAsString("name") == "Beta") {
            found = true;
            EXPECT_TRUE(rs.edit());
            EXPECT_TRUE(rs.setField("name", "Beta_Updated"));
            EXPECT_TRUE(rs.setField("x", 99.0));
            updateOk = rs.update();
            EXPECT_TRUE(updateOk);
            break;
        }
    }
    EXPECT_TRUE(found);

    // 验证修改结果
    GdbRecordset rs2 = points.getRecordset();
    bool foundUpdated = false;
    while (rs2.moveNext()) {
        if (rs2.getFieldAsString("name") == "Beta_Updated") {
            foundUpdated = true;
            EXPECT_DOUBLE_EQ(rs2.getFieldAsDouble("x"), 99.0);
            break;
        }
    }
    EXPECT_TRUE(foundUpdated);

    // 计数不变
    EXPECT_EQ(points.getFeatureCount(), 3);
}

/**
 * T_Write_SetGeometry: edit → setGeometry → update 修改几何。
 */
TEST_F(GdbTutorialFixture, T_Write_SetGeometry) {
    GDALDataset* ds = createGdb("/tmp/tutorial_write_geom.gdb");
    ASSERT_NE(ds, nullptr);
    populatePointsLayer(ds);

    GdbDatasource gdb(ds);
    GdbDataset points = gdb.getDatasets().get("points");

    GdbRecordset rs = points.getRecordset();
    while (rs.moveNext()) {
        if (rs.getFieldAsString("name") == "Alpha") {
            EXPECT_TRUE(rs.edit());
            OGRPoint newGeom(100.0, 200.0);
            EXPECT_TRUE(rs.setGeometry(&newGeom));
            EXPECT_TRUE(rs.update());
            break;
        }
    }

    GdbRecordset rs2 = points.getRecordset();
    while (rs2.moveNext()) {
        if (rs2.getFieldAsString("name") == "Alpha") {
            auto* geom = rs2.getGeometry();
            ASSERT_NE(geom, nullptr);
            auto* pt = dynamic_cast<const OGRPoint*>(geom);
            ASSERT_NE(pt, nullptr);
            EXPECT_DOUBLE_EQ(pt->getX(), 100.0);
            EXPECT_DOUBLE_EQ(pt->getY(), 200.0);
            break;
        }
    }
}

// ===== 删除测试 =====

/**
 * T_Write_DeleteCurrent: deleteCurrent 删除当前要素。
 */
TEST_F(GdbTutorialFixture, T_Write_DeleteCurrent) {
    GDALDataset* ds = createGdb("/tmp/tutorial_write_delete.gdb");
    ASSERT_NE(ds, nullptr);
    populatePointsLayer(ds);

    GdbDatasource gdb(ds);
    GdbDataset points = gdb.getDatasets().get("points");
    EXPECT_EQ(points.getFeatureCount(), 3);

    GdbRecordset rs = points.getRecordset();
    while (rs.moveNext()) {
        if (rs.getFieldAsString("name") == "Beta") {
            EXPECT_TRUE(rs.deleteCurrent());
            break;
        }
    }

    EXPECT_EQ(points.getFeatureCount(), 2);

    GdbRecordset rs2 = points.getRecordset();
    int count = 0;
    while (rs2.moveNext()) count++;
    EXPECT_EQ(count, 2);
}

/**
 * T_Write_DeleteAll: deleteAll 清空图层所有要素。
 */
TEST_F(GdbTutorialFixture, T_Write_DeleteAll) {
    GDALDataset* ds = createGdb("/tmp/tutorial_write_delete_all.gdb");
    ASSERT_NE(ds, nullptr);
    populatePointsLayer(ds);

    GdbDatasource gdb(ds);
    GdbDataset points = gdb.getDatasets().get("points");
    EXPECT_EQ(points.getFeatureCount(), 3);

    EXPECT_TRUE(points.deleteAll());
    EXPECT_EQ(points.getFeatureCount(), 0);

    GdbRecordset rs = points.getRecordset();
    EXPECT_FALSE(rs.moveNext());
    EXPECT_TRUE(rs.isEOF());
}

// ===== 字段类型覆盖测试 =====

/**
 * T_Write_FieldTypes: addNew 覆盖所有字段类型。
 */
TEST_F(GdbTutorialFixture, T_Write_FieldTypes) {
    GDALDataset* ds = createGdb("/tmp/tutorial_write_fieldtypes.gdb");
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("typed", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    layer->CreateField(new OGRFieldDefn("name", OFTString));
    layer->CreateField(new OGRFieldDefn("count", OFTInteger));
    // OpenFileGDB 不原生支持 Integer64，会用 Float64 存储
    // 这里改用 Real 类型验证
    layer->CreateField(new OGRFieldDefn("big_id", OFTReal));
    layer->CreateField(new OGRFieldDefn("score", OFTReal));

    GdbDatasource gdb(ds);
    GdbDataset dataset = gdb.getDatasets().get("typed");

    OGRPoint pt(1.0, 1.0);
    EXPECT_TRUE(dataset.addNew(&pt, {
        {"name", "TestRow"},
        {"count", "42"},
        {"big_id", "9223372036854775807"},
        {"score", "3.14159"},
    }));

    // 直接遍历验证
    GdbRecordset rs = dataset.getRecordset();
    ASSERT_TRUE(rs.moveNext());
    EXPECT_STREQ(rs.getFieldAsString("name").c_str(), "TestRow");
    EXPECT_EQ(rs.getFieldAsInteger("count"), 42);
    // 作为 Real 读取（OpenFileGDB 内部存储为 Float64）
    EXPECT_NEAR(rs.getFieldAsDouble("big_id"), 9.223372036854776e18, 1e6);
    EXPECT_NEAR(rs.getFieldAsDouble("score"), 3.14159, 0.0001);
}
