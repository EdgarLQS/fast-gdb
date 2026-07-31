// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
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

constexpr int kFeatureCount = 100;

enum class GeometryCase {
    MultiPoint,
    Polyline,
    Polygon
};

bool execute_sql(GDALDataset* dataset, const std::string& sql) {
    CPLErrorReset();
    OGRLayer* result = dataset->ExecuteSQL(sql.c_str(), nullptr, nullptr);
    if (result != nullptr) dataset->ReleaseResultSet(result);
    return CPLGetLastErrorType() < CE_Failure;
}

OGRwkbGeometryType geometry_type(GeometryCase geometry_case) {
    switch (geometry_case) {
        case GeometryCase::MultiPoint: return wkbMultiPoint;
        case GeometryCase::Polyline: return wkbLineString;
        case GeometryCase::Polygon: return wkbPolygon;
    }
    return wkbUnknown;
}

std::unique_ptr<OGRGeometry> make_geometry(
    GeometryCase geometry_case,
    double x) {
    switch (geometry_case) {
        case GeometryCase::MultiPoint: {
            auto geometry = std::make_unique<OGRMultiPoint>();
            geometry->addGeometryDirectly(new OGRPoint(x, 0.0));
            geometry->addGeometryDirectly(new OGRPoint(x + 0.25, 0.25));
            return geometry;
        }
        case GeometryCase::Polyline: {
            auto geometry = std::make_unique<OGRLineString>();
            geometry->addPoint(x, 0.0);
            geometry->addPoint(x + 0.4, 0.4);
            return geometry;
        }
        case GeometryCase::Polygon: {
            OGRLinearRing ring;
            ring.addPoint(x, 0.0);
            ring.addPoint(x + 0.4, 0.0);
            ring.addPoint(x + 0.4, 0.4);
            ring.addPoint(x, 0.4);
            ring.addPoint(x, 0.0);
            auto geometry = std::make_unique<OGRPolygon>();
            geometry->addRing(&ring);
            return geometry;
        }
    }
    return nullptr;
}

std::vector<uint32_t> collect_gdal(
    const std::string& path,
    const std::string& layer_name) {
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    if (dataset == nullptr) return {};
    OGRLayer* layer = dataset->GetLayerByName(layer_name.c_str());
    if (layer == nullptr) {
        GDALClose(dataset);
        return {};
    }

    layer->SetSpatialFilterRect(0.0, -1.0, 9.5, 1.0);
    if (layer->SetAttributeFilter("keep = 1") != OGRERR_NONE) {
        GDALClose(dataset);
        return {};
    }

    std::vector<uint32_t> fids;
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        if (feature->GetFID() > 0)
            fids.push_back(static_cast<uint32_t>(feature->GetFID() - 1));
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);
    std::sort(fids.begin(), fids.end());
    fids.erase(std::unique(fids.begin(), fids.end()), fids.end());
    return fids;
}

} // namespace

class SpatialWhereFusedGeometryTest : public GdbTutorialFixture {
protected:
    void run_case(GeometryCase geometry_case,
                  const std::string& suffix,
                  const std::string& layer_name) {
        const std::string path =
            (std::filesystem::temp_directory_path() / suffix).string();
        GDALDataset* dataset = createGdb(path.c_str());
        ASSERT_NE(dataset, nullptr);
        OGRLayer* layer = dataset->CreateLayer(
            layer_name.c_str(), nullptr, geometry_type(geometry_case), nullptr);
        ASSERT_NE(layer, nullptr);
        OGRFieldDefn keep_field("keep", OFTInteger);
        ASSERT_EQ(layer->CreateField(&keep_field), OGRERR_NONE);

        const bool transaction_started =
            layer->StartTransaction() == OGRERR_NONE;
        for (int fid = 0; fid < kFeatureCount; ++fid) {
            std::unique_ptr<OGRGeometry> geometry = make_geometry(
                geometry_case, static_cast<double>(fid));
            ASSERT_NE(geometry, nullptr);
            OGRFeature* feature = OGRFeature::CreateFeature(
                layer->GetLayerDefn());
            ASSERT_NE(feature, nullptr);
            feature->SetField("keep", fid % 2);
            ASSERT_EQ(feature->SetGeometry(geometry.get()), OGRERR_NONE);
            ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
            OGRFeature::DestroyFeature(feature);
        }
        if (transaction_started)
            ASSERT_EQ(layer->CommitTransaction(), OGRERR_NONE);
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
            dataset, "CREATE INDEX keep_idx ON " + layer_name + "(keep)"));
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
        request.ymin = -1.0;
        request.xmax = 9.5;
        request.ymax = 1.0;
        request.where_clause = "keep = 1";
        const QueryResult result = engine.query(request);

        EXPECT_EQ(result.execution_path, "spatial-where:spatial-candidates");
        EXPECT_TRUE(result.combined_metrics.used_spatial_index);
        EXPECT_TRUE(result.combined_metrics.attribute_index_bypassed);
        EXPECT_TRUE(result.combined_metrics.fused_spatial_attribute_scan);
        EXPECT_LE(result.combined_metrics.fused_candidate_count, 12U);
        EXPECT_EQ(result.combined_metrics.spatial_match_count, 10U);
        EXPECT_EQ(result.combined_metrics.attribute_tested, 10U);
        EXPECT_EQ(result.matched_fids,
                  (std::vector<uint32_t>{1, 3, 5, 7, 9}));
        EXPECT_EQ(result.matched_fids, collect_gdal(path, layer_name));
    }
};

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereFusedGeometryTest, MultiPointMatchesGdal) {
    run_case(GeometryCase::MultiPoint,
             "fast_gdb_fused_multipoint.gdb",
             "fused_multipoints");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereFusedGeometryTest, PolylineMatchesGdal) {
    run_case(GeometryCase::Polyline,
             "fast_gdb_fused_polyline.gdb",
             "fused_lines");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialWhereFusedGeometryTest, PolygonMatchesGdal) {
    run_case(GeometryCase::Polygon,
             "fast_gdb_fused_polygon.gdb",
             "fused_polygons");
}
