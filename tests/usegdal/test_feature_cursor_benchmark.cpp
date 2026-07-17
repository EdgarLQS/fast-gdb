#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
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
namespace fs = std::filesystem;
using BenchmarkClock = std::chrono::steady_clock;

namespace {

constexpr const char* kLayer = "feature_cursor_benchmark";
constexpr int kFeatureCount = 100000;
constexpr int kSamples = 5;

enum class PathKind {
    Cursor,
    Legacy,
    Gdal
};

constexpr std::array<std::array<PathKind, 3>, kSamples> kExecutionOrders{{
    {{PathKind::Cursor, PathKind::Legacy, PathKind::Gdal}},
    {{PathKind::Legacy, PathKind::Gdal, PathKind::Cursor}},
    {{PathKind::Gdal, PathKind::Cursor, PathKind::Legacy}},
    {{PathKind::Cursor, PathKind::Gdal, PathKind::Legacy}},
    {{PathKind::Legacy, PathKind::Cursor, PathKind::Gdal}},
}};

bool enabled() {
    const char* value = std::getenv(
        "FAST_GDB_RUN_FEATURE_CURSOR_BENCHMARKS");
    return value != nullptr && std::string(value) == "1";
}

bool execute_sql(GDALDataset* dataset, const std::string& sql) {
    CPLErrorReset();
    OGRLayer* result = dataset->ExecuteSQL(sql.c_str(), nullptr, nullptr);
    if (result != nullptr) dataset->ReleaseResultSet(result);
    return CPLGetLastErrorType() < CE_Failure;
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(
        std::ceil(fraction * static_cast<double>(values.size()))) - 1U;
    return values[std::min(index, values.size() - 1U)];
}

struct Digest {
    uint64_t hash = 1469598103934665603ULL;
    uint64_t feature_count = 0;
    uint64_t output_bytes = 0;

    void add_bytes(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ULL;
        }
        output_bytes += size;
    }

    template <typename T>
    void add_scalar(const T& value) {
        add_bytes(&value, sizeof(value));
    }

    bool operator==(const Digest& other) const {
        return hash == other.hash &&
               feature_count == other.feature_count &&
               output_bytes == other.output_bytes;
    }
};

struct RunResult {
    double milliseconds = 0.0;
    Digest digest;
    std::string execution_path;
    FeatureCursorMetrics profile;
};

class ScopedProfileEnvironment {
public:
    explicit ScopedProfileEnvironment(bool enabled) {
        const char* current = std::getenv("FAST_GDB_FEATURE_CURSOR_PROFILE");
        if (current != nullptr) {
            had_previous_ = true;
            previous_ = current;
        }
#ifdef _WIN32
        _putenv_s("FAST_GDB_FEATURE_CURSOR_PROFILE", enabled ? "1" : "0");
#else
        setenv("FAST_GDB_FEATURE_CURSOR_PROFILE", enabled ? "1" : "0", 1);
#endif
    }

    ~ScopedProfileEnvironment() {
#ifdef _WIN32
        _putenv_s("FAST_GDB_FEATURE_CURSOR_PROFILE",
                  had_previous_ ? previous_.c_str() : "");
#else
        if (had_previous_)
            setenv("FAST_GDB_FEATURE_CURSOR_PROFILE", previous_.c_str(), 1);
        else
            unsetenv("FAST_GDB_FEATURE_CURSOR_PROFILE");
#endif
    }

private:
    std::string previous_;
    bool had_previous_ = false;
};

int fast_field_index(const GdbTableParser* table, const std::string& name) {
    if (table == nullptr) return -1;
    const auto& fields = table->fields();
    for (size_t index = 0; index < fields.size(); ++index) {
        if (fields[index].name == name) return static_cast<int>(index);
    }
    return -1;
}

std::vector<uint8_t> export_iso_wkb(OGRGeometry* geometry) {
    if (geometry == nullptr) return {};
    std::vector<uint8_t> wkb(static_cast<size_t>(geometry->WkbSize()));
    if (geometry->exportToWkb(
            wkbNDR, wkb.data(), wkbVariantIso) != OGRERR_NONE) {
        return {};
    }
    return wkb;
}

void add_fast_feature(Digest& digest,
                      const QueryFeature& feature,
                      int value_index,
                      int payload_index) {
    digest.add_scalar(feature.fid);
    const int32_t value = std::get<int32_t>(
        feature.record.field_values[static_cast<size_t>(value_index)]);
    digest.add_scalar(value);
    const auto& payload = std::get<std::vector<uint8_t>>(
        feature.record.field_values[static_cast<size_t>(payload_index)]);
    digest.add_bytes(payload.data(), payload.size());
    digest.add_bytes(feature.geometry.wkb.data(), feature.geometry.wkb.size());
    ++digest.feature_count;
}

Digest consume_gdal(OGRLayer* layer) {
    Digest digest;
    layer->SetSpatialFilterRect(0, 0, 99, 99);
    if (layer->SetAttributeFilter("value >= 90") != OGRERR_NONE)
        return digest;
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        if (feature->GetFID() > 0) {
            const uint32_t fid =
                static_cast<uint32_t>(feature->GetFID() - 1);
            digest.add_scalar(fid);
            const int32_t value = feature->GetFieldAsInteger("value");
            digest.add_scalar(value);
            const int payload_index = feature->GetFieldIndex("payload");
            int byte_count = 0;
            const GByte* bytes = feature->GetFieldAsBinary(
                payload_index, &byte_count);
            if (bytes != nullptr && byte_count > 0)
                digest.add_bytes(bytes, static_cast<size_t>(byte_count));
            const std::vector<uint8_t> wkb =
                export_iso_wkb(feature->GetGeometryRef());
            digest.add_bytes(wkb.data(), wkb.size());
            ++digest.feature_count;
        }
        OGRFeature::DestroyFeature(feature);
    }
    return digest;
}

std::optional<RunResult> run_cursor(
    const GdbCatalog& catalog,
    const ResolvedTable& resolved,
    const QueryRequest& request,
    bool profile_enabled) {
    ScopedProfileEnvironment profile_environment(profile_enabled);
    const auto start = BenchmarkClock::now();
    QueryEngine engine(catalog, resolved);
    if (!engine.open()) return std::nullopt;
    const int value_index = fast_field_index(engine.table(), "value");
    const int payload_index = fast_field_index(engine.table(), "payload");
    if (value_index < 0 || payload_index < 0) return std::nullopt;

    FeatureCursor cursor = engine.open_cursor(request);
    if (!cursor.error().empty()) return std::nullopt;
    Digest digest;
    QueryFeature feature;
    while (cursor.next(feature))
        add_fast_feature(digest, feature, value_index, payload_index);
    if (!cursor.done() || !cursor.error().empty()) return std::nullopt;

    RunResult result;
    result.milliseconds =
        std::chrono::duration<double, std::milli>(
            BenchmarkClock::now() - start).count();
    result.digest = digest;
    result.execution_path = cursor.query_result().execution_path;
    result.profile = cursor.query_result().feature_cursor_metrics;
    return result;
}

std::optional<RunResult> run_legacy(
    const GdbCatalog& catalog,
    const ResolvedTable& resolved,
    const QueryRequest& request) {
    const auto start = BenchmarkClock::now();
    QueryEngine engine(catalog, resolved);
    if (!engine.open()) return std::nullopt;
    const int value_index = fast_field_index(engine.table(), "value");
    const int payload_index = fast_field_index(engine.table(), "payload");
    if (value_index < 0 || payload_index < 0) return std::nullopt;

    const QueryResult query = engine.query(request);
    Digest digest;
    for (uint32_t fid : query.matched_fids) {
        QueryFeature feature;
        feature.fid = fid;
        if (!engine.read_by_fid(fid, feature.record) ||
            !engine.table()->read_geometry_value(fid, feature.geometry)) {
            return std::nullopt;
        }
        add_fast_feature(digest, feature, value_index, payload_index);
    }

    RunResult result;
    result.milliseconds =
        std::chrono::duration<double, std::milli>(
            BenchmarkClock::now() - start).count();
    result.digest = digest;
    result.execution_path = query.execution_path;
    return result;
}

std::optional<RunResult> run_gdal(
    const std::string& path) {
    const auto start = BenchmarkClock::now();
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    if (dataset == nullptr) return std::nullopt;
    OGRLayer* layer = dataset->GetLayerByName(kLayer);
    if (layer == nullptr) {
        GDALClose(dataset);
        return std::nullopt;
    }
    const Digest digest = consume_gdal(layer);
    GDALClose(dataset);

    RunResult result;
    result.milliseconds =
        std::chrono::duration<double, std::milli>(
            BenchmarkClock::now() - start).count();
    result.digest = digest;
    result.execution_path = "gdal:GetNextFeature";
    return result;
}

struct Samples {
    std::vector<double> cursor_ms;
    std::vector<double> legacy_ms;
    std::vector<double> gdal_ms;
    Digest digest;
    std::string execution_path;
    FeatureCursorMetrics profile;
    bool correct = false;
};

void append_sample(Samples& samples,
                   PathKind path,
                   const RunResult& result) {
    switch (path) {
        case PathKind::Cursor:
            samples.cursor_ms.push_back(result.milliseconds);
            if (samples.execution_path.empty())
                samples.execution_path = result.execution_path;
            break;
        case PathKind::Legacy:
            samples.legacy_ms.push_back(result.milliseconds);
            break;
        case PathKind::Gdal:
            samples.gdal_ms.push_back(result.milliseconds);
            break;
    }
}

void write_evidence(const Samples& samples) {
    const char* configured = std::getenv("FAST_GDB_BENCHMARK_OUTPUT_DIR");
    const fs::path output_dir = configured ? configured : "benchmark_results";
    fs::create_directories(output_dir);
    const fs::path output =
        output_dir / "feature-cursor-100k-schema-v2.json";
    std::ofstream json(output);
    json << std::fixed << std::setprecision(3)
         << "{\n"
         << "  \"evidence_schema_version\": 2,\n"
         << "  \"scenario\": \"full-feature-spatial-where-100k\",\n"
         << "  \"timing_scope\": \"engine-or-dataset-open-through-last-feature\",\n"
         << "  \"cache_semantics\": \"fresh-open-not-strict-cold\",\n"
         << "  \"execution_order\": \"rotated-five-sample-schedule\",\n"
         << "  \"profile_sample_in_timing\": false,\n"
         << "  \"feature_count\": " << kFeatureCount << ",\n"
         << "  \"result_count\": " << samples.digest.feature_count << ",\n"
         << "  \"output_bytes\": " << samples.digest.output_bytes << ",\n"
         << "  \"checksum\": " << samples.digest.hash << ",\n"
         << "  \"sample_count\": " << kSamples << ",\n"
         << "  \"cursor_median_ms\": "
         << percentile(samples.cursor_ms, 0.5) << ",\n"
         << "  \"cursor_p95_ms\": "
         << percentile(samples.cursor_ms, 0.95) << ",\n"
         << "  \"legacy_median_ms\": "
         << percentile(samples.legacy_ms, 0.5) << ",\n"
         << "  \"legacy_p95_ms\": "
         << percentile(samples.legacy_ms, 0.95) << ",\n"
         << "  \"gdal_median_ms\": "
         << percentile(samples.gdal_ms, 0.5) << ",\n"
         << "  \"gdal_p95_ms\": "
         << percentile(samples.gdal_ms, 0.95) << ",\n"
         << "  \"profile_feature_count\": "
         << samples.profile.feature_count << ",\n"
         << "  \"profile_row_lookup_ms\": "
         << samples.profile.row_lookup_ms << ",\n"
         << "  \"profile_field_materialization_ms\": "
         << samples.profile.field_materialization_ms << ",\n"
         << "  \"profile_geometry_decode_ms\": "
         << samples.profile.geometry_decode_ms << ",\n"
         << "  \"profile_wkt_write_ms\": "
         << samples.profile.wkt_write_ms << ",\n"
         << "  \"profile_wkb_write_ms\": "
         << samples.profile.wkb_write_ms << ",\n"
         << "  \"execution_path\": \"" << samples.execution_path
         << "\",\n"
         << "  \"correct\": " << (samples.correct ? "true" : "false")
         << "\n"
         << "}\n";
    EXPECT_TRUE(json.good()) << "failed to write " << output;
}

} // namespace

class FeatureCursorBenchmarkTest : public GdbTutorialFixture {
protected:
    std::string create_fixture() {
        const std::string path =
            spatial_where_test_utils::fixture_path(
                "fast_gdb_feature_cursor_benchmark").string();
        GDALDataset* dataset = createGdb(path.c_str());
        if (dataset == nullptr) return {};
        OGRLayer* layer = dataset->CreateLayer(
            kLayer, nullptr, wkbPoint, nullptr);
        if (layer == nullptr) {
            GDALClose(dataset);
            return {};
        }
        OGRFieldDefn value_field("value", OFTInteger);
        OGRFieldDefn payload_field("payload", OFTBinary);
        if (layer->CreateField(&value_field) != OGRERR_NONE ||
            layer->CreateField(&payload_field) != OGRERR_NONE) {
            GDALClose(dataset);
            return {};
        }
        const int payload_index =
            layer->GetLayerDefn()->GetFieldIndex("payload");
        const bool transaction_started =
            layer->StartTransaction() == OGRERR_NONE;
        for (int fid = 0; fid < kFeatureCount; ++fid) {
            OGRFeature* feature = OGRFeature::CreateFeature(
                layer->GetLayerDefn());
            if (feature == nullptr) {
                GDALClose(dataset);
                return {};
            }
            feature->SetField("value", fid % 100);
            const GByte payload[] = {
                static_cast<GByte>(fid & 0xff),
                static_cast<GByte>((fid >> 8) & 0xff),
                static_cast<GByte>(0x5a)};
            feature->SetField(payload_index, 3, payload);
            OGRPoint point(fid % 1000, fid / 1000);
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
        if (layer == nullptr) {
            GDALClose(dataset);
            return {};
        }
        layer->ResetReading();
        while (OGRFeature* feature = layer->GetNextFeature()) {
            const OGRErr error = layer->SetFeature(feature);
            OGRFeature::DestroyFeature(feature);
            if (error != OGRERR_NONE) {
                GDALClose(dataset);
                return {};
            }
        }
        const bool prepared = execute_sql(
            dataset, std::string("RECOMPUTE EXTENT ON ") + kLayer) &&
            execute_sql(
                dataset, std::string("CREATE INDEX value_idx ON ") +
                             kLayer + "(value)");
        GDALClose(dataset);
        return prepared ? path : std::string{};
    }
};

TEST_F(FeatureCursorBenchmarkTest, Point100KFullFeatureEvidence) {
    if (!enabled())
        GTEST_SKIP() << "set FAST_GDB_RUN_FEATURE_CURSOR_BENCHMARKS=1";

    const std::string path = create_fixture();
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve(kLayer);
    ASSERT_TRUE(resolved.has_value());

    QueryRequest request;
    request.kind = QueryKind::SpatialWhere;
    request.xmin = 0;
    request.ymin = 0;
    request.xmax = 99;
    request.ymax = 99;
    request.where_clause = "value >= 90";

    Samples samples;
    for (int sample = 0; sample < kSamples; ++sample) {
        std::optional<RunResult> cursor;
        std::optional<RunResult> legacy;
        std::optional<RunResult> gdal;
        for (PathKind path_kind : kExecutionOrders[static_cast<size_t>(sample)]) {
            std::optional<RunResult> result;
            switch (path_kind) {
                case PathKind::Cursor:
                    result = run_cursor(catalog, *resolved, request, false);
                    cursor = result;
                    break;
                case PathKind::Legacy:
                    result = run_legacy(catalog, *resolved, request);
                    legacy = result;
                    break;
                case PathKind::Gdal:
                    result = run_gdal(path);
                    gdal = result;
                    break;
            }
            ASSERT_TRUE(result.has_value());
            append_sample(samples, path_kind, *result);
        }
        ASSERT_TRUE(cursor.has_value());
        ASSERT_TRUE(legacy.has_value());
        ASSERT_TRUE(gdal.has_value());
        ASSERT_TRUE(cursor->digest == legacy->digest);
        ASSERT_TRUE(cursor->digest == gdal->digest);
        ASSERT_EQ(cursor->execution_path, samples.execution_path);
        samples.digest = cursor->digest;
    }

    const auto profile = run_cursor(catalog, *resolved, request, true);
    ASSERT_TRUE(profile.has_value());
    ASSERT_TRUE(profile->digest == samples.digest);
    ASSERT_EQ(profile->profile.feature_count, samples.digest.feature_count);
    samples.profile = profile->profile;

    ASSERT_GT(samples.digest.feature_count, 0U);
    ASSERT_EQ(samples.cursor_ms.size(), static_cast<size_t>(kSamples));
    ASSERT_EQ(samples.legacy_ms.size(), static_cast<size_t>(kSamples));
    ASSERT_EQ(samples.gdal_ms.size(), static_cast<size_t>(kSamples));
    samples.correct = true;
    write_evidence(samples);
}
