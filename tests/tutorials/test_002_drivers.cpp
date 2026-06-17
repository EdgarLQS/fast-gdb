/**
 * ============================================================================
 * 教程 002: 两套驱动对比与选择决策
 * ============================================================================
 *
 * 2.1 架构对比
 * ------------
 *   特性                OpenFileGDB                    FileGDB
 *   源码目录            ogr/ogrsf_frmts/openfilegdb/   ogr/ogrsf_frmts/filegdb/
 *   驱动名              OpenFileGDB                    FileGDB
 *   注册函数            RegisterOGROpenFileGDB()       RegisterOGRFileGDB()
 *   外部依赖            无                             FileGDB API SDK (ESRI)
 *   构建方式            默认内置（无依赖）             需 SDK 编译（GDAL_USE_FILEGDB）
 *   矢量读取            ✅ 支持                        ✅ 支持
 *   矢量写入            ✅ 支持（GDAL 3.6+）           ✅ 支持
 *   栅格读取            ✅ 支持                        ❌ 不支持
 *   栅格写入            ❌ 不支持                      ❌ 不支持
 *   CDF 压缩表          ❌ 不支持，回退到 FileGDB       ✅ 支持
 *   曲线几何            ✅ 支持（GDAL 2.2+）           ✅ 读取支持
 *   字段域 (Domains)    ✅ 读取+写入（GDAL 3.5+）      ✅ 读取+写入（GDAL 3.3+）
 *   关系                ✅ 支持（GDAL 3.6+）           ✅ 支持（GDAL 3.6+）
 *   事务                ✅ 模拟（文件系统级备份）       ✅ 原生
 *
 * 2.2 驱动协作机制
 * -----------------
 *   用户代码: GDALOpenEx("data.gdb", GDAL_OF_VECTOR)
 *     ↓
 *   OpenFileGDB 优先尝试（内置默认启用）
 *     ↓ 扫描目录，发现 .gdbtable.cdf 文件
 *     ↓ nLayersSDCOrCDF > 0
 *     ↓ 设置 @MAY_USE_OPENFILEGDB=NO 域名元数据
 *     ↓ 返回 NULL，让下一个驱动尝试
 *     ↓
 *   FileGDB 驱动:
 *     ↓ 检查 @MAY_USE_OPENFILEGDB 标记
 *     ↓ 使用 ESRI SDK 打开所有表（含 CDF）
 *
 * 2.3 选择决策
 * ------------
 *   一般 GDB 矢量读取/写入 -> OpenFileGDB（无依赖、内置可用）
 *   GDB 包含栅格数据       -> OpenFileGDB（FileGDB 不支持栅格）
 *   GDB 包含 CDF 压缩表     -> FileGDB（OpenFileGDB 无法解析 CDF）
 *   高性能批量写入          -> FileGDB（SDK 有 bulk load 优化）
 *
 * 2.4 CMake 构建配置
 * -------------------
 *   OpenFileGDB: ogr_optional_driver(openfilegdb OPENFILEGDB)  — 始终构建
 *   FileGDB:     ogr_dependent_driver(filegdb FileGDB "GDAL_USE_FILEGDB") — 需 SDK
 *
 * ============================================================================
 */

#include "test_fixture.h"

/**
 * 验证 OpenFileGDB 驱动已注册。
 * 对应教程 2.1 节：架构对比
 */
TEST_F(GdbTutorialFixture, T002_OpenFileGdbDriverRegistered) {
    GDALDriverManager* dm = GetGDALDriverManager();
    GDALDriver* drv = dm->GetDriverByName("OpenFileGDB");
    ASSERT_NE(drv, nullptr) << "OpenFileGDB 驱动必须已注册";

    const char* desc = drv->GetDescription();
    EXPECT_STREQ(desc, "OpenFileGDB") << "驱动描述应为 'OpenFileGDB'";

    // 验证驱动支持的操作
    EXPECT_NE(drv->pfnOpen, nullptr) << "OpenFileGDB 必须支持 Open 操作";
    EXPECT_NE(drv->pfnCreate, nullptr) << "OpenFileGDB 必须支持 Create 操作";
}

/**
 * 验证默认情况下使用 OpenFileGDB 打开 GDB。
 * 对应教程 2.2 节：驱动协作机制
 */
TEST_F(GdbTutorialFixture, T002_DefaultDriverIsOpenFileGdb) {
    const char* path = "/tmp/tutorial_002_default.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    // 获取数据集的驱动
    GDALDriver* drv = ds->GetDriver();
    ASSERT_NE(drv, nullptr);

    const char* name = drv->GetDescription();
    EXPECT_STREQ(name, "OpenFileGDB") << "默认使用的驱动应为 OpenFileGDB";

    GDALClose(ds);
}

/**
 * 验证 OpenFileGDB 支持创建、写入、读取的完整流程。
 * 对应教程 2.1 节：矢量写入 ✅ 支持
 */
TEST_F(GdbTutorialFixture, T002_OpenFileGdbFullCycle) {
    const char* path = "/tmp/tutorial_002_cycle.gdb";

    // 创建
    GDALDataset* writeDs = createGdb(path);
    ASSERT_NE(writeDs, nullptr);

    OGRLayer* layer = writeDs->CreateLayer("test", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn nameField("name", OFTString);
    nameField.SetWidth(50);
    ASSERT_EQ(layer->CreateField(&nameField), OGRERR_NONE);

    // 写入要素
    OGRFeature feat(layer->GetLayerDefn());
    feat.SetField("name", "test_feature");
    OGRPoint pt(1.0, 2.0);
    feat.SetGeometry(&pt);
    ASSERT_EQ(layer->CreateFeature(&feat), OGRERR_NONE);

    GDALClose(writeDs);

    // 重新打开读取
    GDALDataset* readDs = (GDALDataset*)GDALOpenEx(path, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    ASSERT_NE(readDs, nullptr);

    OGRLayer* readLayer = readDs->GetLayerByName("test");
    ASSERT_NE(readLayer, nullptr);
    EXPECT_EQ(countFeatures(readLayer), 1) << "应包含 1 个要素";

    GDALClose(readDs);
}
