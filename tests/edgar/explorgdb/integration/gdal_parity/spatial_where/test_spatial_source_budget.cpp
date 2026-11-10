// End-to-end source-proof candidate budget: a large layer must build a
// complete proof, exceed the candidate budget, and fall back to a full scan
// without returning partial results.
//
// Gated by FAST_GDB_RUN_BUDGET_TESTS=1 and FAST_GDB_BENCHMARK_PATH pointing
// to a layer with at least ~8.4M uniform rows (the 10M fixture used by the
// scale benchmarks). Optional FAST_GDB_BENCHMARK_LAYER (default "features").

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"

using namespace explorgdb;
namespace fs = std::filesystem;

namespace {

constexpr const char* kPathEnv = "FAST_GDB_BENCHMARK_PATH";
constexpr const char* kLayerEnv = "FAST_GDB_BENCHMARK_LAYER";

bool budget_enabled() {
    const char* value = std::getenv("FAST_GDB_RUN_BUDGET_TESTS");
    return value != nullptr && std::string(value) == "1";
}

struct FastState {
    std::unique_ptr<GdbCatalog> catalog;
    std::unique_ptr<QueryEngine> engine;
};

std::unique_ptr<FastState> open_fast(const std::string& path,
                                     const std::string& layer) {
    auto state = std::make_unique<FastState>();
    state->catalog = std::make_unique<GdbCatalog>();
    if (!state->catalog->scan(path)) return nullptr;
    CatalogResolver resolver(*state->catalog);
    if (!resolver.load()) return nullptr;
    const auto resolved = resolver.resolve(layer);
    if (!resolved) return nullptr;
    state->engine = std::make_unique<QueryEngine>(*state->catalog, *resolved);
    if (!state->engine->open()) return nullptr;
    return state;
}

std::vector<RowSlot> gdal_fids(const std::string& path, const std::string& layer,
                               double xmin, double ymin, double xmax,
                               double ymax) {
    std::vector<RowSlot> fids;
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
        nullptr));
    if (dataset == nullptr) return fids;
    OGRLayer* source = dataset->GetLayerByName(layer.c_str());
    if (source != nullptr) {
        source->SetSpatialFilterRect(xmin, ymin, xmax, ymax);
        source->ResetReading();
        while (OGRFeature* feature = source->GetNextFeature()) {
            const GIntBig fid = feature->GetFID();
            if (fid >= 1) fids.push_back(static_cast<RowSlot>(fid - 1));
            OGRFeature::DestroyFeature(feature);
        }
        source->SetSpatialFilter(nullptr);
    }
    GDALClose(dataset);
    std::sort(fids.begin(), fids.end());
    fids.erase(std::unique(fids.begin(), fids.end()), fids.end());
    return fids;
}

void expect_same_fids(const std::vector<RowSlot>& fast,
                      const std::vector<RowSlot>& gdal,
                      const char* label) {
    ASSERT_EQ(fast.size(), gdal.size()) << label;
    for (size_t i = 0; i < fast.size(); ++i)
        ASSERT_EQ(fast[i], gdal[i]) << label << " index=" << i;
}

struct Window {
    double xmin, ymin, xmax, ymax;
};

Window centered_window(const FieldDescriptor& geometry, double coverage) {
    const double width = geometry.xmax - geometry.xmin;
    const double height = geometry.ymax - geometry.ymin;
    const double side = std::sqrt(coverage);
    const double half_x = width * side / 2.0;
    const double half_y = height * side / 2.0;
    const double cx = geometry.xmin + width / 2.0;
    const double cy = geometry.ymin + height / 2.0;
    return Window{cx - half_x, cy - half_y, cx + half_x, cy + half_y};
}

} // namespace

class SpatialSourceBudgetTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!budget_enabled())
            GTEST_SKIP() << "Set FAST_GDB_RUN_BUDGET_TESTS=1";
        const char* path = std::getenv(kPathEnv);
        if (path == nullptr || path[0] == '\0' || !fs::is_directory(path))
            GTEST_SKIP() << "Set " << kPathEnv
                         << " to the 10M uniform benchmark FileGDB";
        GDALAllRegister();
    }

    std::string path() const { return std::getenv(kPathEnv); }
    std::string layer() const {
        const char* value = std::getenv(kLayerEnv);
        return value == nullptr || value[0] == '\0' ? "features"
                                                     : std::string(value);
    }
};

TEST_F(SpatialSourceBudgetTest, OverBudgetQueryBuildsProofThenFallsBack) {
    auto state = open_fast(path(), layer());
    ASSERT_NE(state, nullptr) << path() << "/" << layer();
    const FieldDescriptor* geometry = nullptr;
    for (const auto& field : state->engine->table()->fields())
        if (field.type == FieldType::Geometry) { geometry = &field; break; }
    ASSERT_NE(geometry, nullptr) << "layer has no geometry field";
    const size_t feature_count =
        state->engine->table()->active_feature_count();
    ASSERT_GE(feature_count, 8'400'000U)
        << "The candidate budget test requires a large uniform fixture";

    const Window budget_window = centered_window(*geometry, 0.25);
    const QueryResult cold = state->engine->query_bbox_unified(
        budget_window.xmin, budget_window.ymin,
        budget_window.xmax, budget_window.ymax);
    ASSERT_EQ(cold.status, QueryStatus::Ok) << cold.error;
    ASSERT_LT(cold.spatial_metrics.estimated_coverage, 0.29)
        << "window must stay below the direct-scan planner threshold";
    EXPECT_EQ(cold.execution_path, "bbox:model:sequential-fallback");
    EXPECT_NE(cold.fallback_reason.find("candidate budget exceeded"),
              std::string::npos)
        << cold.fallback_reason;
    EXPECT_FALSE(cold.spatial_metrics.source_directory_hit);
    EXPECT_GT(cold.spatial_metrics.source_directory_build_ms, 0.0);
    EXPECT_GT(cold.spatial_metrics.source_directory_bytes, 0U);
    EXPECT_GT(cold.spatial_metrics.source_directory_leaves, 0U);
    EXPECT_GT(cold.spatial_metrics.source_directory_entries, 0U);
    EXPECT_GT(cold.spatial_metrics.source_index_pages_read, 0U);
    EXPECT_EQ(cold.spatial_metrics.candidate_count, feature_count);
    expect_same_fids(cold.matched_fids,
        gdal_fids(path(), layer(), budget_window.xmin, budget_window.ymin,
                  budget_window.xmax, budget_window.ymax),
        "budget-window");

    const QueryResult warm = state->engine->query_bbox_unified(
        budget_window.xmin, budget_window.ymin,
        budget_window.xmax, budget_window.ymax);
    ASSERT_EQ(warm.status, QueryStatus::Ok) << warm.error;
    EXPECT_TRUE(warm.spatial_metrics.source_directory_hit);
    EXPECT_EQ(warm.execution_path, "bbox:model:sequential-fallback");
    EXPECT_NE(warm.fallback_reason.find("candidate budget exceeded"),
              std::string::npos)
        << warm.fallback_reason;
    EXPECT_EQ(warm.spatial_metrics.source_directory_build_ms, 0.0);
    EXPECT_EQ(warm.matched_fids, cold.matched_fids);
    std::printf(
        "[budget] features=%zu coverage=%.2f%% cold_path=%s cold_candidates=%zu "
        "build_ms=%.1f dir_bytes=%zu leaves=%zu entries=%zu pages=%zu "
        "matched=%zu warm_hit=%s warm_path=%s reason=%s\n",
        feature_count, cold.spatial_metrics.estimated_coverage * 100.0,
        cold.execution_path.c_str(), cold.spatial_metrics.candidate_count,
        cold.spatial_metrics.source_directory_build_ms,
        cold.spatial_metrics.source_directory_bytes,
        cold.spatial_metrics.source_directory_leaves,
        cold.spatial_metrics.source_directory_entries,
        cold.spatial_metrics.source_index_pages_read,
        cold.matched_fids.size(),
        warm.spatial_metrics.source_directory_hit ? "true" : "false",
        warm.execution_path.c_str(), warm.fallback_reason.c_str());
}

TEST_F(SpatialSourceBudgetTest, SelectiveQueryUsesProvenCandidates) {
    auto state = open_fast(path(), layer());
    ASSERT_NE(state, nullptr) << path() << "/" << layer();
    const FieldDescriptor* geometry = nullptr;
    for (const auto& field : state->engine->table()->fields())
        if (field.type == FieldType::Geometry) { geometry = &field; break; }
    ASSERT_NE(geometry, nullptr) << "layer has no geometry field";
    const size_t feature_count =
        state->engine->table()->active_feature_count();

    const Window window = centered_window(*geometry, 0.001);
    const QueryResult result = state->engine->query_bbox_unified(
        window.xmin, window.ymin, window.xmax, window.ymax);
    ASSERT_EQ(result.status, QueryStatus::Ok) << result.error;
    EXPECT_TRUE(result.execution_path == "bbox:model:spx-candidates" ||
                result.execution_path == "bbox:model:spx-candidates-batched")
        << result.execution_path << " / " << result.fallback_reason;
    EXPECT_GT(result.spatial_metrics.candidate_count, 0U);
    EXPECT_LT(result.spatial_metrics.candidate_count, feature_count);
    EXPECT_GT(result.spatial_metrics.source_index_pages_read, 0U);
    expect_same_fids(result.matched_fids,
        gdal_fids(path(), layer(), window.xmin, window.ymin,
                  window.xmax, window.ymax),
        "selective-window");
    std::printf(
        "[selective] features=%zu coverage=%.3f%% path=%s candidates=%zu "
        "ratio=%.3f%% dir_bytes=%zu pages=%zu matched=%zu reason=%s\n",
        feature_count, result.spatial_metrics.estimated_coverage * 100.0,
        result.execution_path.c_str(),
        result.spatial_metrics.candidate_count,
        result.spatial_metrics.candidate_ratio * 100.0,
        result.spatial_metrics.source_directory_bytes,
        result.spatial_metrics.source_index_pages_read,
        result.matched_fids.size(), result.fallback_reason.c_str());
}
