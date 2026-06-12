/**
 * ============================================================================
 * 教程 010: GDB 最小实验
 * ============================================================================
 *
 * 本教程包含 5 个最小实验，验证 GDB 的核心功能。
 *
 * 实验 1: 创建 GDB 并列出所有图层
 * ---------------------------------
 *   GDALDataset* ds = GDALOpenEx(path, GDAL_OF_VECTOR);
 *   for (int i = 0; i < ds->GetLayerCount(); i++) {
 *       OGRLayer* layer = ds->GetLayer(i);
 *       printf("Layer %d: %s\n", i, layer->GetName());
 *   }
 *
 * 实验 2: 创建图层并添加字段
 * ---------------------------
 *   OGRLayer* layer = ds->CreateLayer("points", nullptr, wkbPoint, nullptr);
 *   OGRFieldDefn nameField("name", OFTString);
 *   nameField.SetWidth(50);
 *   layer->CreateField(&nameField);
 *
 * 实验 3: 写入要素并验证
 * -----------------------
 *   OGRFeature feat(layer->GetLayerDefn());
 *   feat.SetField("name", "Point A");
 *   OGRPoint pt(1.0, 2.0);
 *   feat.SetGeometry(&pt);
 *   layer->CreateFeature(&feat);
 *
 * 实验 4: 创建不同 CRS 的图层
 * ----------------------------
 *   OGRSpatialReference srs4326; srs4326.importFromEPSG(4326);
 *   OGRSpatialReference srs3857; srs3857.importFromEPSG(3857);
 *   ds->CreateLayer("geo_layer", &srs4326, wkbPoint, nullptr);
 *   ds->CreateLayer("proj_layer", &srs3857, wkbPoint, nullptr);
 *
 * 实验 5: 创建 GDB 后重新打开验证持久化
 * --------------------------------------
 *   创建 → 写入 → 关闭 → 重新打开 → 验证数据完整
 *
 * ============================================================================
 */

#include "test_fixture.h"

/**
 * 实验 1: 创建 GDB 并列出所有图层。
 */
TEST_F(GdbTutorialFixture, T010_Experiment1_ListLayers) {
    const char* path = "/tmp/tutorial_010_list.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    // 创建几个图层
    ds->CreateLayer("layer_a", nullptr, wkbPoint, nullptr);
    ds->CreateLayer("layer_b", nullptr, wkbLineString, nullptr);
    ds->CreateLayer("layer_c", nullptr, wkbPolygon, nullptr);

    GDALClose(ds);

    // 重新打开并列举所有图层
    ds = (GDALDataset*)GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);

    int nLayers = ds->GetLayerCount();
    EXPECT_EQ(nLayers, 3) << "应有 3 个用户图层";

    for (int i = 0; i < nLayers; i++) {
        OGRLayer* layer = ds->GetLayer(i);
        ASSERT_NE(layer, nullptr);
        const char* name = layer->GetName();
        ASSERT_NE(name, nullptr);
    }

    GDALClose(ds);
}

/**
 * 实验 2: 创建图层并添加字段。
 */
TEST_F(GdbTutorialFixture, T010_Experiment2_CreateLayerWithFields) {
    const char* path = "/tmp/tutorial_010_fields.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("experiment2", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    // 添加多个字段
    OGRFieldDefn nameField("name", OFTString);
    nameField.SetWidth(50);
    ASSERT_EQ(layer->CreateField(&nameField), OGRERR_NONE);

    OGRFieldDefn ageField("age", OFTInteger);
    ASSERT_EQ(layer->CreateField(&ageField), OGRERR_NONE);

    OGRFieldDefn heightField("height", OFTReal);
    ASSERT_EQ(layer->CreateField(&heightField), OGRERR_NONE);

    // 验证字段数量
    OGRFeatureDefn* defn = layer->GetLayerDefn();
    EXPECT_EQ(defn->GetFieldCount(), 3) << "应有 3 个字段";

    GDALClose(ds);
}

/**
 * 实验 3: 写入要素并验证。
 */
TEST_F(GdbTutorialFixture, T010_Experiment3_WriteFeatures) {
    const char* path = "/tmp/tutorial_010_write.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("points", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn nameField("name", OFTString);
    nameField.SetWidth(20);
    layer->CreateField(&nameField);

    // 写入 10 个点
    for (int i = 0; i < 10; i++) {
        OGRFeature feat(layer->GetLayerDefn());
        feat.SetField("name", ("point_" + std::to_string(i)).c_str());
        OGRPoint pt((double)i * 1.5, (double)i * 2.5);
        feat.SetGeometry(&pt);
        ASSERT_EQ(layer->CreateFeature(&feat), OGRERR_NONE);
    }

    GDALClose(ds);

    // 验证
    ds = (GDALDataset*)GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);

    layer = ds->GetLayerByName("points");
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(countFeatures(layer), 10) << "应包含 10 个要素";

    GDALClose(ds);
}

/**
 * 实验 4: 创建不同 CRS 的图层。
 */
TEST_F(GdbTutorialFixture, T010_Experiment4_DifferentCrs) {
    const char* path = "/tmp/tutorial_010_crs.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    // EPSG:4326（地理坐标，度）
    OGRSpatialReference srs4326;
    srs4326.importFromEPSG(4326);
    OGRLayer* layer4326 = ds->CreateLayer("geo_layer", &srs4326, wkbPoint, nullptr);
    ASSERT_NE(layer4326, nullptr);

    // EPSG:3857（投影坐标，米）
    OGRSpatialReference srs3857;
    srs3857.importFromEPSG(3857);
    OGRLayer* layer3857 = ds->CreateLayer("proj_layer", &srs3857, wkbPoint, nullptr);
    ASSERT_NE(layer3857, nullptr);

    GDALClose(ds);

    // 验证 CRS
    ds = (GDALDataset*)GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);

    layer4326 = ds->GetLayerByName("geo_layer");
    ASSERT_NE(layer4326, nullptr);
    {
        OGRSpatialReference* srs = layer4326->GetSpatialRef();
        ASSERT_NE(srs, nullptr);
        EXPECT_EQ(getEpsgCode(srs), 4326) << "geo_layer 的 CRS 应为 EPSG:4326";
    }

    layer3857 = ds->GetLayerByName("proj_layer");
    ASSERT_NE(layer3857, nullptr);
    {
        OGRSpatialReference* srs = layer3857->GetSpatialRef();
        ASSERT_NE(srs, nullptr);
        EXPECT_EQ(getEpsgCode(srs), 3857) << "proj_layer 的 CRS 应为 EPSG:3857";
    }

    GDALClose(ds);
}

/**
 * 实验 5: 创建 GDB 后重新打开验证持久化。
 */
TEST_F(GdbTutorialFixture, T010_Experiment5_ReopenVerify) {
    const char* path = "/tmp/tutorial_010_reopen.gdb";

    // 创建并写入
    {
        GDALDataset* ds = createGdb(path);
        ASSERT_NE(ds, nullptr);

        OGRLayer* layer = ds->CreateLayer("persist_test", nullptr, wkbPoint, nullptr);
        ASSERT_NE(layer, nullptr);

        OGRFieldDefn idField("id", OFTInteger);
        layer->CreateField(&idField);

        OGRFeature feat(layer->GetLayerDefn());
        feat.SetField("id", 999);
        OGRPoint pt(42.0, 42.0);
        feat.SetGeometry(&pt);
        ASSERT_EQ(layer->CreateFeature(&feat), OGRERR_NONE);

        GDALClose(ds);
    }

    // 完全重新打开并验证所有数据
    {
        GDALDataset* ds = (GDALDataset*)GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        ASSERT_NE(ds, nullptr);

        OGRLayer* layer = ds->GetLayerByName("persist_test");
        ASSERT_NE(layer, nullptr);

        EXPECT_EQ(countFeatures(layer), 1) << "要素应持久化";

        layer->ResetReading();
        OGRFeature* feat = layer->GetNextFeature();
        ASSERT_NE(feat, nullptr);
        EXPECT_EQ(feat->GetFieldAsInteger("id"), 999);

        OGRPoint* pt = (OGRPoint*)feat->GetGeometryRef();
        ASSERT_NE(pt, nullptr);
        EXPECT_DOUBLE_EQ(pt->getX(), 42.0);
        EXPECT_DOUBLE_EQ(pt->getY(), 42.0);

        OGRFeature::DestroyFeature(feat);
        GDALClose(ds);
    }
}
