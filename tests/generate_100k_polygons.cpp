// tests/generate_100k_polygons.cpp
// 生成 10 万个面要素的 GDB 文件，用于 ArcGIS Pro 索引测试

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <chrono>
#include <filesystem>

#include "gdb_table_writer.h"
#include "geometry_serializer.h"
#include "explorgdb_types.h"

// GDAL 依赖（仅用于创建 schema）
#include "gdal.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

namespace fs = std::filesystem;

using namespace explorgdb;
using namespace explorgdb::writer;

// 使用 GDAL 创建 schema
bool create_schema(const std::string& gdb_path, const std::string& layer_name) {
    GDALAllRegister();

    // 使用 GDAL 创建空 GDB（不需要手动创建目录）
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!driver) {
        std::cerr << "错误: OpenFileGDB 驱动不可用" << std::endl;
        return false;
    }

    // 创建数据集
    char** options = nullptr;
    GDALDataset* ds = driver->Create(gdb_path.c_str(), 0, 0, 0, GDT_Unknown, options);
    if (!ds) {
        std::cerr << "错误: 无法创建 GDB" << std::endl;
        return false;
    }

    // 创建图层
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    OGRLayer* layer = ds->CreateLayer(layer_name.c_str(), &srs, wkbPolygon, nullptr);
    if (!layer) {
        std::cerr << "错误: 无法创建图层" << std::endl;
        GDALClose(ds);
        return false;
    }

    // 添加字段
    OGRFieldDefn name_field("name", OFTString);
    name_field.SetWidth(100);
    layer->CreateField(&name_field);

    OGRFieldDefn pop_field("population", OFTInteger);
    layer->CreateField(&pop_field);

    OGRFieldDefn area_field("area", OFTReal);
    layer->CreateField(&area_field);

    OGRFieldDefn cat_field("category", OFTString);
    cat_field.SetWidth(50);
    layer->CreateField(&cat_field);

    GDALClose(ds);
    return true;
}

// 创建一个简单的正方形多边形
void create_square_polygon(GeometrySerializer& ser, double cx, double cy, double size) {
    double half = size / 2.0;
    std::vector<GeomPoint> ring = {
        {cx - half, cy - half},
        {cx + half, cy - half},
        {cx + half, cy + half},
        {cx - half, cy + half},
        {cx - half, cy - half}  // 闭合
    };
    ser.set_rings({ring});
    ser.serialize(GeomType::Polygon);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <output_gdb_path>" << std::endl;
        return 1;
    }

    std::string gdb_path = argv[1];
    std::string layer_name = "polygons_100k";
    const int NUM_FEATURES = 100000;  // 10 万个要素

    std::cout << "=== 生成 10 万面数据 GDB ===" << std::endl;
    std::cout << "输出路径: " << gdb_path << std::endl;
    std::cout << "图层名称: " << layer_name << std::endl;
    std::cout << "要素数量: " << NUM_FEATURES << std::endl;
    std::cout << std::endl;

    // 步骤 1: 使用 GDAL 创建 schema
    std::cout << "[1/3] 创建 GDB schema (GDAL)..." << std::endl;
    if (!create_schema(gdb_path, layer_name)) {
        std::cerr << "错误: 无法创建 GDB schema" << std::endl;
        return 1;
    }
    std::cout << "Schema 创建完成" << std::endl;
    std::cout << std::endl;

    // 步骤 2: 使用我们的 writer 填充数据
    std::cout << "[2/3] 写入 " << NUM_FEATURES << " 个面要素 (explorgdb writer)..." << std::endl;

    GdbTableWriter writer;
    if (!writer.open_existing(gdb_path, layer_name)) {
        std::cerr << "错误: 无法打开 GDB" << std::endl;
        return 1;
    }

    auto start = std::chrono::high_resolution_clock::now();
    auto& geom_ser = writer.geometry_serializer();

    // 生成 316x316 的网格（约 10 万个格子）
    int grid_size = static_cast<int>(std::sqrt(NUM_FEATURES));
    double start_x = 100.0;  // 起始经度
    double start_y = 30.0;   // 起始纬度
    double cell_size = 0.01; // 每个格子 0.01 度（约 1km）

    int count = 0;
    for (int i = 0; i < grid_size && count < NUM_FEATURES; ++i) {
        for (int j = 0; j < grid_size && count < NUM_FEATURES; ++j) {
            double cx = start_x + i * cell_size;
            double cy = start_y + j * cell_size;

            // 创建多边形
            create_square_polygon(geom_ser, cx, cy, cell_size * 0.8);

            // 写入行
            writer.begin_row();
            writer.append_string(0, "Polygon_" + std::to_string(count));
            writer.append_i32(1, 10000 + count * 10);  // population
            writer.append_f64(2, cell_size * cell_size * 111.32 * 110.54);  // area (km²)
            writer.append_string(3, count % 5 == 0 ? "A" : (count % 5 == 1 ? "B" : "C"));
            writer.append_geometry(4);
            writer.end_row();

            count++;
        }

        // 每 1 万个要素打印进度
        if (count % 10000 == 0) {
            std::cout << "  已写入: " << count << " / " << NUM_FEATURES << std::endl;
        }
    }

    auto end_write = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> write_time = end_write - start;

    std::cout << std::endl;
    std::cout << "写入完成!" << std::endl;
    std::cout << "  要素数: " << count << std::endl;
    std::cout << "  耗时: " << write_time.count() << " ms" << std::endl;
    std::cout << "  速度: " << (count / write_time.count() * 1000.0) << " features/sec" << std::endl;
    std::cout << "  平均每要素: " << (write_time.count() * 1000.0 / count) << " us" << std::endl;
    std::cout << std::endl;

    // 关闭 writer
    std::cout << "[3/3] 关闭 GDB..." << std::endl;
    writer.close();

    std::cout << std::endl;
    std::cout << "=== 完成 ===" << std::endl;
    std::cout << "GDB 文件已生成: " << gdb_path << std::endl;
    std::cout << std::endl;
    std::cout << "下一步:" << std::endl;
    std::cout << "1. 用 ArcGIS Pro 打开此 GDB" << std::endl;
    std::cout << "2. 右键图层 → Properties → Indexes" << std::endl;
    std::cout << "3. 创建空间索引和属性索引" << std::endl;
    std::cout << "4. 保存后带回测试我们的 reader" << std::endl;

    return 0;
}
