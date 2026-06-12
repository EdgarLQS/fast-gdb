/**
 * ============================================================================
 * 教程 004: GDB 栅格图层读取
 * ============================================================================
 *
 * 重要：OpenFileGDB 是 GDAL 中唯一支持 GDB 栅格读取的驱动。
 *
 * 4.1 GDB 栅格存储结构
 * ---------------------
 *   GDB 中的栅格存储在 fras_bnd_<layername> 系列文件中：
 *   - fras_bnd_<layername>.ras.bnd.0  波段 0 数据
 *   - fras_bnd_<layername>.ras.bnd.1  波段 1 数据
 *   - ...
 *   按块（block）存储，类似 GDAL 的分块缓存机制。
 *
 * 4.2 band_types 位域
 * -------------------
 *   波段类型存储在 GDB_Items XML 的 <RasterDef> 中：
 *   - bit 0-3: 波段数据类型（Byte, Int16, UInt16, Int32, Float32, Float64）
 *   - bit 4: 有无 NoData 值
 *   - bit 5: 有无统计信息
 *   压缩格式：JPEG, LZ77, LZW, None 等。
 *
 * 4.3 子数据集访问
 * -----------------
 *   当 GDB 包含栅格时，可以通过 SUBDATASETS 元数据访问：
 *   gdalinfo 'OpenFileGDB:"mydata.gdb":elevation_raster'
 *
 *   C++ API:
 *   char** papszSDS = poDS->GetMetadata("SUBDATASETS");
 *
 * 4.4 栅格读取限制
 * -----------------
 *   - OpenFileGDB 不支持创建栅格数据集
 *   - FileGDB 驱动也不支持栅格写入
 *   - 栅格读取仅 OpenFileGDB 支持
 *
 * ============================================================================
 */

#include "test_fixture.h"
#include <filesystem>

/**
 * 验证 OpenFileGDB 驱动不支持创建栅格。
 * 对应教程 4.4 节：栅格读取限制
 */
TEST_F(GdbTutorialFixture, T004_OpenFileGdbCannotCreateRaster) {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    ASSERT_NE(drv, nullptr);

    // OpenFileGDB::Create() 仅支持矢量
    const char* path = "/tmp/tutorial_004_noraster.gdb";
    GDALDeleteDataset(drv, path);

    GDALDataset* ds = (GDALDataset*)drv->Create(path, 100, 100, 3, GDT_Byte, nullptr);
    // 如果创建了，验证它是矢量数据集（栅格数为 0）
    if (ds) {
        EXPECT_EQ(ds->GetRasterCount(), 0) << "OpenFileGDB Create 应创建矢量数据集";
        GDALClose(ds);
    } else {
        SUCCEED() << "OpenFileGDB 不支持创建栅格数据集";
    }
}

/**
 * 验证 GDAL 中有 GDB 栅格驱动注册。
 * 对应教程：OpenFileGDB 是唯一支持 GDB 栅格读取的驱动
 */
TEST_F(GdbTutorialFixture, T004_GdalHasRasterDrivers) {
    GDALDriverManager* dm = GetGDALDriverManager();

    // OpenFileGDB 应该存在
    GDALDriver* ofgdb = dm->GetDriverByName("OpenFileGDB");
    ASSERT_NE(ofgdb, nullptr) << "OpenFileGDB 驱动必须存在";

    // 验证 pfnIdentify 存在
    EXPECT_NE(ofgdb->pfnIdentify, nullptr);
}

/**
 * 验证创建矢量 GDB 后不包含 ras_bnd 文件。
 * 对应教程 4.1 节：ras_bnd 文件只在有栅格时生成
 */
TEST_F(GdbTutorialFixture, T004_VectorGdbHasNoFrasBnd) {
    const char* path = "/tmp/tutorial_004_vector.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    // 只创建矢量图层
    ds->CreateLayer("points", nullptr, wkbPoint, nullptr);
    GDALClose(ds);

    // 验证不存在 ras_bnd 相关文件
    bool hasFrasBnd = false;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        std::string name = entry.path().filename().string();
        if (name.find("ras_bnd") != std::string::npos || name.find("fras_bnd") != std::string::npos) {
            hasFrasBnd = true;
            break;
        }
    }

    EXPECT_FALSE(hasFrasBnd) << "纯矢量 GDB 不应包含 ras_bnd 文件";
}
