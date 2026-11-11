// Spatial source-proof benchmark: cold directory build vs warm candidate queries.
//
// Gated by FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 and FAST_GDB_BENCHMARK_PATH.
// Optional environment:
//   FAST_GDB_BENCHMARK_CASE           run one configured case
//   FAST_GDB_BENCHMARK_TRIALS         trials per case (default 5, max 100)
//   FAST_GDB_BENCHMARK_LAYER          layer name (default "features")
//   FAST_GDB_BENCHMARK_BACKEND        "fast", "gdal" or "both" (default "both")
//   FAST_GDB_BENCHMARK_STRICT_COLD=1  clear the external file cache per sample
//   FAST_GDB_BENCHMARK_CACHE_CLEAR_COMMAND  required by strict cold mode

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/resource.h>

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace explorgdb;

namespace {

constexpr double kExtentMin = 0.0;
constexpr double kExtentMax = 100000.0;

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::string(value) == "1";
}

std::string env_string(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return value == nullptr || value[0] == '\0' ? fallback
                                                  : std::string(value);
}

int benchmark_trials() {
    const char* value = std::getenv("FAST_GDB_BENCHMARK_TRIALS");
    if (value == nullptr || *value == '\0') return 5;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 1 || parsed > 100) return 5;
    return static_cast<int>(parsed);
}

bool strict_cold_mode() {
    return env_enabled("FAST_GDB_BENCHMARK_STRICT_COLD");
}

void clear_external_file_cache() {
    if (!strict_cold_mode()) return;
    const char* command = std::getenv("FAST_GDB_BENCHMARK_CACHE_CLEAR_COMMAND");
    if (command == nullptr || command[0] == '\0')
        throw std::runtime_error(
            "strict cold mode requires FAST_GDB_BENCHMARK_CACHE_CLEAR_COMMAND");
    if (std::system(command) != 0)
        throw std::runtime_error("external file-cache clear failed");
}

struct BenchmarkCase {
    const char* name;
    double target_coverage;
    double xmin, ymin, xmax, ymax;
};

BenchmarkCase centered_case(const char* name, double coverage) {
    const double center = (kExtentMin + kExtentMax) / 2.0;
    const double half = std::sqrt(coverage) * (kExtentMax - kExtentMin) / 2.0;
    return BenchmarkCase{name, coverage, center - half, center - half,
                         center + half, center + half};
}

std::vector<BenchmarkCase> density_cases() {
    std::vector<BenchmarkCase> cases = {
        centered_case("coverage_01pct", 0.01),
        centered_case("coverage_10pct", 0.10),
        centered_case("coverage_25pct", 0.25),
        centered_case("coverage_80pct", 0.80),
        {"coverage_full", 1.0, -1000.0, -1000.0, 101000.0, 101000.0}};
    const std::string selected = env_string("FAST_GDB_BENCHMARK_CASE", "");
    if (selected.empty()) return cases;
    cases.erase(std::remove_if(cases.begin(), cases.end(),
                               [&](const BenchmarkCase& item) {
                                   return selected != item.name;
                               }),
                cases.end());
    return cases;
}

enum class Backend { Both, Fast, Gdal };

Backend selected_backend() {
    const std::string value = env_string("FAST_GDB_BENCHMARK_BACKEND", "both");
    if (value == "fast") return Backend::Fast;
    if (value == "gdal") return Backend::Gdal;
    return Backend::Both;
}

uint64_t process_peak_rss_bytes() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss);
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024U;
#endif
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(
        std::ceil(fraction * static_cast<double>(values.size())) - 1.0);
    return values[std::min(index, values.size() - 1)];
}

double median(std::vector<double> values) {
    return percentile(std::move(values), 0.5);
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

struct FastSample {
    QueryResult result;
    double wall_ms = 0.0;
};

FastSample run_fast(const std::string& path, const std::string& layer,
                    const BenchmarkCase& benchmark) {
    clear_external_file_cache();
    const auto start = Clock::now();
    FastSample sample;
    auto state = open_fast(path, layer);
    if (state)
        sample.result = state->engine->query_bbox_unified(
            benchmark.xmin, benchmark.ymin, benchmark.xmax, benchmark.ymax);
    sample.wall_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - start).count();
    return sample;
}

struct GdalSample {
    std::vector<RowSlot> fids;
    double wall_ms = 0.0;
};

GdalSample run_gdal(const std::string& path, const std::string& layer,
                    const BenchmarkCase& benchmark) {
    clear_external_file_cache();
    const auto start = Clock::now();
    GdalSample sample;
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
        nullptr));
    if (dataset == nullptr) return sample;
    OGRLayer* source = dataset->GetLayerByName(layer.c_str());
    if (source != nullptr) {
        source->SetSpatialFilterRect(benchmark.xmin, benchmark.ymin,
                                     benchmark.xmax, benchmark.ymax);
        source->ResetReading();
        while (OGRFeature* feature = source->GetNextFeature()) {
            const GIntBig fid = feature->GetFID();
            if (fid >= 1) sample.fids.push_back(static_cast<RowSlot>(fid - 1));
            OGRFeature::DestroyFeature(feature);
        }
        source->SetSpatialFilter(nullptr);
    }
    GDALClose(dataset);
    std::sort(sample.fids.begin(), sample.fids.end());
    sample.fids.erase(std::unique(sample.fids.begin(), sample.fids.end()),
                      sample.fids.end());
    sample.wall_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - start).count();
    return sample;
}

// Independent truth: scan every source geometry and test its envelope against
// the query window. GDAL's own spatial filter consults the .spx index and has
// been observed to disagree with a geometry scan on windows that extend to
// negative coordinates, so the filter is only used as a timing reference.
GdalSample run_gdal_geometry_truth(const std::string& path,
                                   const std::string& layer,
                                   const BenchmarkCase& benchmark) {
    GdalSample sample;
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr,
        nullptr));
    if (dataset == nullptr) return sample;
    if (OGRLayer* source = dataset->GetLayerByName(layer.c_str())) {
        source->ResetReading();
        while (OGRFeature* feature = source->GetNextFeature()) {
            const OGRGeometry* geometry = feature->GetGeometryRef();
            if (geometry != nullptr) {
                OGREnvelope envelope{};
                geometry->getEnvelope(&envelope);
                if (envelope.MinX <= benchmark.xmax &&
                    envelope.MaxX >= benchmark.xmin &&
                    envelope.MinY <= benchmark.ymax &&
                    envelope.MaxY >= benchmark.ymin) {
                    const GIntBig fid = feature->GetFID();
                    if (fid >= 1)
                        sample.fids.push_back(static_cast<RowSlot>(fid - 1));
                }
            }
            OGRFeature::DestroyFeature(feature);
        }
    }
    GDALClose(dataset);
    std::sort(sample.fids.begin(), sample.fids.end());
    sample.fids.erase(std::unique(sample.fids.begin(), sample.fids.end()),
                      sample.fids.end());
    return sample;
}

void print_header(const std::string& label, const char* mode) {
    std::printf("\n=== Source-proof spatial benchmark: %s (%s) ===\n",
                label.c_str(), mode);
    std::printf("%-16s %9s %8s %-30s %10s %10s %10s %8s %8s %8s\n",
                "case", "cands", "ratio", "path", "total_ms", "build_ms",
                "dir_bytes", "leaves", "entries", "pages");
}

void print_fast_row(const BenchmarkCase& benchmark, const FastSample& sample) {
    const auto& metrics = sample.result.spatial_metrics;
    std::printf("%-16s %9zu %7.2f%% %-30s %10.1f %10.2f %10zu %8zu %8zu %8zu\n",
                benchmark.name, metrics.candidate_count,
                metrics.candidate_ratio * 100.0,
                sample.result.execution_path.c_str(), sample.wall_ms,
                metrics.source_directory_build_ms,
                metrics.source_directory_bytes, metrics.source_directory_leaves,
                metrics.source_directory_entries,
                metrics.source_index_pages_read);
}

void print_gdal_row(const BenchmarkCase& benchmark, const GdalSample& sample,
                    size_t truth_count) {
    std::printf("%-16s %9zu %7s %-30s %10.1f %10s %10s %8s %8s %8s\n",
                benchmark.name, sample.fids.size(), "-", "gdal:spatial-filter",
                sample.wall_ms, "-", "-", "-", "-", "-");
    if (sample.fids.size() != truth_count)
        std::printf("  [gdal-filter] %s differs from geometry truth: "
                    "filter=%zu truth=%zu\n",
                    benchmark.name, sample.fids.size(), truth_count);
}

bool verify_fast(const BenchmarkCase& benchmark, const FastSample& fast,
                 const GdalSample& gdal, bool compare_fids) {
    EXPECT_EQ(fast.result.status, QueryStatus::Ok)
        << benchmark.name << ": " << fast.result.error;
    if (!compare_fids || fast.result.status != QueryStatus::Ok) return true;
    EXPECT_EQ(fast.result.matched_fids, gdal.fids)
        << benchmark.name << " path=" << fast.result.execution_path
        << " reason=" << fast.result.fallback_reason;
    return fast.result.matched_fids == gdal.fids;
}

void run_cold_matrix(const std::string& path, const std::string& layer,
                     const std::string& label,
                     const std::vector<BenchmarkCase>& cases) {
    const Backend backend = selected_backend();
    print_header(label, strict_cold_mode()
                            ? "cold-fresh-open"
                            : "fresh-open (OS cache not cleared)");
    for (const auto& benchmark : cases) {
        GdalSample oracle;
        if (backend == Backend::Gdal) {
            oracle = run_gdal_geometry_truth(path, layer, benchmark);
            for (int trial = 0; trial < benchmark_trials(); ++trial)
                print_gdal_row(benchmark, run_gdal(path, layer, benchmark),
                               oracle.fids.size());
            continue;
        }
        if (backend == Backend::Both)
            oracle = run_gdal_geometry_truth(path, layer, benchmark);
        for (int trial = 0; trial < benchmark_trials(); ++trial) {
            const FastSample sample = run_fast(path, layer, benchmark);
            verify_fast(benchmark, sample, oracle, backend == Backend::Both);
            print_fast_row(benchmark, sample);
        }
        if (backend == Backend::Both)
            print_gdal_row(benchmark, run_gdal(path, layer, benchmark),
                           oracle.fids.size());
    }
    std::printf("peak_rss_bytes=%llu\n",
                static_cast<unsigned long long>(process_peak_rss_bytes()));
}

void run_warm_matrix(const std::string& path, const std::string& layer,
                     const std::string& label,
                     const std::vector<BenchmarkCase>& cases) {
    const Backend backend = selected_backend();
    if (backend == Backend::Gdal) {
        ADD_FAILURE() << "warm source-proof mode requires FAST_GDB_BENCHMARK_BACKEND=fast or both";
        return;
    }
    auto state = open_fast(path, layer);
    if (!state) {
        ADD_FAILURE() << "cannot open " << path << "/" << layer;
        return;
    }
    for (const auto& benchmark : cases)
        state->engine->query_bbox_unified(benchmark.xmin, benchmark.ymin,
                                          benchmark.xmax, benchmark.ymax);

    print_header(label, "warm-steady-state");
    for (const auto& benchmark : cases) {
        const GdalSample oracle = backend == Backend::Both
            ? run_gdal_geometry_truth(path, layer, benchmark) : GdalSample{};
        std::vector<double> wall_ms;
        QueryResult last;
        for (int trial = 0; trial < benchmark_trials(); ++trial) {
            const auto start = Clock::now();
            last = state->engine->query_bbox_unified(
                benchmark.xmin, benchmark.ymin, benchmark.xmax, benchmark.ymax);
            wall_ms.push_back(std::chrono::duration<double, std::milli>(
                Clock::now() - start).count());
        }
        FastSample sample{last, median(wall_ms)};
        print_fast_row(benchmark, sample);
        std::printf("  warm median=%.2fms p95=%.2fms hit=%s\n",
                    median(wall_ms), percentile(wall_ms, 0.95),
                    sample.result.spatial_metrics.source_directory_hit ? "true"
                                                                       : "false");
        verify_fast(benchmark, sample, oracle, backend == Backend::Both);
    }
    std::printf("peak_rss_bytes=%llu\n",
                static_cast<unsigned long long>(process_peak_rss_bytes()));
}

struct BenchmarkConfig {
    std::string path;
    std::string layer;
    std::string label;
    std::vector<BenchmarkCase> cases;
};

std::optional<BenchmarkConfig> benchmark_config() {
    if (!env_enabled("FAST_GDB_RUN_SPATIAL_BENCHMARKS")) return std::nullopt;
    GDALAllRegister();
    BenchmarkConfig config;
    config.path = env_string("FAST_GDB_BENCHMARK_PATH", "");
    if (config.path.empty() || !fs::is_directory(config.path))
        return std::nullopt;
    config.layer = env_string("FAST_GDB_BENCHMARK_LAYER", "features");
    config.label = env_string("FAST_GDB_BENCHMARK_LABEL", config.path);
    config.cases = density_cases();
    if (config.cases.empty()) return std::nullopt;
    return config;
}

} // namespace

TEST(SpatialSourceProofBenchmark, ColdBuildMatrix) {
    const auto config = benchmark_config();
    if (!config) GTEST_SKIP()
        << "Set FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 and FAST_GDB_BENCHMARK_PATH";
    run_cold_matrix(config->path, config->layer, config->label, config->cases);
}

TEST(SpatialSourceProofBenchmark, WarmQueryMatrix) {
    const auto config = benchmark_config();
    if (!config) GTEST_SKIP()
        << "Set FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 and FAST_GDB_BENCHMARK_PATH";
    run_warm_matrix(config->path, config->layer, config->label, config->cases);
}
