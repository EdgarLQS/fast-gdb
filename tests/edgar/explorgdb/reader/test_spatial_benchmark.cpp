#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
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
#include "../test_paths.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using explorgdb::CatalogResolver;
using explorgdb::GdbCatalog;
using explorgdb::QueryEngine;
using explorgdb::QueryResult;

namespace {

constexpr const char* kLayerName = "features";

struct BenchmarkCase {
    const char* name;
    double target_coverage;
    double xmin;
    double ymin;
    double xmax;
    double ymax;
};

BenchmarkCase centered_case(const char* name, double coverage) {
    constexpr double kExtentMin = 0.0;
    constexpr double kExtentMax = 100000.0;
    constexpr double kCenter = (kExtentMin + kExtentMax) / 2.0;
    const double side =
        std::sqrt(coverage) * (kExtentMax - kExtentMin);
    const double half = side / 2.0;
    return BenchmarkCase{
        name, coverage,
        kCenter - half, kCenter - half,
        kCenter + half, kCenter + half};
}

std::vector<BenchmarkCase> density_cases() {
    return {
        centered_case("coverage_01pct", 0.01),
        centered_case("coverage_10pct", 0.10),
        centered_case("coverage_30pct", 0.30),
        centered_case("coverage_80pct", 0.80),
        {"coverage_full", 1.0, -1000.0, -1000.0, 101000.0, 101000.0},
    };
}

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::string(value) == "1";
}

void set_env_value(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

std::unique_ptr<QueryEngine> open_fast_engine(
    const std::string& gdb_path,
    GdbCatalog& catalog) {
    if (!catalog.scan(gdb_path)) return nullptr;
    CatalogResolver resolver(catalog);
    if (!resolver.load()) return nullptr;
    const auto resolved = resolver.resolve(kLayerName);
    if (!resolved.has_value()) return nullptr;

    auto engine = std::make_unique<QueryEngine>(catalog, *resolved);
    if (!engine->open()) return nullptr;
    return engine;
}

struct TimedFastResult {
    QueryResult query;
    double wall_ms = 0.0;
};

TimedFastResult query_fast_gdb(
    QueryEngine& engine,
    const BenchmarkCase& benchmark) {
    const auto start = Clock::now();
    TimedFastResult timed;
    timed.query = engine.query_bbox_unified(
        benchmark.xmin, benchmark.ymin,
        benchmark.xmax, benchmark.ymax);
    timed.wall_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - start).count();
    return timed;
}

struct TimedGdalResult {
    std::vector<uint32_t> fids;
    double wall_ms = 0.0;
};

TimedGdalResult query_gdal(
    const std::string& gdb_path,
    const BenchmarkCase& benchmark) {
    TimedGdalResult timed;
    const auto start = Clock::now();

    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        gdb_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    if (dataset == nullptr) return timed;

    OGRLayer* layer = dataset->GetLayerByName(kLayerName);
    if (layer == nullptr) {
        GDALClose(dataset);
        return timed;
    }

    layer->SetSpatialFilterRect(
        benchmark.xmin, benchmark.ymin,
        benchmark.xmax, benchmark.ymax);
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        const GIntBig source_fid = feature->GetFID();
        if (source_fid >= 1) {
            timed.fids.push_back(
                static_cast<uint32_t>(source_fid - 1));
        }
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);

    std::sort(timed.fids.begin(), timed.fids.end());
    timed.fids.erase(
        std::unique(timed.fids.begin(), timed.fids.end()),
        timed.fids.end());
    timed.wall_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - start).count();
    return timed;
}

void print_header(const std::string& label, size_t feature_count) {
    std::printf("\n\n=== Spatial density benchmark: %s ===\n", label.c_str());
    std::printf("features=%zu layer=%s\n", feature_count, kLayerName);
    std::printf(
        "%-18s %9s %9s %-31s %10s %10s %10s %10s %10s %10s %10s\n",
        "case", "candidates", "ratio", "path", "fast_ms", "gdal_ms",
        "spx_ms", "scan_ms", "blob_ms", "bbox_ms", "exact_ms");
}

void print_row(const BenchmarkCase& benchmark,
               const TimedFastResult& fast,
               const TimedGdalResult& gdal) {
    const auto& metrics = fast.query.spatial_metrics;
    std::printf(
        "%-18s %9zu %8.2f%% %-31s %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f\n",
        benchmark.name,
        metrics.candidate_count,
        metrics.candidate_ratio * 100.0,
        fast.query.execution_path.c_str(),
        fast.wall_ms,
        gdal.wall_ms,
        metrics.candidate_lookup_ms,
        metrics.geometry_scan_ms,
        metrics.blob_lookup_ms,
        metrics.bbox_filter_ms,
        metrics.exact_filter_ms);
    std::printf(
        "  funnel: candidate=%zu rejected=%zu contained=%zu exact=%zu "
        "invalid=%zu result=%zu coverage_est=%.2f%% target=%.0f%% "
        "spx_bypassed=%s geometry_only=%s fast/gdal=%.3f\n",
        metrics.candidate_count,
        metrics.bbox_rejected,
        metrics.bbox_contained,
        metrics.exact_tested,
        metrics.invalid_geometries,
        fast.query.matched_fids.size(),
        metrics.estimated_coverage * 100.0,
        benchmark.target_coverage * 100.0,
        metrics.spx_bypassed ? "true" : "false",
        metrics.geometry_only_scan ? "true" : "false",
        gdal.wall_ms > 0.0 ? fast.wall_ms / gdal.wall_ms : 0.0);
}

void run_density_matrix(const fs::path& gdb_path,
                        const std::string& label) {
    ASSERT_TRUE(fs::is_directory(gdb_path))
        << "Benchmark data not found: " << gdb_path;

    GdbCatalog catalog;
    auto engine = open_fast_engine(gdb_path.string(), catalog);
    ASSERT_NE(engine, nullptr);
    ASSERT_NE(engine->table(), nullptr);

    engine->query_bbox_unified(-10.0, -10.0, -1.0, -1.0);

    print_header(label, engine->table()->active_feature_count());
    for (const auto& benchmark : density_cases()) {
        const TimedFastResult fast = query_fast_gdb(*engine, benchmark);
        const TimedGdalResult gdal = query_gdal(gdb_path.string(), benchmark);
        print_row(benchmark, fast, gdal);

        EXPECT_EQ(fast.query.matched_fids, gdal.fids)
            << "FID mismatch for " << benchmark.name;
        EXPECT_EQ(fast.query.spatial_metrics.invalid_geometries, 0u)
            << fast.query.fallback_reason;
        EXPECT_LE(fast.query.spatial_metrics.bbox_rejected,
                  fast.query.spatial_metrics.feature_count);
        EXPECT_LE(fast.query.spatial_metrics.bbox_contained,
                  fast.query.spatial_metrics.feature_count);
        EXPECT_LE(fast.query.spatial_metrics.exact_tested,
                  fast.query.spatial_metrics.feature_count);
    }
}

class SpatialDensityBenchmark : public ::testing::Test {
protected:
    void SetUp() override {
        GDALAllRegister();
        if (!env_enabled("FAST_GDB_RUN_SPATIAL_BENCHMARKS")) {
            GTEST_SKIP()
                << "Set FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 to run benchmarks";
        }
        set_env_value("FAST_GDB_SPATIAL_PROFILE", "1");
    }
};

} // namespace

TEST_F(SpatialDensityBenchmark, DensityMatrix1M) {
    const fs::path path = explorgdb_test_paths::test_data_path(
        "test_data/large/large_test.gdb");
    if (!fs::is_directory(path)) {
        GTEST_SKIP() << "1M benchmark data not found: " << path;
    }
    run_density_matrix(path, "1M generated polygons");
}

TEST_F(SpatialDensityBenchmark, DensityMatrix10M) {
    if (!env_enabled("FAST_GDB_RUN_10M_BENCHMARKS")) {
        GTEST_SKIP()
            << "Set FAST_GDB_RUN_10M_BENCHMARKS=1 for the 10M matrix";
    }
    const fs::path path = explorgdb_test_paths::test_data_path(
        "test_data/large_10m/large_10m_test.gdb");
    if (!fs::is_directory(path)) {
        GTEST_SKIP() << "10M benchmark data not found: " << path;
    }
    run_density_matrix(path, "10M generated polygons");
}
