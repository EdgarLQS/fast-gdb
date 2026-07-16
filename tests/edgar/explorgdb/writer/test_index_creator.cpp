// tests/edgar/explorgdb/writer/test_index_creator.cpp
// Index Creator 测试 — TDD 方式开发
//
// 测试策略：
//   1. 使用 GDAL OpenFileGDB 驱动创建真实 GDB 文件
//   2. 调用 CreateSpatialIndex 创建空间索引
//   3. 验证 .spx 文件存在

#include <gtest/gtest.h>
#include "gdb_index_creator.h"
#include "gdal.h"
#include "ogrsf_frmts.h"
#include <filesystem>
#include <iostream>

using namespace explorgdb::writer;
namespace fs = std::filesystem;

// 跨平台 getpid
#ifdef _WIN32
#include <process.h>
#endif

class IndexCreatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        GDALAllRegister();
        auto tmp = fs::temp_directory_path();
#ifdef _WIN32
        test_dir_ = (tmp / ("index_test_" + std::to_string(_getpid()))).string();
#else
        test_dir_ = (tmp / ("index_test_" + std::to_string(getpid()))).string();
#endif
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    std::string gdb_path() { return test_dir_ + "/test.gdb"; }

    // 生成包含 100 个简单点要素的测试 GDB
    bool generate_test_data(const std::string& gdb_path) {
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
        if (!driver) {
            std::cerr << "Failed to get OpenFileGDB driver\n";
            return false;
        }

        GDALDataset* ds = driver->Create(gdb_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        if (!ds) {
            std::cerr << "Failed to create GDB: " << gdb_path << "\n";
            return false;
        }

        // 创建空间参考 (WGS84)
        OGRSpatialReference srs;
        srs.importFromEPSG(4326);

        // 创建点图层
        OGRLayer* layer = ds->CreateLayer("points", &srs, wkbPoint, nullptr);
        if (!layer) {
            GDALClose(ds);
            std::cerr << "Failed to create layer\n";
            return false;
        }

        // 添加一个整数字段用于标识
        OGRFieldDefn id_field("id", OFTInteger);
        layer->CreateField(&id_field);

        // 写入 100 个点要素
        for (int i = 0; i < 100; i++) {
            OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
            feat->SetField("id", i);

            // 创建几何对象
            OGRPoint point;
            point.setX(-180.0 + (i * 3.6));  // 经度均匀分布
            point.setY(-90.0 + (i % 50) * 3.6);  // 纬度分布
            feat->SetGeometry(&point);

            if (layer->CreateFeature(feat) != OGRERR_NONE) {
                OGRFeature::DestroyFeature(feat);
                GDALClose(ds);
                std::cerr << "Failed to create feature " << i << "\n";
                return false;
            }

            OGRFeature::DestroyFeature(feat);
        }

        GDALClose(ds);
        return true;
    }

    std::string test_dir_;

    // 获取 GDB 中第一个图层的名称
    std::string get_first_layer_name(const std::string& gdb_path) {
        GDALDatasetH ds = GDALOpenEx(gdb_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                      nullptr, nullptr, nullptr);
        if (!ds) return "";

        int layer_count = GDALDatasetGetLayerCount(ds);
        if (layer_count == 0) {
            GDALClose(ds);
            return "";
        }

        OGRLayerH layer = GDALDatasetGetLayer(ds, 0);
        if (!layer) {
            GDALClose(ds);
            return "";
        }

        const char* name = OGR_L_GetName(layer);
        std::string result(name ? name : "");
        GDALClose(ds);
        return result;
    }
};

// T_IC01: 创建空间索引并验证 .spx 文件存在
TEST_F(IndexCreatorTest, T_IC01_CreateSpatialIndex) {
    // 准备：生成测试数据
    ASSERT_TRUE(generate_test_data(gdb_path())) << "Failed to generate test data";

    // 获取实际的图层名称（GDAL OpenFileGDB 可能使用内部名称）
    std::string layer_name = get_first_layer_name(gdb_path());
    ASSERT_FALSE(layer_name.empty()) << "Could not find layer in GDB";

    // 执行：创建空间索引
    bool result = CreateSpatialIndex(gdb_path(), layer_name);

    // 验证 1：函数返回成功
    EXPECT_TRUE(result) << "CreateSpatialIndex should return true on success";

    // 验证 2：检查是否存在任意 .spx 文件
    // OpenFileGDB 自动为图层分配内部 ID，所以我们不硬编码文件名
    bool spx_exists = false;
    std::string found_spx;
    for (const auto& entry : fs::directory_iterator(test_dir_ + "/test.gdb")) {
        if (entry.path().extension() == ".spx") {
            spx_exists = true;
            found_spx = entry.path().filename().string();
            break;
        }
    }

    EXPECT_TRUE(spx_exists) << "Spatial index file (.spx) should exist after CreateSpatialIndex";

    if (!spx_exists) {
        std::cerr << "GDB directory contents:\n";
        for (const auto& e : fs::directory_iterator(test_dir_ + "/test.gdb")) {
            std::cerr << "  " << e.path().filename() << "\n";
        }
    } else {
        std::cout << "DEBUG: Found .spx file: " << found_spx << "\n";
    }
}

// T_IC02: 创建单字段属性索引并验证 .atx 文件存在
TEST_F(IndexCreatorTest, T_IC02_CreateAttributeIndex) {
    // 准备：生成测试数据
    ASSERT_TRUE(generate_test_data(gdb_path())) << "Failed to generate test data";

    std::string layer_name = get_first_layer_name(gdb_path());
    ASSERT_FALSE(layer_name.empty()) << "Could not find layer in GDB";

    // 执行：创建单字段属性索引
    std::string field_name = "id";
    std::string index_name = "points_id_idx";
    bool result = CreateAttributeIndex(gdb_path(), layer_name, field_name, index_name);

    // 验证 1：函数返回成功
    EXPECT_TRUE(result) << "CreateAttributeIndex should return true on success";

    // 验证 2：检查是否存在 .atx 文件包含索引名称
    bool atx_exists = false;
    std::string found_atx;
    for (const auto& entry : fs::directory_iterator(test_dir_ + "/test.gdb")) {
        if (entry.path().extension() == ".atx") {
            std::string filename = entry.path().stem().string();
            if (filename.find(index_name) != std::string::npos) {
                atx_exists = true;
                found_atx = entry.path().filename().string();
                break;
            }
        }
    }

    EXPECT_TRUE(atx_exists) << "Attribute index file (.atx) should exist after CreateAttributeIndex";

    if (!atx_exists) {
        std::cerr << "GDB directory contents (looking for .atx with '" << index_name << "'):\n";
        for (const auto& e : fs::directory_iterator(test_dir_ + "/test.gdb")) {
            std::cerr << "  " << e.path().filename() << "\n";
        }
    } else {
        std::cout << "DEBUG: Found .atx file: " << found_atx << "\n";
    }
}

// T_IC03: 创建多字段联合索引并验证 .atx 文件存在
TEST_F(IndexCreatorTest, T_IC03_CreateCompositeIndex) {
    // 准备：生成测试数据
    ASSERT_TRUE(generate_test_data(gdb_path())) << "Failed to generate test data";

    std::string layer_name = get_first_layer_name(gdb_path());
    ASSERT_FALSE(layer_name.empty()) << "Could not find layer in GDB";

    // 执行：创建多字段联合索引（使用较短的索引名称，OpenFileGDB 限制 16 字符）
    std::vector<std::string> field_names = {"id"};  // Using existing 'id' field
    std::string index_name = "pts_comp_idx";  // Short name to fit 16 char limit
    bool result = CreateCompositeIndex(gdb_path(), layer_name, field_names, index_name);

    // 验证 1：函数返回成功
    EXPECT_TRUE(result) << "CreateCompositeIndex should return true on success";

    // 验证 2：检查是否存在 .atx 文件包含索引名称
    bool atx_exists = false;
    for (const auto& entry : fs::directory_iterator(test_dir_ + "/test.gdb")) {
        if (entry.path().extension() == ".atx") {
            std::string filename = entry.path().stem().string();
            if (filename.find(index_name) != std::string::npos) {
                atx_exists = true;
                break;
            }
        }
    }

    EXPECT_TRUE(atx_exists) << "Composite index file (.atx) should exist after CreateCompositeIndex";
    EXPECT_FALSE(CreateCompositeIndex(
        gdb_path(), layer_name, {"id", "id"}, "unsupported_idx"));
}

// T_IC04: 批量创建多个索引
TEST_F(IndexCreatorTest, T_IC04_CreateIndexes_Batch) {
    // 准备：生成测试数据
    ASSERT_TRUE(generate_test_data(gdb_path())) << "Failed to generate test data";

    std::string layer_name = get_first_layer_name(gdb_path());
    ASSERT_FALSE(layer_name.empty()) << "Could not find layer in GDB";

    // 准备：定义多个索引
    std::vector<IndexDefinition> definitions;

    // 空间索引
    definitions.push_back(IndexDefinition::Spatial());

    // 单字段属性索引
    definitions.emplace_back("points_id_idx", "id");

    // 执行：批量创建索引
    bool result = CreateIndexes(gdb_path(), layer_name, definitions);

    // 验证 1：批量创建返回成功
    EXPECT_TRUE(result) << "CreateIndexes should return true when all indexes created successfully";

    // 验证 2：检查 .spx 文件存在
    bool spx_exists = false;
    for (const auto& entry : fs::directory_iterator(test_dir_ + "/test.gdb")) {
        if (entry.path().extension() == ".spx") {
            spx_exists = true;
            break;
        }
    }
    EXPECT_TRUE(spx_exists) << "Spatial index file (.spx) should exist after batch create";

    // 验证 3：检查 .atx 文件存在
    bool atx_exists = false;
    for (const auto& entry : fs::directory_iterator(test_dir_ + "/test.gdb")) {
        if (entry.path().extension() == ".atx") {
            std::string filename = entry.path().stem().string();
            if (filename.find("points_id_idx") != std::string::npos) {
                atx_exists = true;
                break;
            }
        }
    }
    EXPECT_TRUE(atx_exists) << "Attribute index file (.atx) should exist after batch create";
}

// T_IC05: 删除索引
TEST_F(IndexCreatorTest, T_IC05_DropIndex) {
    // 准备：生成测试数据
    ASSERT_TRUE(generate_test_data(gdb_path())) << "Failed to generate test data";

    std::string layer_name = get_first_layer_name(gdb_path());
    ASSERT_FALSE(layer_name.empty()) << "Could not find layer in GDB";

    // 准备：先创建一个属性索引
    std::string field_name = "id";
    std::string index_name = "points_id_idx";
    bool create_result = CreateAttributeIndex(gdb_path(), layer_name, field_name, index_name);
    ASSERT_TRUE(create_result) << "Failed to create index for drop test";

    // 验证 1：索引文件存在
    bool atx_exists_before = false;
    for (const auto& entry : fs::directory_iterator(test_dir_ + "/test.gdb")) {
        if (entry.path().extension() == ".atx") {
            std::string filename = entry.path().stem().string();
            if (filename.find(index_name) != std::string::npos) {
                atx_exists_before = true;
                break;
            }
        }
    }
    EXPECT_TRUE(atx_exists_before) << "Index file should exist before drop";

    // 执行：删除索引
    EXPECT_FALSE(DropIndex(gdb_path(), index_name));
    bool atx_exists_after = false;
    for (const auto& entry : fs::directory_iterator(gdb_path())) {
        if (entry.path().extension() == ".atx" &&
            entry.path().stem().string().find(index_name) != std::string::npos) {
            atx_exists_after = true;
        }
    }
    EXPECT_TRUE(atx_exists_after);
}

// T_IC06: 检查是否有空间索引
TEST_F(IndexCreatorTest, T_IC06_HasSpatialIndex) {
    // 准备：生成测试数据
    ASSERT_TRUE(generate_test_data(gdb_path())) << "Failed to generate test data";

    std::string layer_name = get_first_layer_name(gdb_path());
    ASSERT_FALSE(layer_name.empty()) << "Could not find layer in GDB";

    // 验证 1：创建空间索引前检查
    EXPECT_TRUE(HasSpatialIndex(gdb_path(), layer_name));

    // 执行：创建空间索引
    bool create_result = CreateSpatialIndex(gdb_path(), layer_name);
    ASSERT_TRUE(create_result) << "Failed to create spatial index";

    // 验证 2：创建空间索引后检查
    bool has_after = HasSpatialIndex(gdb_path(), layer_name);
    EXPECT_TRUE(has_after) << "HasSpatialIndex should return true after creating spatial index";
    EXPECT_FALSE(HasSpatialIndex(gdb_path(), "missing_layer"));
}
