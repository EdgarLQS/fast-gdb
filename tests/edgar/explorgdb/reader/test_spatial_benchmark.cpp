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

// ============================================================================
// Benchmark case definitions
// ============================================================================

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

// ============================================================================
// Environment helpers
// ============================================================================

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::string(value) == "1";
}

constexpr int kDefaultNumTrials = 5;
constexpr int kMaximumNumTrials = 100;

int benchmark_trials() {
    const char* value = std::getenv("FAST_GDB_BENCHMARK_TRIALS");
    if (value == nullptr || *value == '\0') return kDefaultNumTrials;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 1 ||
        parsed > kMaximumNumTrials) {
        return kDefaultNumTrials;
    }
    return static_cast<int>(parsed);
}

// ============================================================================
// Benchmark mode selection
// ============================================================================

enum class BenchmarkMode {
    SteadyState,  // default: open once, warm up, only time queries
    FreshOpen,    // each run: open, query, close, time everything
};

BenchmarkMode get_benchmark_mode() {
    const char* mode = std::getenv("FAST_GDB_BENCHMARK_MODE");
    if (mode && std::string(mode) == "fresh-open") {
        return BenchmarkMode::FreshOpen;
    }
    return BenchmarkMode::SteadyState;
}

// ============================================================================
// Query helpers
// ============================================================================

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

struct TimedQueryResult {
    QueryResult query;
    double wall_ms = 0.0;
};

TimedQueryResult query_fast_gdb(
    QueryEngine& engine,
    const BenchmarkCase& benchmark) {
    const auto start = Clock::now();
    TimedQueryResult timed;
    timed.query = engine.query_bbox_unified(
        benchmark.xmin, benchmark.ymin,
        benchmark.xmax, benchmark.ymax);
    timed.wall_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - start).count();
    return timed;
}

struct GdalTimedResult {
    std::vector<uint32_t> fids;
    double wall_ms = 0.0;
};

GdalTimedResult query_gdal_steady_state(
    GDALDataset* dataset,
    const BenchmarkCase& benchmark) {
    GdalTimedResult timed;
    OGRLayer* layer = dataset->GetLayerByName(kLayerName);
    if (layer == nullptr) return timed;

    const auto start = Clock::now();
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
    timed.wall_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - start).count();

    // Sort and deduplicate
    std::sort(timed.fids.begin(), timed.fids.end());
    timed.fids.erase(
        std::unique(timed.fids.begin(), timed.fids.end()),
        timed.fids.end());
    return timed;
}

TimedQueryResult query_fast_gdb_fresh_open(
    const std::string& gdb_path,
    const BenchmarkCase& benchmark) {
    TimedQueryResult timed;
    const auto start = Clock::now();

    // Open, query, and close (via RAII) all inside the timer,
    // symmetric with query_gdal_fresh_open
    {
        GdbCatalog catalog;
        auto engine = open_fast_engine(gdb_path, catalog);
        if (engine) {
            timed.query = engine->query_bbox_unified(
                benchmark.xmin, benchmark.ymin,
                benchmark.xmax, benchmark.ymax);
        }
    }

    timed.wall_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - start).count();
    return timed;
}

GdalTimedResult query_gdal_fresh_open(
    const std::string& gdb_path,
    const BenchmarkCase& benchmark) {
    GdalTimedResult timed;
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

    timed.wall_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - start).count();

    std::sort(timed.fids.begin(), timed.fids.end());
    timed.fids.erase(
        std::unique(timed.fids.begin(), timed.fids.end()),
        timed.fids.end());
    return timed;
}

// ============================================================================
// Statistics helpers
// ============================================================================

double median(std::vector<double>& values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    if (n % 2 == 0) {
        return (values[n / 2 - 1] + values[n / 2]) / 2.0;
    }
    return values[n / 2];
}

double p95(std::vector<double>& values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t idx = static_cast<size_t>(
        std::ceil(values.size() * 0.95) - 1);
    return values[std::min(idx, values.size() - 1)];
}

// ============================================================================
// Per-case result collector
// ============================================================================

struct CaseRunResults {
    std::vector<double> fast_ms_vals;
    std::vector<double> gdal_ms_vals;
    std::vector<QueryResult> fast_results;
    std::vector<GdalTimedResult> gdal_results;
};

constexpr double kPerformanceRatioLimit = 0.90;
constexpr double kSmallQueryToleranceMs = 200.0;

// ============================================================================
// Printing
// ============================================================================

void print_header(const std::string& label, size_t feature_count,
                  const char* mode_str, bool profile_enabled) {
    std::printf("\n\n=== Spatial density benchmark: %s (%s) ===\n",
                label.c_str(), mode_str);
    std::printf("features=%zu layer=%s\n", feature_count, kLayerName);
    if (profile_enabled) {
        std::printf("profile enabled: diagnostic timings; performance gates disabled\n");
    }
    std::printf("%-18s %9s %9s %-31s %8s %8s %8s %8s %8s %8s %8s\n",
                "case", "candidates", "ratio", "path",
                "fast_ms", "gdal_ms", "spx_ms", "scan_ms",
                "blob_ms", "bbox_ms", "exact_ms");
}

void print_row(const BenchmarkCase& benchmark,
               const TimedQueryResult& fast,
               const GdalTimedResult& gdal) {
    const auto& metrics = fast.query.spatial_metrics;
    std::printf(
        "%-18s %9zu %8.2f%% %-31s %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f\n",
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

void print_summary_row(const char* name,
                       double fast_median_ms, double gdal_median_ms,
                       double fast_p95_ms, double gdal_p95_ms,
                       double ratio) {
    std::printf(
        "%-18s %10.1f %10.1f %10.1f %10.1f %10.3f\n",
        name, fast_median_ms, gdal_median_ms,
        fast_p95_ms, gdal_p95_ms, ratio);
}

void print_summary_header(const char* mode_str) {
    std::printf("\n--- %s Summary (median of %d) ---\n",
                mode_str, benchmark_trials());
    std::printf("%-18s %10s %10s %10s %10s %10s\n",
                "case", "fast_med", "gdal_med",
                "fast_p95", "gdal_p95", "ratio");
}

// ============================================================================
// Main benchmark runner
// ============================================================================

void store_result(CaseRunResults& results, TimedQueryResult fast,
                  GdalTimedResult gdal) {
    results.fast_ms_vals.push_back(fast.wall_ms);
    results.gdal_ms_vals.push_back(gdal.wall_ms);
    results.fast_results.push_back(std::move(fast.query));
    results.gdal_results.push_back(std::move(gdal));
}

void warm_gdal_query(GDALDataset* dataset, const BenchmarkCase& benchmark) {
    OGRLayer* layer = dataset->GetLayerByName(kLayerName);
    if (layer == nullptr) return;
    layer->SetSpatialFilterRect(benchmark.xmin, benchmark.ymin,
                                benchmark.xmax, benchmark.ymax);
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        OGRFeature::DestroyFeature(feature);
    }
}

void collect_steady_state(QueryEngine& engine, GDALDataset* dataset,
                          const std::vector<BenchmarkCase>& cases,
                          std::vector<CaseRunResults>& results) {
    for (const auto& benchmark : cases) {
        engine.query_bbox_unified(benchmark.xmin, benchmark.ymin,
                                  benchmark.xmax, benchmark.ymax);
        warm_gdal_query(dataset, benchmark);
    }
    const int num_trials = benchmark_trials();
    for (int trial = 0; trial < num_trials; ++trial) {
        for (size_t ci = 0; ci < cases.size(); ++ci) {
            const size_t idx = (ci + static_cast<size_t>(trial)) % cases.size();
            store_result(results[idx], query_fast_gdb(engine, cases[idx]),
                         query_gdal_steady_state(dataset, cases[idx]));
        }
    }
}

void run_fresh_pair(const std::string& gdb_path, const BenchmarkCase& benchmark,
                    bool fast_first, TimedQueryResult& fast,
                    GdalTimedResult& gdal) {
    if (fast_first) {
        fast = query_fast_gdb_fresh_open(gdb_path, benchmark);
        gdal = query_gdal_fresh_open(gdb_path, benchmark);
    } else {
        gdal = query_gdal_fresh_open(gdb_path, benchmark);
        fast = query_fast_gdb_fresh_open(gdb_path, benchmark);
    }
}

void collect_fresh_open(const std::string& gdb_path,
                        const std::vector<BenchmarkCase>& cases,
    std::vector<CaseRunResults>& results) {
    for (size_t ci = 0; ci < cases.size(); ++ci) {
        if (ci % 2 == 0) {
            query_fast_gdb_fresh_open(gdb_path, cases[ci]);
            query_gdal_fresh_open(gdb_path, cases[ci]);
        } else {
            query_gdal_fresh_open(gdb_path, cases[ci]);
            query_fast_gdb_fresh_open(gdb_path, cases[ci]);
        }
    }
    const int num_trials = benchmark_trials();
    for (int trial = 0; trial < num_trials; ++trial) {
        for (size_t ci = 0; ci < cases.size(); ++ci) {
            const size_t idx = (ci + static_cast<size_t>(trial)) % cases.size();
            TimedQueryResult fast;
            GdalTimedResult gdal;
            run_fresh_pair(gdb_path, cases[idx], trial % 2 == 0, fast, gdal);
            store_result(results[idx], std::move(fast), std::move(gdal));
        }
    }
}

size_t feature_count_from(const std::vector<CaseRunResults>& results) {
    for (const auto& result : results) {
        if (!result.fast_results.empty()) {
            return result.fast_results.front().spatial_metrics.feature_count;
        }
    }
    return 0;
}

void print_detailed_results(const std::vector<BenchmarkCase>& cases,
                            const std::vector<CaseRunResults>& results) {
    for (size_t ci = 0; ci < cases.size(); ++ci) {
        const size_t last = results[ci].fast_ms_vals.size() - 1;
        TimedQueryResult fast{results[ci].fast_results[last],
                               results[ci].fast_ms_vals[last]};
        print_row(cases[ci], fast, results[ci].gdal_results[last]);
    }
}

void print_summary(const std::vector<BenchmarkCase>& cases,
                   std::vector<CaseRunResults>& results,
                   const char* mode_str) {
    print_summary_header(mode_str);
    for (size_t ci = 0; ci < cases.size(); ++ci) {
        const double fast_med = median(results[ci].fast_ms_vals);
        const double gdal_med = median(results[ci].gdal_ms_vals);
        const double ratio = gdal_med > 0.0 ? fast_med / gdal_med : 0.0;
        print_summary_row(cases[ci].name, fast_med, gdal_med,
                          p95(results[ci].fast_ms_vals),
                          p95(results[ci].gdal_ms_vals), ratio);
    }
}

bool meets_performance_gate(const BenchmarkCase& benchmark, double fast_ms,
                            double gdal_ms) {
    if (gdal_ms <= 0.0) return false;
    if (benchmark.target_coverage <= 0.10) {
        return fast_ms <= gdal_ms + kSmallQueryToleranceMs ||
               fast_ms <= gdal_ms * kPerformanceRatioLimit;
    }
    return fast_ms <= gdal_ms * kPerformanceRatioLimit;
}

void verify_results(const std::vector<BenchmarkCase>& cases,
                    std::vector<CaseRunResults>& results,
                    bool profile_enabled) {
    for (size_t ci = 0; ci < cases.size(); ++ci) {
        for (size_t trial = 0; trial < results[ci].fast_results.size(); ++trial) {
            const auto& fast = results[ci].fast_results[trial];
            const auto& gdal = results[ci].gdal_results[trial];
            EXPECT_EQ(fast.matched_fids, gdal.fids) << cases[ci].name << trial;
            EXPECT_EQ(fast.spatial_metrics.invalid_geometries, 0u)
                << fast.fallback_reason;
            EXPECT_LE(fast.spatial_metrics.bbox_rejected,
                      fast.spatial_metrics.feature_count);
            EXPECT_LE(fast.spatial_metrics.bbox_contained,
                      fast.spatial_metrics.feature_count);
            EXPECT_LE(fast.spatial_metrics.exact_tested,
                      fast.spatial_metrics.feature_count);
        }
        if (!profile_enabled) {
            const double fast_ms = median(results[ci].fast_ms_vals);
            const double gdal_ms = median(results[ci].gdal_ms_vals);
            EXPECT_TRUE(meets_performance_gate(cases[ci], fast_ms, gdal_ms))
                << "Performance gate failed for " << cases[ci].name
                << ": fast=" << fast_ms << "ms gdal=" << gdal_ms << "ms";
        }
    }
}

size_t collect_steady_state(const fs::path& gdb_path,
                            const std::vector<BenchmarkCase>& cases,
    std::vector<CaseRunResults>& results) {
    GdbCatalog catalog;
    auto engine = open_fast_engine(gdb_path.string(), catalog);
    if (!engine || !engine->table()) {
        ADD_FAILURE() << "Cannot open fast-gdb engine";
        return 0;
    }
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        gdb_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    if (dataset == nullptr) {
        ADD_FAILURE() << "Cannot open GDB via GDAL";
        return 0;
    }
    const size_t feature_count = engine->table()->active_feature_count();
    collect_steady_state(*engine, dataset, cases, results);
    GDALClose(dataset);
    return feature_count;
}

void run_density_matrix(const fs::path& gdb_path, const std::string& label) {
    ASSERT_TRUE(fs::is_directory(gdb_path))
        << "Benchmark data not found: " << gdb_path;
    const auto mode = get_benchmark_mode();
    const char* mode_str = mode == BenchmarkMode::FreshOpen
        ? "fresh-open" : "steady-state";
    const bool profile_enabled = env_enabled("FAST_GDB_SPATIAL_PROFILE");
    const auto cases = density_cases();
    std::vector<CaseRunResults> results(cases.size());
    size_t feature_count = 0;
    if (mode == BenchmarkMode::SteadyState) {
        feature_count = collect_steady_state(gdb_path, cases, results);
    } else {
        collect_fresh_open(gdb_path.string(), cases, results);
        feature_count = feature_count_from(results);
    }
    print_header(label, feature_count, mode_str, profile_enabled);
    print_detailed_results(cases, results);
    print_summary(cases, results, mode_str);
    verify_results(cases, results, profile_enabled);
}

// ============================================================================
// Test fixture
// ============================================================================

class SpatialDensityBenchmark : public ::testing::Test {
protected:
    void SetUp() override {
        GDALAllRegister();
        if (!env_enabled("FAST_GDB_RUN_SPATIAL_BENCHMARKS")) {
            GTEST_SKIP()
                << "Set FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 to run benchmarks";
        }
        // NOTE: FAST_GDB_SPATIAL_PROFILE is NOT forced on here.
        // For wall-clock performance measurement, leave it unset.
        // Set FAST_GDB_SPATIAL_PROFILE=1 separately for detailed hotspot analysis.
        // This change is per Phase A of the spatial optimization plan:
        // "墙钟性能门禁关闭 FAST_GDB_SPATIAL_PROFILE"
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

TEST_F(SpatialDensityBenchmark, DensityMatrixConfigured) {
    const char* configured_path = std::getenv("FAST_GDB_BENCHMARK_PATH");
    if (configured_path == nullptr || configured_path[0] == '\0') {
        GTEST_SKIP() << "Set FAST_GDB_BENCHMARK_PATH to run a configured matrix";
    }
    const char* configured_label = std::getenv("FAST_GDB_BENCHMARK_LABEL");
    run_density_matrix(configured_path,
                       configured_label == nullptr ? "configured matrix" : configured_label);
}
