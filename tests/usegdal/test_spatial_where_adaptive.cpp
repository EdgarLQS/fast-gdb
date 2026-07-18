#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "cpl_error.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"
#include "spatial_where_test_utils.h"
#include "test_fixture.h"

using namespace explorgdb;

namespace {

constexpr const char* kLayer = "adaptive_points";
constexpr int kFeatureCount = 100;

bool execute_sql(GDALDataset* dataset, const std::string& sql) {
    CPLErrorReset();
    OGRLayer* result = dataset->ExecuteSQL(sql.c_str(), nullptr, nullptr);
    if (result != nullptr) dataset->ReleaseResultSet(result);
    return CPLGetLastErrorType() < CE_Failure;
}

bool rewrite_features(OGRLayer* layer) {
    if (layer == nullptr) return false;
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        const OGRErr error = layer->SetFeature(feature);
        OGRFeature::DestroyFeature(feature);
        if (error != OGRERR_NONE) return false;
    }
    return true;
}

std::vector<uint32_t> collect_gdal(
    const std::string& path,
    double xmin, double ymin, double xmax, double ymax,
    const char* where) {
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    if (dataset == nullptr) return {};
    OGRLayer* layer = dataset->GetLayerByName(kLayer);
    if (layer == nullptr) {
        GDALClose(dataset);
        return {};
    }

    layer->SetSpatialFilterRect(xmin, ymin, xmax, ymax);
    if (layer->SetAttributeFilter(where) != OGRERR_NONE) {
        GDALClose(dataset);
        return {};
    }

    std::vector<uint32_t> fids;
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        if (feature->GetFID() > 0) {
            fids.push_back(static_cast<uint32_t>(feature->GetFID() - 1));
        }
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);
    std::sort(fids.begin(), fids.end());
    fids.erase(std::unique(fids.begin(), fids.end()), fids.end());
    return fids;
}

} // namespace

class SpatialWhereAdaptiveTest : public GdbTutorialFixture {
protected:
    std::string create_fixture(const std::string& suffix) {
        const std::string path =
            spatial_where_test_utils::fixture_path(suffix).string();
        GDALDataset* dataset = createGdb(path.c_str());
        if (dataset == nullptr) return {};
        OGRLayer* layer = dataset->CreateLayer(
            kLayer, nullptr, wkbPoint, nullptr);
        if (layer == nullptr) {
            GDALClose(dataset);
            return {};
        }

        OGRFieldDefn value_field("value", OFTInteger);
        if (layer->CreateField(&value_field) != OGRERR_NONE) {
            GDALClose(dataset);
            return {};
        }

        const bool transaction_started =
            layer->StartTransaction() == OGRERR_NONE;
        for (int fid = 0; fid < kFeatureCount; ++fid) {
            OGRFeature* feature = OGRFeature::CreateFeature(
                layer->GetLayerDefn());
            if (feature == nullptr) {
                GDALClose(dataset);
                return {};
            }
            feature->SetField("value", fid);
            OGRPoint point(static_cast<double>(fid), 0.0);
            const OGRErr geometry_error = feature->SetGeometry(&point);
            const OGRErr create_error = geometry_error == OGRERR_NONE
                ? layer->CreateFeature(feature)
                : geometry_error;
            OGRFeature::DestroyFeature(feature);
            if (create_error != OGRERR_NONE) {
                GDALClose(dataset);
                return {};
            }
        }
        if (transaction_started &&
            layer->CommitTransaction() != OGRERR_NONE) {
            GDALClose(dataset);
            return {};
        }
        GDALClose(dataset);

        dataset = static_cast<GDALDataset*>(GDALOpenEx(
            path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
            nullptr, nullptr, nullptr));
        if (dataset == nullptr) return {};
        layer = dataset->GetLayerByName(kLayer);
        const bool prepared = layer != nullptr &&
            rewrite_features(layer) &&
            execute_sql(dataset, std::string("RECOMPUTE EXTENT ON ") + kLayer) &&
            execute_sql(dataset, std::string("CREATE INDEX value_idx ON ") +
                                 kLayer + "(value)");
        GDALClose(dataset);
        return prepared ? path : std::string{};
    }

    QueryResult run_query(const std::string& path,
                          double xmin, double xmax,
                          const char* where) {
        GdbCatalog catalog;
        EXPECT_TRUE(catalog.scan(path));
        CatalogResolver resolver(catalog);
        EXPECT_TRUE(resolver.load());
        const auto resolved = resolver.resolve(kLayer);
        EXPECT_TRUE(resolved.has_value());
        if (!resolved.has_value()) return {};

        QueryEngine engine(catalog, *resolved);
        EXPECT_TRUE(engine.open());
        QueryRequest request;
        request.kind = QueryKind::SpatialWhere;
        request.xmin = xmin;
        request.ymin = -1.0;
        request.xmax = xmax;
        request.ymax = 1.0;
        request.where_clause = where;
        return engine.query(request);
    }
};

TEST_F(SpatialWhereAdaptiveTest,
       SelectiveSpatialCandidatesUseFusedSingleRowScan) {
    const std::string path = create_fixture(
        "fast_gdb_spatial_where_adaptive_bypass");
    ASSERT_FALSE(path.empty());

    const QueryResult result = run_query(path, 0.0, 9.0, "value >= 5");
    EXPECT_EQ(result.execution_path, "spatial-where:spatial-candidates");
    EXPECT_TRUE(result.combined_metrics.used_spatial_index);
    EXPECT_FALSE(result.combined_metrics.used_attribute_index);
    EXPECT_TRUE(result.combined_metrics.attribute_index_bypassed);
    EXPECT_TRUE(result.combined_metrics.fused_spatial_attribute_scan);
    EXPECT_GE(result.combined_metrics.fused_candidate_count, 10U);
    EXPECT_LE(result.combined_metrics.fused_candidate_count, 12U);
    EXPECT_EQ(result.combined_metrics.spatial_candidate_count,
              result.combined_metrics.fused_candidate_count);
    EXPECT_EQ(result.combined_metrics.spatial_match_count, 10U);
    EXPECT_EQ(result.combined_metrics.attribute_tested, 10U);
    EXPECT_GE(result.combined_metrics.fused_candidate_scan_ms, 0.0);
    EXPECT_EQ(result.matched_fids,
              (std::vector<uint32_t>{5, 6, 7, 8, 9}));
    EXPECT_EQ(result.matched_fids,
              collect_gdal(path, 0.0, -1.0, 9.0, 1.0, "value >= 5"));
}

TEST_F(SpatialWhereAdaptiveTest,
       HighCoverageUsesDirectAttributeIndexAndReportsMetrics) {
    const std::string path = create_fixture(
        "fast_gdb_spatial_where_adaptive_atx");
    ASSERT_FALSE(path.empty());

    const QueryResult result = run_query(path, 0.0, 99.0, "value >= 90");
    EXPECT_EQ(result.execution_path, "spatial-where:spx+atx");
    EXPECT_TRUE(result.combined_metrics.used_spatial_index);
    EXPECT_TRUE(result.combined_metrics.used_attribute_index);
    EXPECT_FALSE(result.combined_metrics.attribute_index_bypassed);
    EXPECT_FALSE(result.combined_metrics.fused_spatial_attribute_scan);
    EXPECT_EQ(result.combined_metrics.fused_candidate_count, 0U);
    EXPECT_EQ(result.combined_metrics.attribute_candidate_count, 10U);
    EXPECT_EQ(result.combined_metrics.attribute_tested, 10U);
    EXPECT_EQ(result.combined_metrics.attribute_index_entries_scanned, 100U);
    EXPECT_GT(result.combined_metrics.attribute_index_page_count, 0U);
    EXPECT_GT(result.combined_metrics.attribute_index_pages_visited, 0U);
    EXPECT_EQ(result.matched_fids,
              (std::vector<uint32_t>{90, 91, 92, 93, 94,
                                     95, 96, 97, 98, 99}));
    EXPECT_EQ(result.matched_fids,
              collect_gdal(path, 0.0, -1.0, 99.0, 1.0, "value >= 90"));
}
