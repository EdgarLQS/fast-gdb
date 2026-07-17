#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
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
namespace fs = std::filesystem;
using BenchmarkClock = std::chrono::steady_clock;

namespace {

constexpr const char* kLayer = "combined_benchmark_points";
constexpr int kFeatureCount = 100000;
constexpr int kSamples = 5;

bool enabled() {
    const char* value = std::getenv(
        "FAST_GDB_RUN_SPATIAL_WHERE_BENCHMARKS");
    return value != nullptr && std::string(value) == "1";
}

bool sql(GDALDataset* dataset, const std::string& text) {
    CPLErrorReset();
    OGRLayer* result = dataset->ExecuteSQL(text.c_str(), nullptr, nullptr);
    if (result) dataset->ReleaseResultSet(result);
    return CPLGetLastErrorType() < CE_Failure;
}

double percentile(std::vector<double> samples, double fraction) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t index = static_cast<size_t>(
        std::ceil(fraction * static_cast<double>(samples.size()))) - 1U;
    return samples[std::min(index, samples.size() - 1U)];
}

std::vector<uint32_t> collect_gdal(OGRLayer* layer) {
    std::vector<uint32_t> fids;
    layer->SetSpatialFilterRect(0.0, 0.0, 99.0, 99.0);
    if (layer->SetAttributeFilter("value >= 90") != OGRERR_NONE)
        return fids;
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        if (feature->GetFID() > 0)
            fids.push_back(static_cast<uint32_t>(feature->GetFID() - 1));
        OGRFeature::DestroyFeature(feature);
    }
    layer->SetSpatialFilter(nullptr);
    layer->SetAttributeFilter(nullptr);
    std::sort(fids.begin(), fids.end());
    fids.erase(std::unique(fids.begin(), fids.end()), fids.end());
    return fids;
}

struct Samples {
    std::vector<double> combined_ms;
    std::vector<double> legacy_ms;
    std::vector<double> gdal_ms;
    CombinedQueryMetrics metrics;
    std::vector<uint32_t> expected;
    std::string execution_path;
    bool correct = false;
};

void write_evidence(const Samples& samples) {
    const char* configured = std::getenv("FAST_GDB_BENCHMARK_OUTPUT_DIR");
    const fs::path output_dir = configured ? configured : "benchmark_results";
    fs::create_directories(output_dir);
    const fs::path output =
        output_dir / "spatial-where-100k-schema-v2.json";
    std::ofstream json(output);
    const double combined_median = percentile(samples.combined_ms, 0.5);
    const double legacy_median = percentile(samples.legacy_ms, 0.5);
    const double legacy_regression_percent = legacy_median > 0.0
        ? (combined_median / legacy_median - 1.0) * 100.0
        : 0.0;
    json << std::fixed << std::setprecision(3)
         << "{\n"
         << "  \"evidence_schema_version\": 2,\n"
         << "  \"scenario\": \"spatial-10pct-attribute-10pct\",\n"
         << "  \"manifest\": \"spatial_where_point_100k_seed_0\",\n"
         << "  \"feature_count\": " << kFeatureCount << ",\n"
         << "  \"result_count\": " << samples.expected.size() << ",\n"
         << "  \"sample_count\": " << kSamples << ",\n"
         << "  \"combined_median_ms\": "
         << combined_median << ",\n"
         << "  \"combined_p95_ms\": "
         << percentile(samples.combined_ms, 0.95) << ",\n"
         << "  \"legacy_median_ms\": "
         << legacy_median << ",\n"
         << "  \"legacy_p95_ms\": "
         << percentile(samples.legacy_ms, 0.95) << ",\n"
         << "  \"gdal_median_ms\": "
         << percentile(samples.gdal_ms, 0.5) << ",\n"
         << "  \"gdal_p95_ms\": "
         << percentile(samples.gdal_ms, 0.95) << ",\n"
         << "  \"execution_path\": \"" << samples.execution_path
         << "\",\n"
         << "  \"spatial_candidate_count\": "
         << samples.metrics.spatial_candidate_count << ",\n"
         << "  \"spatial_match_count\": "
         << samples.metrics.spatial_match_count << ",\n"
         << "  \"attribute_candidate_count\": "
         << samples.metrics.attribute_candidate_count << ",\n"
         << "  \"attribute_tested\": "
         << samples.metrics.attribute_tested << ",\n"
         << "  \"final_match_count\": "
         << samples.metrics.final_match_count << ",\n"
         << "  \"legacy_regression_percent\": "
         << legacy_regression_percent << ",\n"
         << "  \"performance_gate_status\": \"not_run\",\n"
         << "  \"correct\": " << (samples.correct ? "true" : "false")
         << "\n"
         << "}\n";
    EXPECT_TRUE(json.good()) << "failed to write " << output;
}

} // namespace

class SpatialWhereBenchmarkTest : public GdbTutorialFixture {
protected:
    std::string createFixture() {
        const std::string path =
            (fs::temp_directory_path() /
             "fast_gdb_spatial_where_benchmark_100k.gdb").string();
        GDALDataset* dataset = createGdb(path.c_str());
        EXPECT_NE(dataset, nullptr);
        if (!dataset) return {};

        OGRLayer* layer = dataset->CreateLayer(
            kLayer, nullptr, wkbPoint, nullptr);
        EXPECT_NE(layer, nullptr);
        if (!layer) { GDALClose(dataset); return {}; }
        OGRFieldDefn value("value", OFTInteger);
        EXPECT_EQ(layer->CreateField(&value), OGRERR_NONE);

        const bool transaction_started =
            layer->StartTransaction() == OGRERR_NONE;
        for (int fid = 0; fid < kFeatureCount; ++fid) {
            OGRFeature* feature = OGRFeature::CreateFeature(
                layer->GetLayerDefn());
            feature->SetField("value", fid % 100);
            OGRPoint point(fid % 1000, fid / 1000);
            EXPECT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
            EXPECT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
            OGRFeature::DestroyFeature(feature);
        }
        if (transaction_started)
            EXPECT_EQ(layer->CommitTransaction(), OGRERR_NONE);
        GDALClose(dataset);

        dataset = static_cast<GDALDataset*>(GDALOpenEx(
            path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
            nullptr, nullptr, nullptr));
        EXPECT_NE(dataset, nullptr);
        if (!dataset) return {};
        layer = dataset->GetLayerByName(kLayer);
        layer->ResetReading();
        while (OGRFeature* feature = layer->GetNextFeature()) {
            EXPECT_EQ(layer->SetFeature(feature), OGRERR_NONE);
            OGRFeature::DestroyFeature(feature);
        }
        EXPECT_TRUE(sql(dataset, std::string("RECOMPUTE EXTENT ON ") + kLayer));
        EXPECT_TRUE(sql(dataset, std::string("CREATE INDEX value_idx ON ") +
                                 kLayer + "(value)"));
        GDALClose(dataset);
        return path;
    }
};

TEST_F(SpatialWhereBenchmarkTest, Point100KSchemaV2Evidence) {
    if (!enabled())
        GTEST_SKIP() << "set FAST_GDB_RUN_SPATIAL_WHERE_BENCHMARKS=1";

    const std::string path = createFixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve(kLayer);
    ASSERT_TRUE(resolved.has_value());
    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());

    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->GetLayerByName(kLayer);
    ASSERT_NE(layer, nullptr);

    QueryRequest combined_request;
    combined_request.kind = QueryKind::SpatialWhere;
    combined_request.xmin = 0.0;
    combined_request.ymin = 0.0;
    combined_request.xmax = 99.0;
    combined_request.ymax = 99.0;
    combined_request.where_clause = "value >= 90";

    Samples samples;
    for (int sample = 0; sample < kSamples; ++sample) {
        auto start = BenchmarkClock::now();
        const QueryResult combined = engine.query(combined_request);
        samples.combined_ms.push_back(
            std::chrono::duration<double, std::milli>(
                BenchmarkClock::now() - start).count());
        ASSERT_EQ(combined.execution_path, "spatial-where:spx+atx");
        if (samples.execution_path.empty())
            samples.execution_path = combined.execution_path;
        ASSERT_EQ(combined.execution_path, samples.execution_path);
        samples.metrics = combined.combined_metrics;

        start = BenchmarkClock::now();
        QueryResult spatial = engine.query_bbox_unified(
            0.0, 0.0, 99.0, 99.0);
        std::vector<uint32_t> attribute = engine.query_attribute_double(
            "value_idx", 90.0, AttrOp::Ge);
        std::sort(spatial.matched_fids.begin(), spatial.matched_fids.end());
        std::sort(attribute.begin(), attribute.end());
        std::vector<uint32_t> legacy;
        std::set_intersection(
            spatial.matched_fids.begin(), spatial.matched_fids.end(),
            attribute.begin(), attribute.end(),
            std::back_inserter(legacy));
        samples.legacy_ms.push_back(
            std::chrono::duration<double, std::milli>(
                BenchmarkClock::now() - start).count());

        start = BenchmarkClock::now();
        const std::vector<uint32_t> gdal = collect_gdal(layer);
        samples.gdal_ms.push_back(
            std::chrono::duration<double, std::milli>(
                BenchmarkClock::now() - start).count());

        ASSERT_EQ(combined.matched_fids, legacy);
        ASSERT_EQ(combined.matched_fids, gdal);
        samples.expected = combined.matched_fids;
    }
    GDALClose(dataset);

    ASSERT_FALSE(samples.expected.empty());
    ASSERT_EQ(samples.metrics.final_match_count, samples.expected.size());
    samples.correct = true;
    write_evidence(samples);
}
