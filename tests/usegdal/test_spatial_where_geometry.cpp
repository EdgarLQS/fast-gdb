#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "cpl_error.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"
#include "test_fixture.h"

using namespace explorgdb;

namespace {

bool sql(GDALDataset* dataset, const std::string& text) {
    CPLErrorReset();
    OGRLayer* result = dataset->ExecuteSQL(text.c_str(), nullptr, nullptr);
    if (result) dataset->ReleaseResultSet(result);
    return CPLGetLastErrorType() < CE_Failure;
}

bool build_indexes(GDALDataset* dataset, const std::string& layer_name) {
    OGRLayer* layer = dataset->GetLayerByName(layer_name.c_str());
    if (!layer) return false;
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        const OGRErr error = layer->SetFeature(feature);
        OGRFeature::DestroyFeature(feature);
        if (error != OGRERR_NONE) return false;
    }
    return sql(dataset, "RECOMPUTE EXTENT ON " + layer_name) &&
           sql(dataset, "CREATE INDEX keep_idx ON " +
                        layer_name + "(keep)");
}

std::vector<uint32_t> gdal_fids(OGRLayer* layer,
                                double xmin, double ymin,
                                double xmax, double ymax) {
    std::vector<uint32_t> result;
    layer->SetSpatialFilterRect(xmin, ymin, xmax, ymax);
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

void expect_fast_equals_gdal(const std::string& path,
                             const std::string& layer_name,
                             double xmin, double ymin,
                             double xmax, double ymax,
                             const std::vector<uint32_t>& expected) {
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
    request.xmin = xmin;
    request.ymin = ymin;
    request.xmax = xmax;
    request.ymax = ymax;
    request.where_clause = "keep = 1";
    const QueryResult fast = engine.query(request);
    EXPECT_EQ(fast.execution_path, "spatial-where:spx+atx");
    EXPECT_EQ(fast.matched_fids, expected);

    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName(layer_name.c_str());
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(fast.matched_fids,
              gdal_fids(layer, xmin, ymin, xmax, ymax));
    GDALClose(dataset);
}

} // namespace

class SpatialWhereGeometryTest : public GdbTutorialFixture {};

TEST_F(SpatialWhereGeometryTest, PolylineCrossingUsesExactIntersection) {
    const std::string path =
        (std::filesystem::temp_directory_path() /
         "fast_gdb_spatial_where_polyline.gdb").string();
    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer(
        "combined_lines", nullptr, wkbLineString, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn keep("keep", OFTInteger);
    ASSERT_EQ(layer->CreateField(&keep), OGRERR_NONE);

    OGRLineString crossing;
    crossing.addPoint(-5.0, 5.0);
    crossing.addPoint(15.0, 5.0);
    OGRLineString outside;
    outside.addPoint(20.0, 20.0);
    outside.addPoint(30.0, 30.0);
    OGRLineString filtered;
    filtered.addPoint(1.0, 1.0);
    filtered.addPoint(2.0, 2.0);
    const OGRGeometry* geometries[] = {&crossing, &outside, &filtered};
    const int keeps[] = {1, 1, 0};
    for (int index = 0; index < 3; ++index) {
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        feature->SetField("keep", keeps[index]);
        ASSERT_EQ(feature->SetGeometry(geometries[index]), OGRERR_NONE);
        ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);

    dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    ASSERT_TRUE(build_indexes(dataset, "combined_lines"));
    GDALClose(dataset);

    expect_fast_equals_gdal(
        path, "combined_lines", 0.0, 0.0, 10.0, 10.0, {0});
}

TEST_F(SpatialWhereGeometryTest, PolygonHoleDoesNotBecomeEnvelopeHit) {
    const std::string path =
        (std::filesystem::temp_directory_path() /
         "fast_gdb_spatial_where_polygon.gdb").string();
    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer(
        "combined_polygons", nullptr, wkbPolygon, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn keep("keep", OFTInteger);
    ASSERT_EQ(layer->CreateField(&keep), OGRERR_NONE);

    OGRLinearRing shell;
    shell.addPoint(0.0, 0.0);
    shell.addPoint(10.0, 0.0);
    shell.addPoint(10.0, 10.0);
    shell.addPoint(0.0, 10.0);
    shell.addPoint(0.0, 0.0);
    OGRLinearRing hole;
    hole.addPoint(3.0, 3.0);
    hole.addPoint(7.0, 3.0);
    hole.addPoint(7.0, 7.0);
    hole.addPoint(3.0, 7.0);
    hole.addPoint(3.0, 3.0);
    OGRPolygon polygon_with_hole;
    polygon_with_hole.addRing(&shell);
    polygon_with_hole.addRing(&hole);

    OGRLinearRing hit_ring;
    hit_ring.addPoint(4.0, 4.0);
    hit_ring.addPoint(6.0, 4.0);
    hit_ring.addPoint(6.0, 6.0);
    hit_ring.addPoint(4.0, 6.0);
    hit_ring.addPoint(4.0, 4.0);
    OGRPolygon polygon_in_hole;
    polygon_in_hole.addRing(&hit_ring);

    const OGRGeometry* geometries[] = {
        &polygon_with_hole, &polygon_in_hole};
    for (const OGRGeometry* geometry : geometries) {
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        feature->SetField("keep", 1);
        ASSERT_EQ(feature->SetGeometry(geometry), OGRERR_NONE);
        ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);

    dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    ASSERT_TRUE(build_indexes(dataset, "combined_polygons"));
    GDALClose(dataset);

    expect_fast_equals_gdal(
        path, "combined_polygons", 4.5, 4.5, 5.5, 5.5, {1});
}

TEST_F(SpatialWhereGeometryTest, MultiPointUsesAnyMemberIntersection) {
    const std::string path =
        (std::filesystem::temp_directory_path() /
         "fast_gdb_spatial_where_multipoint.gdb").string();
    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer(
        "combined_multipoints", nullptr, wkbMultiPoint, nullptr);
    ASSERT_NE(layer, nullptr);
    OGRFieldDefn keep("keep", OFTInteger);
    ASSERT_EQ(layer->CreateField(&keep), OGRERR_NONE);

    OGRMultiPoint hit;
    hit.addGeometryDirectly(new OGRPoint(50.0, 50.0));
    hit.addGeometryDirectly(new OGRPoint(5.0, 5.0));
    OGRMultiPoint miss;
    miss.addGeometryDirectly(new OGRPoint(20.0, 20.0));
    miss.addGeometryDirectly(new OGRPoint(30.0, 30.0));
    const OGRGeometry* geometries[] = {&hit, &miss};
    for (const OGRGeometry* geometry : geometries) {
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        feature->SetField("keep", 1);
        ASSERT_EQ(feature->SetGeometry(geometry), OGRERR_NONE);
        ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);

    dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    ASSERT_TRUE(build_indexes(dataset, "combined_multipoints"));
    GDALClose(dataset);

    expect_fast_equals_gdal(
        path, "combined_multipoints", 0.0, 0.0, 10.0, 10.0, {0});
}
