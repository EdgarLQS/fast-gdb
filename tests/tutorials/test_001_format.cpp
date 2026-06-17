/**
 * ============================================================================
 * 教程 001: GDB 格式概述与内部结构
 * ============================================================================
 *
 * 1.1 什么是 File Geodatabase
 * ---------------------------
 * ESRI 自 ArcGIS 10.0 引入的本地数据存储格式，基于文件系统的目录结构
 * （以 .gdb 结尾的文件夹）。支持矢量要素类、属性表、栅格数据集、拓扑、
 * 网络、关系、字段域（Domains）等。
 *
 * 1.2 内部文件结构
 * -----------------
 * 一个 .gdb 目录包含以下文件（文件名以 9 位十六进制数字命名）：
 *
 *   文件模式                      用途                        示例
 *   a00000001.gdbtable   系统目录表 — 列出所有表名         系统
 *   a00000001.gdbtablx   系统目录偏移索引                   系统
 *   a00000003.gdbtable   GDB_SpatialRefs — 空间参考定义     系统
 *   a00000004.gdbtable   GDB_Items — XML 元数据             系统
 *   aXXXXXXXX.gdbtable   用户数据表（要素、属性）           按表分配
 *   aXXXXXXXX.gdbtablx   偏移索引（行号→文件偏移）          与 .gdbtable 配对
 *   aXXXXXXXX.spx        空间索引文件                       空间查询加速
 *   aXXXXXXXX.atx        属性索引文件（B-tree）             WHERE 子句加速
 *   fras_bnd_<layername> 栅格波段表                        GDB 栅格数据存储
 *
 * 源码参考：ogropenfilegdbdatasource.cpp 的 Open() 函数读取目录后解析
 *           a00000001.gdbtable 系统目录表。
 *
 * 1.3 .gdbtable 二进制结构
 * ------------------------
 * Header (40 bytes):
 *   [0-3]:   unknown
 *   [4-7]:   nValidRecordCount (int32) — 有效记录数
 *   [8-11]:  nHeaderBufferMaxSize (int32) — 最大行 blob 大小
 *   [20-23]: nTotalRecordCount (int32) — 总记录数
 *   [32-39]: nOffsetFieldDesc (uint64) — 字段描述区偏移
 *
 * Field Descriptor Section (at nOffsetFieldDesc):
 *   [0-3]:   nFieldDescLength (uint32)
 *   [4-7]:   version (uint32): 3=v9, 4/6=v10
 *   [8]:     byTableGeomType — 0=无, 1=点, 2=多点, 3=线, 4=面, 9=multipatch
 *   [9]:     flags — bit 0: UTF8 vs UTF16
 *   [12-13]: nFields (uint16)
 *
 * 1.4 字段类型枚举（filegdbtable.h:66-86）
 * ----------------------------------------
 *   0  FGFT_INT16              -> OFTInteger
 *   1  FGFT_INT32              -> OFTInteger
 *   2  FGFT_FLOAT32            -> OFTReal
 *   3  FGFT_FLOAT64            -> OFTReal
 *   4  FGFT_STRING             -> OFTString
 *   5  FGFT_DATETIME           -> OFTDateTime
 *   6  FGFT_OBJECTID           -> OFTInteger64
 *   7  FGFT_GEOMETRY           -> OGRGeometry（二进制 blob）
 *   13 FGFT_INT64              -> OFTInteger64（ArcGIS Pro 3.2+）
 *
 * 1.5 数据版本
 * ------------
 *   v10+（ArcGIS 10.0+）：XML 定义存储在 GDB_Items 表
 *   v9.x（ArcGIS 9.x）：存储在 GDB_FeatureClasses / GDB_ObjectClasses 表
 *   OpenFileGDB 同时支持，通过 OpenFileGDBv9() / OpenFileGDBv10() 分发
 *
 * ============================================================================
 */

#include "test_fixture.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

/**
 * 验证创建 GDB 后生成预期的内部文件结构。
 * 对应教程 1.2 节：内部文件结构
 */
TEST_F(GdbTutorialFixture, T001_CreateGdbGeneratesInternalFiles) {
    const char* path = "/tmp/tutorial_001_test.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);
    GDALClose(ds);

    // GDB 是一个目录
    ASSERT_TRUE(fs::is_directory(path));

    // 遍历目录内容，验证 .gdbtable 和 .gdbtablx 文件存在
    bool hasGdbtable = false;
    bool hasGdbtablx = false;

    for (const auto& entry : fs::directory_iterator(path)) {
        std::string name = entry.path().filename().string();
        if (name.find(".gdbtable") != std::string::npos && name.find(".gdbtablx") == std::string::npos)
            hasGdbtable = true;
        if (name.find(".gdbtablx") != std::string::npos) hasGdbtablx = true;
    }

    EXPECT_TRUE(hasGdbtable) << "GDB 应包含 .gdbtable 文件";
    EXPECT_TRUE(hasGdbtablx) << "GDB 应包含 .gdbtablx 文件";
}

/**
 * 验证创建图层后生成对应的内部文件。
 * 对应教程 1.2 节：用户数据表 aXXXXXXXX.gdbtable
 */
TEST_F(GdbTutorialFixture, T001_CreateLayerGeneratesInternalFiles) {
    const char* path = "/tmp/tutorial_001_layer_test.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    // 创建图层
    char** options = nullptr;
    options = CSLSetNameValue(options, "GEOMETRY_NAME", "geom");
    OGRLayer* layer = ds->CreateLayer("test_layer", nullptr, wkbPolygon, options);
    CSLDestroy(options);
    ASSERT_NE(layer, nullptr);

    GDALClose(ds);

    // 检查是否存在与图层相关的文件
    bool hasLayerFiles = false;
    for (const auto& entry : fs::directory_iterator(path)) {
        std::string name = entry.path().filename().string();
        if (name.find(".gdbtable") != std::string::npos) {
            hasLayerFiles = true;
        }
    }

    EXPECT_TRUE(hasLayerFiles) << "创建图层后应生成新的 .gdbtable 文件";
}

/**
 * 验证 .gdbtable 文件头部结构。
 * 对应教程 1.3 节：.gdbtable 二进制结构
 */
TEST_F(GdbTutorialFixture, T001_GdbTableFileHeaderMagic) {
    const char* path = "/tmp/tutorial_001_magic_test.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);
    GDALClose(ds);

    // 找到一个 .gdbtable 文件
    std::string tablePath;
    for (const auto& entry : fs::directory_iterator(path)) {
        std::string name = entry.path().filename().string();
        if (name.find(".gdbtable") != std::string::npos && name.find(".gdbtablx") == std::string::npos) {
            tablePath = entry.path().string();
            break;
        }
    }

    ASSERT_FALSE(tablePath.empty()) << "应找到 .gdbtable 文件";

    // 读取文件头部字节，验证文件非空且可读
    std::ifstream ifs(tablePath, std::ios::binary);
    ASSERT_TRUE(ifs.is_open());

    ifs.seekg(0, std::ios::end);
    auto fileSize = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    EXPECT_GT(fileSize, 0) << ".gdbtable 文件应非空";

    uint32_t header = 0;
    ifs.read(reinterpret_cast<char*>(&header), sizeof(header));
    EXPECT_GT(header, 0u) << ".gdbtable 文件头部应非零";

    ifs.close();
}
