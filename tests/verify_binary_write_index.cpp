// tests/verify_binary_write_index.cpp
// 关键验证：二进制写入后能否创建索引
//
// 测试场景：
//   1. 用 GDAL 创建空 GDB schema（只建图层和字段，不写数据）
//   2. 用 GdbTableWriter 直接二进制写入数据
//   3. 检查是否已有索引文件（预期：无）
//   4. 调用 CreateSpatialIndex
//   5. 检查是否创建了 .spx 文件
//   6. 调用 CreateAttributeIndex
//   7. 检查是否创建了 .atx 文件

#include "explorgdb/writer/gdb_table_writer.h"
#include "explorgdb/writer/gdb_index_creator.h"
#include "explorgdb/writer/geometry_serializer.h"
#include "gdal.h"
#include "ogr_api.h"
#include "ogr_srs_api.h"
#include <iostream>
#include <filesystem>
#include <cmath>

namespace fs = std::filesystem;
using namespace explorgdb::writer;

void list_index_files(const std::string& gdb_path, const std::string& label) {
    std::cout << "\n[" << label << "] Index files in " << gdb_path << ":\n";
    bool found = false;
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        auto ext = entry.path().extension();
        if (ext == ".spx" || ext == ".atx") {
            std::cout << "  " << entry.path().filename().string() << "\n";
            found = true;
        }
    }
    if (!found) {
        std::cout << "  (none)\n";
    }
}

int main() {
    GDALAllRegister();

    const std::string test_dir = "/tmp/binary_write_index_test";
    const std::string gdb_path = test_dir + "/test.gdb";
    const std::string layer_name = "test_points";

    // Cleanup
    fs::remove_all(test_dir);
    fs::create_directories(test_dir);

    // ── Step 1: Create schema using GDAL ──
    std::cout << "=== Step 1: Create schema using GDAL ===\n";
    {
        GDALDriverH driver = GDALGetDriverByName("OpenFileGDB");
        if (!driver) {
            std::cerr << "Failed to get OpenFileGDB driver\n";
            return 1;
        }

        char** options = nullptr;
        GDALDatasetH ds = GDALCreate(driver, gdb_path.c_str(), 0, 0, 0, GDT_Unknown, options);
        if (!ds) {
            std::cerr << "Failed to create GDB: " << gdb_path << "\n";
            return 1;
        }

        OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
        OSRSetWellKnownGeogCS(srs, "WGS84");

        OGRLayerH layer = GDALDatasetCreateLayer(ds, layer_name.c_str(), srs, wkbPoint, nullptr);
        if (!layer) {
            std::cerr << "Failed to create layer: " << layer_name << "\n";
            GDALClose(ds);
            OSRDestroySpatialReference(srs);
            return 1;
        }

        OGRFieldDefnH name_field = OGR_Fld_Create("name", OFTString);
        OGR_Fld_SetWidth(name_field, 50);
        if (OGR_L_CreateField(layer, name_field, TRUE) != OGRERR_NONE) {
            std::cerr << "Failed to create field 'name'\n";
            OGR_Fld_Destroy(name_field);
            OSRDestroySpatialReference(srs);
            GDALClose(ds);
            return 1;
        }
        OGR_Fld_Destroy(name_field);

        OGRFieldDefnH id_field = OGR_Fld_Create("id", OFTInteger);
        if (OGR_L_CreateField(layer, id_field, TRUE) != OGRERR_NONE) {
            std::cerr << "Failed to create field 'id'\n";
            OGR_Fld_Destroy(id_field);
            OSRDestroySpatialReference(srs);
            GDALClose(ds);
            return 1;
        }
        OGR_Fld_Destroy(id_field);

        OSRDestroySpatialReference(srs);
        GDALClose(ds);
    }
    list_index_files(gdb_path, "After schema creation");

    // ── Step 2: Write data using GdbTableWriter (binary direct write) ──
    std::cout << "\n=== Step 2: Write data using GdbTableWriter (binary) ===\n";
    {
        // Check if .spx exists BEFORE writing
        bool spx_before_write = false;
        for (const auto& entry : fs::directory_iterator(gdb_path)) {
            if (entry.path().extension() == ".spx") {
                spx_before_write = true;
                break;
            }
        }
        std::cout << "Spatial index (.spx) exists BEFORE binary write: " << (spx_before_write ? "YES" : "NO") << "\n";

        GdbTableWriter writer;
        if (!writer.open_existing(gdb_path, layer_name)) {
            std::cerr << "Failed to open GDB for writing\n";
            return 1;
        }

        auto& geom_ser = writer.geometry_serializer();

        for (int i = 0; i < 100; ++i) {
            // Create point geometry
            double x = 100.0 + i * 0.01;
            double y = 30.0 + i * 0.01;

            std::vector<GeomPoint> points = {{x, y}};
            geom_ser.set_points(points);
            geom_ser.serialize(GeomType::Point);

            writer.begin_row();
            writer.append_string(0, "Point_" + std::to_string(i));  // name field
            writer.append_i32(1, i);                                  // id field
            writer.append_geometry(2);                                // geometry field
            writer.end_row();
        }

        writer.close();
        std::cout << "Wrote " << writer.row_count() << " features via binary direct write\n";

        // Check if .spx exists AFTER writing
        bool spx_after_write = false;
        for (const auto& entry : fs::directory_iterator(gdb_path)) {
            if (entry.path().extension() == ".spx") {
                spx_after_write = true;
                break;
            }
        }
        std::cout << "Spatial index (.spx) exists AFTER binary write: " << (spx_after_write ? "YES" : "NO") << "\n";
    }
    list_index_files(gdb_path, "After binary writing");

    // ── Step 3: Try to create spatial index ──
    std::cout << "\n=== Step 3: Call CreateSpatialIndex ===\n";

    // First check if indexes exist BEFORE calling CreateSpatialIndex
    bool spx_before = false;
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        if (entry.path().extension() == ".spx") {
            spx_before = true;
            break;
        }
    }
    std::cout << "Spatial index (.spx) exists BEFORE CreateSpatialIndex: " << (spx_before ? "YES" : "NO") << "\n";

    bool spatial_result = CreateSpatialIndex(gdb_path, layer_name);
    std::cout << "CreateSpatialIndex returned: " << (spatial_result ? "true" : "false") << "\n";

    bool spx_after = false;
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        if (entry.path().extension() == ".spx") {
            spx_after = true;
            break;
        }
    }
    std::cout << "Spatial index (.spx) exists AFTER CreateSpatialIndex: " << (spx_after ? "YES" : "NO") << "\n";
    list_index_files(gdb_path, "After CreateSpatialIndex");

    // ── Step 4: Try to create attribute index ──
    std::cout << "\n=== Step 4: Call CreateAttributeIndex ===\n";
    bool attr_result = CreateAttributeIndex(gdb_path, layer_name, "name", "name_idx");
    std::cout << "CreateAttributeIndex returned: " << (attr_result ? "true" : "false") << "\n";
    list_index_files(gdb_path, "After CreateAttributeIndex");

    // ── Step 5: Summary ──
    std::cout << "\n=== Summary ===\n";
    std::cout << "Spatial index creation: " << (spatial_result ? "SUCCESS" : "FAILED") << "\n";
    std::cout << "Attribute index creation: " << (attr_result ? "SUCCESS" : "FAILED") << "\n";

    if (!spatial_result && !attr_result) {
        std::cout << "\n⚠ CRITICAL FINDING: Neither spatial nor attribute indexes were created!\n";
        std::cout << "  This means GDAL's CREATE INDEX SQL does not work on binary-written GDBs.\n";
        std::cout << "  The hybrid workflow (binary write → index creation) will NOT work.\n";
    } else if (spatial_result && !attr_result) {
        std::cout << "\n⚠ PARTIAL SUCCESS: Only spatial index was created.\n";
        std::cout << "  Attribute index creation failed - may need different approach.\n";
    } else if (!spatial_result && attr_result) {
        std::cout << "\n⚠ PARTIAL SUCCESS: Only attribute index was created.\n";
        std::cout << "  Spatial index creation failed - GDAL may not support it for binary writes.\n";
    } else {
        std::cout << "\n✓ BOTH INDEXES CREATED SUCCESSFULLY!\n";
        std::cout << "  The hybrid workflow is viable.\n";
    }

    // Cleanup (commented out for debugging)
    std::cout << "\nGDB kept at: " << gdb_path << " (for inspection)\n";
    std::cout << "Press Enter to continue and cleanup...";
    std::cin.get();

    fs::remove_all(test_dir);

    return 0;
}
