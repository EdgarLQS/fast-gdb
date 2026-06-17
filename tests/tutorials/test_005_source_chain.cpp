/**
 * ============================================================================
 * 教程 005: GDB 源码深度解析与调用链
 * ============================================================================
 *
 * 5.1 类层次结构
 * ---------------
 *   GDALDriver
 *     │
 *     ├── OGROpenFileGDBDriver (ogropenfilegdbdriver.cpp)
 *     │     注册 pfnOpen, pfnCreate, pfnIdentify
 *     │
 *     ├── OGROpenFileGDBDataSource (ogropenfilegdbdatasource.cpp)
 *     │     Open() — 打开 .gdb 目录，解析系统目录表
 *     │     GetLayer() — 通过名称或索引获取图层
 *     │
 *     ├── OpenFileGDBLayer (ogropenfilegdblayer.cpp)
 *     │     GetNextFeature() — 读取下一个要素
 *     │     GetLayerDefn() — 返回图层定义（字段、几何类型）
 *     │
 *     └── OpenFileGDBFeature (filegdbtable.cpp)
 *           ReadFeature() — 从 .gdbtable 二进制数据解析要素
 *
 * 5.2 三条核心调用链
 * -------------------
 *   调用链 1: 驱动注册
 *     GDALAllRegister()
 *       → RegisterOGROpenFileGDB()
 *       → poReg->RegisterDriver(new OGROpenFileGDBDriver())
 *       → GDALDriverManager::RegisterDriver()
 *
 *   调用链 2: 打开数据源
 *     GDALOpenEx(path, GDAL_OF_VECTOR)
 *       → OGROpenFileGDBDriver::Open()
 *       → OGROpenFileGDBDataSource::Open()
 *       → 扫描 .gdb 目录，解析 a00000001.gdbtable
 *       → 识别 v9/v10 版本，分发到对应处理函数
 *
 *   调用链 3: 读取要素
 *     OGRLayer::GetNextFeature()
 *       → OpenFileGDBLayer::GetNextFeature()
 *       → OpenFileGDBTable::ReadRow()
 *       → 解析 .gdbtable 二进制记录 → OGRFeature
 *
 * 5.3 二进制格式解析
 * -------------------
 *   filegdbtable.cpp (148KB) 是核心解析文件：
 *   - ReadHeader() 读取表头
 *   - ReadFieldDescriptors() 读取字段描述
 *   - ParseGeometry() 解析几何 blob
 *   - ParseFieldData() 解析字段值
 *
 * ============================================================================
 */

#include "test_fixture.h"

/**
 * 调用链 1: 驱动注册链验证
 * GDALAllRegister() → RegisterOGROpenFileGDB() → GDALDriverManager::RegisterDriver()
 */
TEST_F(GdbTutorialFixture, T005_DriverRegistrationChain) {
    // GDALAllRegister 已在 SetUp 中调用
    GDALDriverManager* dm = GetGDALDriverManager();
    ASSERT_NE(dm, nullptr);

    // 验证 OpenFileGDB 已注册
    GDALDriver* drv = dm->GetDriverByName("OpenFileGDB");
    ASSERT_NE(drv, nullptr);

    // 验证 pfnIdentify 存在（用于格式识别）
    EXPECT_NE(drv->pfnIdentify, nullptr) << "OpenFileGDB 应有 Identify 函数";

    // 验证 pfnOpen 存在（用于打开数据源）
    EXPECT_NE(drv->pfnOpen, nullptr) << "OpenFileGDB 应有 Open 函数";
}

/**
 * 调用链 2: 数据源打开链验证
 * GDALOpenEx() → OGROpenFileGDBDriver::Open() → OGROpenFileGDBDataSource::Open()
 */
TEST_F(GdbTutorialFixture, T005_DataSourceOpenChain) {
    // 创建 GDB 并写入一个图层，确保它是有效的
    const char* path = "/tmp/tutorial_005_dsopen.gdb";
    m_gdbPath = path;

    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    ASSERT_NE(drv, nullptr);

    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    // 创建图层确保数据有效
    OGRLayer* layer = ds->CreateLayer("test_open", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(1.0, 2.0);
    feat.SetGeometry(&pt);
    ASSERT_EQ(layer->CreateFeature(&feat), OGRERR_NONE);

    // 关闭写会话
    GDALClose(ds);

    // 通过 GDALOpenEx 以只读模式重新打开
    ds = (GDALDataset*)GDALOpenEx(path, GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr) << "GDALOpenEx 应能打开已创建的 GDB";

    // 验证图层可访问
    layer = ds->GetLayerByName("test_open");
    ASSERT_NE(layer, nullptr) << "应能通过 GetLayerByName 获取图层";

    GDALClose(ds);
}

/**
 * 调用链 3: 要素读取链验证
 * OGRLayer::GetNextFeature() → OpenFileGDBLayer::GetNextFeature() → OGRFeature
 */
TEST_F(GdbTutorialFixture, T005_FeatureReadChain) {
    const char* path = "/tmp/tutorial_005_feature.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    // 创建图层并写入要素
    OGRLayer* layer = ds->CreateLayer("test_layer", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn nameField("name", OFTString);
    nameField.SetWidth(30);
    ASSERT_EQ(layer->CreateField(&nameField), OGRERR_NONE);

    OGRFeature feat(layer->GetLayerDefn());
    feat.SetField("name", "chain_test");
    OGRPoint pt(10.0, 20.0);
    feat.SetGeometry(&pt);
    ASSERT_EQ(layer->CreateFeature(&feat), OGRERR_NONE);

    GDALClose(ds);

    // 重新打开并读取要素（验证 GetLayer → GetNextFeature 调用链）
    ds = (GDALDataset*)GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    ASSERT_NE(ds, nullptr);

    layer = ds->GetLayerByName("test_layer");
    ASSERT_NE(layer, nullptr) << "应能通过 GetLayerByName 获取图层";

    layer->ResetReading();
    OGRFeature* readFeat = layer->GetNextFeature();
    ASSERT_NE(readFeat, nullptr) << "应能通过 GetNextFeature 获取要素";

    EXPECT_STREQ(readFeat->GetFieldAsString("name"), "chain_test");
    ASSERT_NE(readFeat->GetGeometryRef(), nullptr);
    EXPECT_EQ(readFeat->GetGeometryRef()->getGeometryType(), wkbPoint);

    OGRFeature::DestroyFeature(readFeat);
    GDALClose(ds);
}
