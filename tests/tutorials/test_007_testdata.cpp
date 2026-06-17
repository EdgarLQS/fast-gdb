/**
 * ============================================================================
 * 教程 007: GDB 测试数据与验证
 * ============================================================================
 *
 * 7.1 GDAL 源码中的测试位置
 * ---------------------------
 *   autotest/ogr/ogr_openfilegdb.py      OpenFileGDB 矢量测试（30+ 用例）
 *   autotest/ogr/ogr_fgdb.py              FileGDB SDK 测试
 *   autotest/gdrivers/openfilegdb.py      OpenFileGDB 栅格测试
 *
 * 7.2 测试数据集
 * --------------
 *   GDAL autotest 提供多种测试 GDB 数据集：
 *   - testopenfilegdb.gdb.zip    基本矢量测试数据
 *   - curves.gdb                  曲线几何测试
 *   - relationships.gdb           关系测试数据
 *   - Domains.gdb                 字段域测试数据
 *
 * 7.3 测试覆盖范围
 * -----------------
 *   - 图层创建、字段定义、要素写入
 *   - 空间参考设置与验证
 *   - GDB_Items XML 元数据解析
 *   - 名称清洗（Laundering）
 *   - 事务支持
 *   - 栅格子数据集访问
 *
 * 7.4 运行 GDAL 官方测试
 * -----------------------
 *   cd gdal/autotest
 *   pytest ogr/ogr_openfilegdb.py -v
 *   pytest gdrivers/openfilegdb.py -v
 *
 * ============================================================================
 */

#include "test_fixture.h"

/**
 * 验证能创建包含多种几何类型的测试数据集。
 * 对应教程 7.3 节：测试覆盖范围
 */
TEST_F(GdbTutorialFixture, T007_MultiGeometryDataset) {
    const char* path = "/tmp/tutorial_007_multi.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    // 创建点图层
    OGRLayer* points = ds->CreateLayer("test_points", nullptr, wkbPoint, nullptr);
    ASSERT_NE(points, nullptr);
    ASSERT_EQ(points->CreateField(new OGRFieldDefn("id", OFTInteger)), OGRERR_NONE);

    // 创建线图层
    OGRLayer* lines = ds->CreateLayer("test_lines", nullptr, wkbLineString, nullptr);
    ASSERT_NE(lines, nullptr);
    ASSERT_EQ(lines->CreateField(new OGRFieldDefn("id", OFTInteger)), OGRERR_NONE);

    // 创建多边形图层
    OGRLayer* polygons = ds->CreateLayer("test_polygons", nullptr, wkbPolygon, nullptr);
    ASSERT_NE(polygons, nullptr);
    ASSERT_EQ(polygons->CreateField(new OGRFieldDefn("id", OFTInteger)), OGRERR_NONE);

    EXPECT_EQ(ds->GetLayerCount(), 3) << "应有 3 个测试图层";

    GDALClose(ds);
}

/**
 * 验证能创建包含多个字段的测试图层。
 * 对应教程 7.3 节：字段定义测试
 */
TEST_F(GdbTutorialFixture, T007_MultiFieldLayer) {
    const char* path = "/tmp/tutorial_007_fields.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("field_test", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    // 创建 GDAL 支持的多种字段类型
    const struct { const char* name; OGRFieldType type; int width; } fields[] = {
        {"str_field", OFTString, 50},
        {"int_field", OFTInteger, 0},
        {"real_field", OFTReal, 0},
        {"int64_field", OFTInteger64, 0},
    };

    for (const auto& f : fields) {
        OGRFieldDefn fd(f.name, f.type);
        if (f.width > 0) fd.SetWidth(f.width);
        ASSERT_EQ(layer->CreateField(&fd), OGRERR_NONE) << "字段 " << f.name << " 应创建成功";
    }

    // 写入一个测试要素
    OGRFeature feat(layer->GetLayerDefn());
    feat.SetField("str_field", "test_value");
    feat.SetField("int_field", 42);
    feat.SetField("real_field", 3.14159);
    feat.SetField("int64_field", (GIntBig)123456789012);
    OGRPoint pt(0, 0);
    feat.SetGeometry(&pt);
    ASSERT_EQ(layer->CreateFeature(&feat), OGRERR_NONE);

    GDALClose(ds);

    // 验证读取
    ds = (GDALDataset*)GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);

    layer = ds->GetLayerByName("field_test");
    ASSERT_NE(layer, nullptr);

    layer->ResetReading();
    OGRFeature* readFeat = layer->GetNextFeature();
    ASSERT_NE(readFeat, nullptr);

    EXPECT_STREQ(readFeat->GetFieldAsString("str_field"), "test_value");
    EXPECT_EQ(readFeat->GetFieldAsInteger("int_field"), 42);
    EXPECT_NEAR(readFeat->GetFieldAsDouble("real_field"), 3.14159, 0.001);

    OGRFeature::DestroyFeature(readFeat);
    GDALClose(ds);
}

/**
 * 验证 GDB_Items 系统表存在且可查询。
 * 对应教程 7.3 节：GDB_Items XML 元数据解析
 */
TEST_F(GdbTutorialFixture, T007_GdbItemsTableExists) {
    const char* path = "/tmp/tutorial_007_items.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    // 创建至少一个用户图层
    ds->CreateLayer("user_layer", nullptr, wkbPoint, nullptr);

    GDALClose(ds);

    // 重新打开并检查 GDB_Items
    ds = (GDALDataset*)GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);

    OGRLayer* items = ds->GetLayerByName("GDB_Items");
    ASSERT_NE(items, nullptr) << "GDB_Items 系统表必须存在";

    // GDB_Items 应包含至少一条记录（用户图层的元数据）
    int itemCount = countFeatures(items);
    EXPECT_GT(itemCount, 0) << "GDB_Items 应包含图层元数据";

    GDALClose(ds);
}
