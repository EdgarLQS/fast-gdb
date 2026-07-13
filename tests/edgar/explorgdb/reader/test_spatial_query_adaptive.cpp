#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"
#include "test_fixture.h"

using namespace explorgdb;

namespace {

class SpatialQueryAdaptiveTest : public GdbTutorialFixture {
protected:
    std::string create_adaptive_point_gdb(size_t feature_count) {
        const auto path = (std::filesystem::temp_directory_path() /
                           "fast_gdb_spatial_adaptive.gdb").string();
        std::error_code error;
        std::filesystem::remove_all(path, error);

        GDALDataset* dataset = createGdb(path.c_str());
        if (dataset == nullptr) return {};

        OGRSpatialReference srs;
        if (srs.importFromEPSG(4326) != OGRERR_NONE) {
            GDALClose(dataset);
            return {};
        }
        OGRLayer* layer = dataset->CreateLayer(
            "adaptive_points", &srs, wkbPoint, nullptr);
        if (layer == nullptr) {
            GDALClose(dataset);
            return {};
        }

        OGRFieldDefn value_field("value", OFTInteger);
        if (layer->CreateField(&value_field) != OGRERR_NONE) {
            GDALClose(dataset);
            return {};
        }

        for (size_t index = 0; index < feature_count; ++index) {
            OGRFeature* feature =
                OGRFeature::CreateFeature(layer->GetLayerDefn());
            if (feature == nullptr) {
                GDALClose(dataset);
                return {};
            }
            feature->SetField("value", static_cast<int>(index));
            OGRPoint point(
                static_cast<double>(index % 100),
                static_cast<double>(index / 100));
            feature->SetGeometry(&point);
            const OGRErr create_error = layer->CreateFeature(feature);
            OGRFeature::DestroyFeature(feature);
            if (create_error != OGRERR_NONE) {
                GDALClose(dataset);
                return {};
            }
        }

        GDALClose(dataset);
        return path;
    }
};

std::unique_ptr<QueryEngine> open_engine(
    const std::string& path,
    GdbCatalog& catalog) {
    if (!catalog.scan(path)) return nullptr;
    CatalogResolver resolver(catalog);
    if (!resolver.load()) return nullptr;
    const auto resolved = resolver.resolve("adaptive_points");
    if (!resolved.has_value()) return nullptr;

    auto engine = std::make_unique<QueryEngine>(catalog, *resolved);
    if (!engine->open()) return nullptr;
    return engine;
}

} // namespace

TEST_F(SpatialQueryAdaptiveTest,
       GeometryBlobViewsRemainStableAcrossCandidateLookups) {
    const std::string path = create_adaptive_point_gdb(8);
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    auto engine = open_engine(path, catalog);
    ASSERT_NE(engine, nullptr);

    const uint8_t* first_blob = nullptr;
    size_t first_size = 0;
    ASSERT_TRUE(engine->peek_bbox_source(0, first_blob, first_size));
    ASSERT_NE(first_blob, nullptr);
    ASSERT_GT(first_size, 0u);

    std::array<uint8_t, 16> snapshot{};
    const size_t snapshot_size = std::min(snapshot.size(), first_size);
    std::memcpy(snapshot.data(), first_blob, snapshot_size);

    const uint8_t* second_blob = nullptr;
    size_t second_size = 0;
    ASSERT_TRUE(engine->peek_bbox_source(1, second_blob, second_size));
    ASSERT_NE(second_blob, nullptr);
    EXPECT_NE(first_blob, second_blob);
    EXPECT_EQ(std::memcmp(first_blob, snapshot.data(), snapshot_size), 0);
}

TEST_F(SpatialQueryAdaptiveTest,
       HighDensityQueryUsesAdaptivePathAndPreservesFids) {
    constexpr size_t kFeatureCount = 1200;
    const std::string path = create_adaptive_point_gdb(kFeatureCount);
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    auto engine = open_engine(path, catalog);
    ASSERT_NE(engine, nullptr);

    QueryRequest request;
    request.kind = QueryKind::SpatialBbox;
    request.xmin = -1.0;
    request.ymin = -1.0;
    request.xmax = 100.0;
    request.ymax = 20.0;

    const QueryResult result = engine->query(request);
    ASSERT_EQ(result.matched_fids.size(), kFeatureCount);
    EXPECT_TRUE(std::is_sorted(
        result.matched_fids.begin(), result.matched_fids.end()));
    EXPECT_EQ(result.matched_fids.front(), 0u);
    EXPECT_EQ(result.matched_fids.back(), kFeatureCount - 1);
    EXPECT_EQ(result.spatial_metrics.feature_count, kFeatureCount);
    EXPECT_GE(result.spatial_metrics.candidate_count,
              result.matched_fids.size());
    EXPECT_GT(result.spatial_metrics.candidate_ratio, 0.5);
    EXPECT_EQ(result.spatial_metrics.invalid_geometries, 0u);
    EXPECT_EQ(result.execution_path, "bbox:model:sequential-adaptive");

    const auto legacy = engine->query_bbox(
        request.xmin, request.ymin, request.xmax, request.ymax);
    EXPECT_EQ(legacy, result.matched_fids);
}

TEST_F(SpatialQueryAdaptiveTest,
       LowDensityQueryRetainsSpatialIndexCandidatePath) {
    constexpr size_t kFeatureCount = 1200;
    const std::string path = create_adaptive_point_gdb(kFeatureCount);
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    auto engine = open_engine(path, catalog);
    ASSERT_NE(engine, nullptr);

    const QueryResult result = engine->query_bbox_unified(
        -0.1, -0.1, 1.1, 1.1);
    ASSERT_FALSE(result.matched_fids.empty());
    EXPECT_LT(result.matched_fids.size(), kFeatureCount / 10);
    EXPECT_LT(result.spatial_metrics.candidate_ratio, 0.5);
    EXPECT_EQ(result.execution_path, "bbox:model:spx-candidates");
    EXPECT_EQ(result.spatial_metrics.invalid_geometries, 0u);
}
