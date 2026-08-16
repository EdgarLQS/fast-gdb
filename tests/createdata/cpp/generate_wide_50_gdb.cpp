// generate_wide_50_gdb.cpp
// 使用 GDAL C API 创建含 50 个字段的 FileGDB 宽表
// 输出: test_data/benchmark/wide_50_gdal.gdb/

#include "gdal.h"
#include "ogr_api.h"
#include "cpl_string.h"
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char* argv[]) {
    const char* output_path = argc > 1
        ? argv[1]
        : "test_data/benchmark/wide_50_gdal.gdb";

    GDALAllRegister();
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");

    // Delete existing
    GDALDatasetH existing = GDALOpenEx(output_path, GDAL_OF_VECTOR | GDAL_OF_UPDATE,
                                       nullptr, nullptr, nullptr);
    if (existing) {
        GDALClose(existing);
        std::string cmd = std::string("rm -rf ") + output_path;
        system(cmd.c_str());
    }

    // Create FileGDB
    GDALDriverH driver = GDALGetDriverByName("OpenFileGDB");
    if (!driver) {
        fprintf(stderr, "ERROR: OpenFileGDB driver not available\n");
        return 1;
    }

    GDALDatasetH dataset = GDALCreate(driver, output_path, 0, 0, 0, GDT_Unknown, nullptr);
    if (!dataset) {
        fprintf(stderr, "ERROR: Failed to create %s\n", output_path);
        return 1;
    }

    // Create layer
    OGRLayerH layer = GDALDatasetCreateLayer(dataset, "wide_table", nullptr, wkbPoint, nullptr);
    if (!layer) {
        fprintf(stderr, "ERROR: Failed to create layer\n");
        GDALClose(dataset);
        return 1;
    }

    // Add fields: int_{0..19}, double_{0..14}, text_{0..14}
    char name[64];

    // keep field for query filtering
    OGRFieldDefnH keep_field = OGR_Fld_Create("keep", OFTInteger);
    if (keep_field) { OGR_L_CreateField(layer, keep_field, 1); OGR_Fld_Destroy(keep_field); }

    for (int i = 0; i < 20; ++i) {
        snprintf(name, sizeof(name), "int_%d", i);
        OGRFieldDefnH field = OGR_Fld_Create(name, OFTInteger);
        if (field) { OGR_L_CreateField(layer, field, 1); OGR_Fld_Destroy(field); }
    }
    for (int i = 0; i < 15; ++i) {
        snprintf(name, sizeof(name), "double_%d", i);
        OGRFieldDefnH field = OGR_Fld_Create(name, OFTReal);
        if (field) { OGR_L_CreateField(layer, field, 1); OGR_Fld_Destroy(field); }
    }
    for (int i = 0; i < 15; ++i) {
        snprintf(name, sizeof(name), "text_%d", i);
        OGRFieldDefnH field = OGR_Fld_Create(name, OFTString);
        if (field) { OGR_Fld_SetWidth(field, 64); OGR_L_CreateField(layer, field, 1); OGR_Fld_Destroy(field); }
    }

    // Insert 100 features
    for (int fid = 1; fid <= 100; ++fid) {
        OGRFeatureH feature = OGR_F_Create(OGR_L_GetLayerDefn(layer));
        OGRGeometryH pt = OGR_G_CreateGeometry(wkbPoint);
        OGR_G_SetPoint_2D(pt, 0, fid * 1.0, fid * 2.0);
        OGR_F_SetGeometry(feature, pt);
        OGR_F_SetFID(feature, fid);
        OGR_F_SetFieldInteger(feature, OGR_F_GetFieldIndex(feature, "keep"), fid <= 50 ? 1 : 0);

        for (int i = 0; i < 20; ++i) {
            snprintf(name, sizeof(name), "int_%d", i);
            OGR_F_SetFieldInteger(feature, OGR_F_GetFieldIndex(feature, name), fid * (i + 1));
        }
        for (int i = 0; i < 15; ++i) {
            snprintf(name, sizeof(name), "double_%d", i);
            OGR_F_SetFieldDouble(feature, OGR_F_GetFieldIndex(feature, name), fid * 1.5 + i * 0.1);
        }
        for (int i = 0; i < 15; ++i) {
            snprintf(name, sizeof(name), "text_%d", i);
            char val[64];
            snprintf(val, sizeof(val), "value_%d_%d", fid, i);
            OGR_F_SetFieldString(feature, OGR_F_GetFieldIndex(feature, name), val);
        }

        OGR_L_CreateFeature(layer, feature);
        OGR_G_DestroyGeometry(pt);
        OGR_F_Destroy(feature);
    }

    // Rebuild spatial index
    OGR_L_SetAttributeFilter(layer, nullptr);
    OGR_L_ResetReading(layer);
    OGRFeatureH f;
    while ((f = OGR_L_GetNextFeature(layer)) != nullptr) {
        OGR_L_SetFeature(layer, f);
        OGR_F_Destroy(f);
    }
    GDALDatasetExecuteSQL(dataset, "RECOMPUTE EXTENT ON wide_table", nullptr, nullptr);
    GDALDatasetExecuteSQL(dataset, "CREATE INDEX keep_idx ON wide_table(keep)", nullptr, nullptr);

    GDALClose(dataset);

    // Verify
    dataset = GDALOpenEx(output_path, GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (dataset) {
        OGRLayerH verify_layer = GDALDatasetGetLayerByName(dataset, "wide_table");
        if (verify_layer) {
            fprintf(stdout, "OK: %s created with %d fields, %lld features\n",
                    output_path,
                    OGR_FD_GetFieldCount(OGR_L_GetLayerDefn(verify_layer)),
                    (long long)OGR_L_GetFeatureCount(verify_layer, 1));
        }
        GDALClose(dataset);
    }

    return 0;
}
