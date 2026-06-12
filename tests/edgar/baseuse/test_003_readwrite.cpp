/**
 * ============================================================================
 * 教程 003: GDB 读取与写入实战指南
 * ============================================================================
 *
 * 3.1 CLI 工具
 * ------------
 *   # 列出 GDB 中的所有图层
 *   ogrinfo mydata.gdb
 *
 *   # 查看特定图层结构信息
 *   ogrinfo mydata.gdb roads -so
 *
 *   # 导出为 GeoJSON / GeoPackage
 *   ogr2ogr -f GeoJSON roads.json mydata.gdb roads
 *   ogr2ogr -f GPKG output.gpkg mydata.gdb
 *
 *   # 空间过滤 / 属性过滤
 *   ogr2ogr -f GeoJSON filtered.json mydata.gdb roads -spat 100 200 300 400
 *   ogr2ogr -f GeoJSON big_roads.json mydata.gdb roads -where "road_type = 'highway'"
 *
 *   # 读取 zipped .gdb（OpenFileGDB 特有能力）
 *   ogrinfo data.gdb.zip
 *
 * 3.2 C++ API — 创建 GDB
 * -----------------------
 *   OGRSFDriver* poDriver = OGRSFDriverRegistrar::GetRegistrar()
 *       ->GetDriverByName("OpenFileGDB");
 *   GDALDataset* poDS = poDriver->Create(pszGdbPath, 0, 0, 0, GDT_Unknown, nullptr);
 *   OGRLayer* poLayer = poDS->CreateLayer("points", nullptr, wkbPoint, papszOptions);
 *   poLayer->CreateField(&oNameField);
 *   poLayer->CreateFeature(poFeat);
 *
 * 3.3 写入选项
 * ------------
 *   STRING_WIDTH           65536   字符串字段默认宽度
 *   GEOMETRY_NULLABLE      YES     几何字段是否可空
 *   CREATE_CSV             NO      同时生成 .csv 辅助文件（调试用）
 *
 * 3.4 写入限制
 * ------------
 *   栅格写入        OpenFileGDB 和 FileGDB 均不支持
 *   CDF 压缩        写入的表不会使用 CDF 压缩
 *   字段域写入      需 GDAL 3.5+
 *   名称清洗        字段名和图层名会自动 Launder
 *
 * 3.5 事务支持
 * ------------
 *   OpenFileGDB 的事务通过文件系统级备份模拟：
 *   BackupSystemTablesForTransaction() 备份系统表，回滚时恢复。
 *   大量修改时有显著 I/O 开销。
 *
 * ============================================================================
 */

#include "test_fixture.h"

/**
 * 验证创建图层并写入要素后能正确读取。
 * 对应教程 3.2 节：C++ API
 */
TEST_F(GdbTutorialFixture, T003_CreateLayerWriteRead) {
    const char* path = "/tmp/tutorial_003_basic.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    // 创建点图层
    OGRLayer* layer = ds->CreateLayer("points", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    // 添加字段（对应教程 3.2 创建字段示例）
    OGRFieldDefn strField("name", OFTString);
    strField.SetWidth(50);
    ASSERT_EQ(layer->CreateField(&strField), OGRERR_NONE);

    OGRFieldDefn intField("value", OFTInteger);
    ASSERT_EQ(layer->CreateField(&intField), OGRERR_NONE);

    // 写入 5 个要素
    for (int i = 0; i < 5; i++) {
        OGRFeature feat(layer->GetLayerDefn());
        feat.SetField("name", ("point_" + std::to_string(i)).c_str());
        feat.SetField("value", i * 10);
        OGRPoint pt((double)i, (double)i * 2);
        feat.SetGeometry(&pt);
        ASSERT_EQ(layer->CreateFeature(&feat), OGRERR_NONE);
    }

    GDALClose(ds);

    // 重新打开并验证
    ds = (GDALDataset*)GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);

    layer = ds->GetLayerByName("points");
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(countFeatures(layer), 5) << "应包含 5 个要素";

    // 验证字段值
    layer->ResetReading();
    OGRFeature* feat = layer->GetNextFeature();
    ASSERT_NE(feat, nullptr);
    EXPECT_STREQ(feat->GetFieldAsString("name"), "point_0");
    EXPECT_EQ(feat->GetFieldAsInteger("value"), 0);
    OGRFeature::DestroyFeature(feat);

    GDALClose(ds);
}

/**
 * 验证创建多边形图层。
 * 对应教程 3.2 节：复杂几何类型
 */
TEST_F(GdbTutorialFixture, T003_CreatePolygonLayer) {
    const char* path = "/tmp/tutorial_003_polygon.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    // 创建多边形图层
    OGRLayer* layer = ds->CreateLayer("polygons", nullptr, wkbPolygon, nullptr);
    ASSERT_NE(layer, nullptr);

    // 添加面积字段
    OGRFieldDefn areaField("area", OFTReal);
    ASSERT_EQ(layer->CreateField(&areaField), OGRERR_NONE);

    // 写入一个矩形多边形
    OGRLinearRing ring;
    ring.addPoint(0, 0);
    ring.addPoint(10, 0);
    ring.addPoint(10, 10);
    ring.addPoint(0, 10);
    ring.closeRings();

    OGRPolygon poly;
    poly.addRing(&ring);

    OGRFeature feat(layer->GetLayerDefn());
    feat.SetField("area", 100.0);
    feat.SetGeometry(&poly);
    ASSERT_EQ(layer->CreateFeature(&feat), OGRERR_NONE);

    GDALClose(ds);

    // 验证
    ds = (GDALDataset*)GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);

    layer = ds->GetLayerByName("polygons");
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(countFeatures(layer), 1);

    layer->ResetReading();
    OGRFeature* readFeat = layer->GetNextFeature();
    ASSERT_NE(readFeat, nullptr);
    ASSERT_NE(readFeat->GetGeometryRef(), nullptr);
    OGRwkbGeometryType geomType = readFeat->GetGeometryRef()->getGeometryType();
    EXPECT_TRUE(geomType >= wkbPolygon && geomType <= wkbPolygonZM) << "几何类型应为多边形变体";
    OGRFeature::DestroyFeature(readFeat);

    GDALClose(ds);
}

/**
 * 验证图层次数（创建多个图层后检查数量）。
 */
TEST_F(GdbTutorialFixture, T003_MultipleLayers) {
    const char* path = "/tmp/tutorial_003_multi.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    // 创建 3 个不同类型图层
    ds->CreateLayer("points", nullptr, wkbPoint, nullptr);
    ds->CreateLayer("lines", nullptr, wkbLineString, nullptr);
    ds->CreateLayer("polygons", nullptr, wkbPolygon, nullptr);

    int layerCount = ds->GetLayerCount();
    EXPECT_EQ(layerCount, 3) << "应有 3 个图层";

    GDALClose(ds);
}
