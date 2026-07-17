#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "cpl_error.h"
#include "cpl_string.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"
#include "test_fixture.h"

using namespace explorgdb;

namespace {

bool execute_sql(GDALDataset* dataset, const std::string& sql) {
    CPLErrorReset();
    OGRLayer* result = dataset->ExecuteSQL(sql.c_str(), nullptr, nullptr);
    if (result) dataset->ReleaseResultSet(result);
    return CPLGetLastErrorType() < CE_Failure;
}

OGRGeometry* geometry_from_wkt(const std::string& text) {
    const char* cursor = text.c_str();
    OGRGeometry* geometry = nullptr;
    if (OGRGeometryFactory::createFromWkt(
            &cursor, nullptr, &geometry) != OGRERR_NONE) {
        return nullptr;
    }
    return geometry;
}

std::vector<uint32_t> collect_gdal(OGRLayer* layer) {
    std::vector<uint32_t> result;
    layer->SetSpatialFilterRect(0.0, 0.0, 10.0, 10.0);
    if (layer->SetAttributeFilter("keep = 1") != OGRERR_NONE)
        return result;
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        if (feature->GetFID() > 0)
            result.push_back(static_cast<uint32_t>(feature->GetFID() - 1));
        OGRFeature::DestroyFeature(feature);
    }
    layer->SetSpatialFilter(nullptr);
    layer->SetAttributeFilter(nullptr);
    std::sort(result.begin(), result.end());
    return result;
}

struct DimensionCase {
    const char* suffix;
    OGRwkbGeometryType type;
    const char* near_wkt;
    const char* far_wkt;
};

} // namespace

class SpatialWhereDimensionTest : public GdbTutorialFixture {};

TEST_F(SpatialWhereDimensionTest, PointZPointMAndPointZMUseXYForBbox) {
    const DimensionCase cases[] = {
        {"z", wkbPoint25D, "POINT Z (1 1 100)",
         "POINT Z (20 20 200)"},
        {"m", wkbPointM, "POINT M (1 1 500)",
         "POINT M (20 20 600)"},
        {"zm", wkbPointZM, "POINT ZM (1 1 100 500)",
         "POINT ZM (20 20 200 600)"},
    };

    for (const DimensionCase& test_case : cases) {
        SCOPED_TRACE(test_case.suffix);
        const std::string layer_name =
            std::string("combined_point_") + test_case.suffix;
        const std::string path =
            (std::filesystem::temp_directory_path() /
             ("fast_gdb_spatial_where_dimension_" +
              std::string(test_case.suffix) + ".gdb")).string();
        GDALDataset* dataset = createGdb(path.c_str());
        ASSERT_NE(dataset, nullptr);

        char** options = nullptr;
        options = CSLSetNameValue(
            options, "TARGET_ARCGIS_VERSION", "ARCGIS_PRO_3_2_OR_LATER");
        OGRLayer* layer = dataset->CreateLayer(
            layer_name.c_str(), nullptr, test_case.type, options);
        CSLDestroy(options);
        ASSERT_NE(layer, nullptr);
        OGRFieldDefn keep("keep", OFTInteger);
        ASSERT_EQ(layer->CreateField(&keep), OGRERR_NONE);

        const char* wkts[] = {test_case.near_wkt, test_case.far_wkt};
        for (const char* wkt : wkts) {
            OGRGeometry* geometry = geometry_from_wkt(wkt);
            ASSERT_NE(geometry, nullptr);
            OGRFeature* feature = OGRFeature::CreateFeature(
                layer->GetLayerDefn());
            feature->SetField("keep", 1);
            ASSERT_EQ(feature->SetGeometry(geometry), OGRERR_NONE);
            ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
            OGRFeature::DestroyFeature(feature);
            OGRGeometryFactory::destroyGeometry(geometry);
        }
        GDALClose(dataset);

        dataset = static_cast<GDALDataset*>(GDALOpenEx(
            path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
            nullptr, nullptr, nullptr));
        ASSERT_NE(dataset, nullptr);
        layer = dataset->GetLayerByName(layer_name.c_str());
        ASSERT_NE(layer, nullptr);
        layer->ResetReading();
        while (OGRFeature* feature = layer->GetNextFeature()) {
            ASSERT_EQ(layer->SetFeature(feature), OGRERR_NONE);
            OGRFeature::DestroyFeature(feature);
        }
        ASSERT_TRUE(execute_sql(
            dataset, "RECOMPUTE EXTENT ON " + layer_name));
        ASSERT_TRUE(execute_sql(
            dataset, "CREATE INDEX keep_idx ON " +
                     layer_name + "(keep)"));
        GDALClose(dataset);

        GdbCatalog catalog;
        ASSERT_TRUE(catalog.scan(path));
        CatalogResolver resolver(catalog);
        ASSERT_TRUE(resolver.load());
        const auto resolved = resolver.resolve(layer_name);
        ASSERT_TRUE(resolved.has_value());
        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open());

        QueryRequest request;
        request.kind = QueryKind::SpatialWhere;
        request.xmin = 0.0;
        request.ymin = 0.0;
        request.xmax = 10.0;
        request.ymax = 10.0;
        request.where_clause = "keep = 1";
        const QueryResult fast = engine.query(request);
        EXPECT_EQ(fast.execution_path, "spatial-where:spx+atx");
        EXPECT_EQ(fast.matched_fids, (std::vector<uint32_t>{0}));

        dataset = static_cast<GDALDataset*>(GDALOpenEx(
            path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr));
        ASSERT_NE(dataset, nullptr);
        layer = dataset->GetLayerByName(layer_name.c_str());
        ASSERT_NE(layer, nullptr);
        EXPECT_EQ(fast.matched_fids, collect_gdal(layer));
        GDALClose(dataset);
    }
}
