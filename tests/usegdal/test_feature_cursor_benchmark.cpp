// tests/usegdal/test_feature_cursor_benchmark.cpp
// 100K Point WKB-first 全对象读取、按需 WKT 与 GDAL 对照基准。

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
    RecordGeometry,
    Gdal
};

constexpr std::array<std::array<PathKind, 3>, kSamples> kExecutionOrders{{
    {{PathKind::Cursor, PathKind::RecordGeometry, PathKind::Gdal}},
    {{PathKind::RecordGeometry, PathKind::Gdal, PathKind::Cursor}},
    {{PathKind::Gdal, PathKind::Cursor, PathKind::RecordGeometry}},
    {{PathKind::Cursor, PathKind::Gdal, PathKind::RecordGeometry}},
    {{PathKind::RecordGeometry, PathKind::Cursor, PathKind::Gdal}},
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

double elapsed_ms(BenchmarkClock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        BenchmarkClock::now() - start).count();
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
    double checksum_ms = 0.0;
    Digest digest;
    std::string execution_path;
    CombinedQueryMetrics query_profile;
    FeatureCursorMetrics feature_profile;
};

struct WktResult {
    double milliseconds = 0.0;
    uint64_t feature_count = 0;
    uint64_t output_bytes = 0;
};

int fast_field_index(const GdbTableParser* table,
                     const std::string& name) {
    if (table == nullptr) return -1;
    const auto& fields = table->fields();
    for (size_t index = 0; index < fields.size(); ++index) {
        if (fields[index].name == name) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

std::vector<uint8_t> export_iso_wkb(OGRGeometry* geometry) {
    if (geometry == nullptr) return {};
    std::vector<uint8_t> wkb(
        static_cast<size_t>(geometry->WkbSize()));
    if (geometry->exportToWkb(
            wkbNDR, wkb.data(), wkbVariantIso) != OGRERR_NONE) {
        return {};
    }
    return wkb;
}

void add_fast_feature(Digest& digest,
                      uint32_t fid,
                      const FeatureRecord& record,
                      const GeometryValue& geometry,
                      int value_index,
                      int payload_index) {
    digest.add_scalar(fid);
    const int32_t value = std::get<int32_t>(
        record.field_values[static_cast<size_t>(value_index)]);
    digest.add_scalar(value);
    const auto& payload = std::get<std::vector<uint8_t>>(
        record.field_values[static_cast<size_t>(payload_index)]);
    digest.add_bytes(payload.data(), payload.size());
    digest.add_bytes(geometry.wkb.data(), geometry.wkb.size());
    ++digest.feature_count;
}

Digest consume_gdal(OGRLayer* layer) {
    Digest digest;
    layer->SetSpatialFilterRect(0, 0, 99, 99);
    if (layer->SetAttributeFilter("value >= 90") != OGRERR_NONE) {
        return digest;
    }
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        if (feature->GetFID() > 0) {
            const uint32_t fid =
                static_cast<uint32_t>(feature->GetFID() - 1);
            digest.add_scalar(fid);
            const int32_t value =
                feature->GetFieldAsInteger("value");
            digest.add_scalar(value);
            const int payload_index =
                feature->GetFieldIndex("payload");
            int byte_count = 0;
            const GByte* bytes = feature->GetFieldAsBinary(
                payload_index, &byte_count);
            if (bytes != nullptr && byte_count > 0) {
                digest.add_bytes(
                    bytes, static_cast<size_t>(byte_count));
            }
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
    QueryRequest cursor_request = request;
    cursor_request.profile_feature_reads = profile_enabled;

    const auto start = BenchmarkClock::now();
    QueryEngine engine(catalog, resolved);
    if (!engine.open()) return std::nullopt;
    const int value_index = fast_field_index(engine.table(), "value");
    const int payload_index = fast_field_index(engine.table(), "payload");
    if (value_index < 0 || payload_index < 0) return std::nullopt;

    FeatureCursor cursor = engine.open_cursor(cursor_request);
    if (!cursor.error().empty()) return std::nullopt;
    Digest digest;
    double checksum_ms = 0.0;
    QueryFeature feature;
    while (cursor.next(feature)) {
        if (profile_enabled) {
            const auto checksum_start = BenchmarkClock::now();
            add_fast_feature(digest, feature.fid, feature.record,
                             feature.geometry, value_index,
                             payload_index);
            checksum_ms += elapsed_ms(checksum_start);
        } else {
            add_fast_feature(digest, feature.fid, feature.record,
                             feature.geometry, value_index,
                             payload_index);
        }
    }
    if (!cursor.done() || !cursor.error().empty()) {
        return std::nullopt;
    }

    RunResult result;
    result.milliseconds = elapsed_ms(start);
    result.checksum_ms = checksum_ms;
    result.digest = digest;
    result.execution_path = cursor.query_result().execution_path;
    result.query_profile = cursor.query_result().combined_metrics;
    result.feature_profile =
        cursor.query_result().feature_cursor_metrics;
    return result;
}

std::optional<RunResult> run_record_geometry(
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
        FeatureRecord record;
        GeometryValue geometry;
        if (!engine.read_by_fid(fid, record) ||
            !engine.table()->read_geometry_value(fid, geometry)) {
            return std::nullopt;
        }
        add_fast_feature(digest, fid, record, geometry,
                         value_index, payload_index);
    }

    RunResult result;
    result.milliseconds = elapsed_ms(start);
    result.digest = digest;
    result.execution_path = query.execution_path;
    return result;
}

std::optional<RunResult> run_gdal(const std::string& path) {
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
    result.milliseconds = elapsed_ms(start);
    result.digest = digest;
    result.execution_path = "gdal:GetNextFeature";
    return result;
}

std::optional<WktResult> run_explicit_wkt(
    const GdbCatalog& catalog,
    const ResolvedTable& resolved,
    const QueryRequest& request) {
    QueryEngine engine(catalog, resolved);
    if (!engine.open()) return std::nullopt;
    const QueryResult query = engine.query(request);

    std::vector<GeometryValue> geometries;
    geometries.reserve(query.matched_fids.size());
    for (uint32_t fid : query.matched_fids) {
        GeometryValue geometry;
        if (!engine.table()->read_geometry_value(fid, geometry)) {
            return std::nullopt;
        }
        geometries.push_back(std::move(geometry));
    }

    WktResult result;
    const auto start = BenchmarkClock::now();
    for (const GeometryValue& geometry : geometries) {
        const auto wkt = geometry.to_wkt();
        if (!wkt.has_value()) return std::nullopt;
        result.output_bytes += wkt->size();
        ++result.feature_count;
    }
    result.milliseconds = elapsed_ms(start);
    return result;
}

struct Samples {
    std::vector<double> cursor_ms;
    std::vector<double> record_geometry_ms;
    std::vector<double> gdal_ms;
    Digest digest;
    std::string execution_path;
    CombinedQueryMetrics query_profile;
    FeatureCursorMetrics feature_profile;
    double checksum_ms = 0.0;
    WktResult explicit_wkt;
    bool correct = false;
};

void append_sample(Samples& samples,
                   PathKind path,
                   const RunResult& result) {
    switch (path) {
        case PathKind::Cursor:
            samples.cursor_ms.push_back(result.milliseconds);
            if (samples.execution_path.empty()) {
                samples.execution_path = result.execution_path;
            }
            break;
        case PathKind::RecordGeometry:
            samples.record_geometry_ms.push_back(result.milliseconds);
            break;
        case PathKind::Gdal:
            samples.gdal_ms.push_back(result.milliseconds);
            break;
    }
}

void write_evidence(const Samples& samples) {
    const char* configured =
        std::getenv("FAST_GDB_BENCHMARK_OUTPUT_DIR");
    const fs::path output_dir =
        configured != nullptr && *configured != '\0'
        ? fs::path(configured)
        : fs::temp_directory_path() / "fast-gdb-benchmark-results";
    fs::create_directories(output_dir);
    const fs::path output =
        output_dir / "feature-cursor-100k-schema-v3.json";

    std::ofstream json(output);
    json << std::fixed << std::setprecision(3)
         << "{\n"
         << "  \"evidence_schema_version\": 3,\n"
         << "  \"scenario\": \"wkb-first-full-feature-spatial-where-100k\",\n"
         << "  \"checksum_semantics\": \"fid-fields-binary-iso-wkb\",\n"
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
         << "  \"record_geometry_median_ms\": "
         << percentile(samples.record_geometry_ms, 0.5) << ",\n"
         << "  \"record_geometry_p95_ms\": "
         << percentile(samples.record_geometry_ms, 0.95) << ",\n"
         << "  \"gdal_median_ms\": "
         << percentile(samples.gdal_ms, 0.5) << ",\n"
         << "  \"gdal_p95_ms\": "
         << percentile(samples.gdal_ms, 0.95) << ",\n"
         << "  \"explicit_to_wkt_ms\": "
         << samples.explicit_wkt.milliseconds << ",\n"
         << "  \"explicit_to_wkt_count\": "
         << samples.explicit_wkt.feature_count << ",\n"
         << "  \"explicit_to_wkt_bytes\": "
         << samples.explicit_wkt.output_bytes << ",\n"
         << "  \"profile_query_total_ms\": "
         << samples.query_profile.total_ms << ",\n"
         << "  \"profile_query_spatial_ms\": "
         << samples.query_profile.spatial_ms << ",\n"
         << "  \"profile_query_attribute_ms\": "
         << samples.query_profile.attribute_ms << ",\n"
         << "  \"profile_query_intersection_ms\": "
         << samples.query_profile.intersection_ms << ",\n"
         << "  \"profile_fused_candidate_count\": "
         << samples.query_profile.fused_candidate_count << ",\n"
         << "  \"profile_spatial_match_count\": "
         << samples.query_profile.spatial_match_count << ",\n"
         << "  \"profile_attribute_tested\": "
         << samples.query_profile.attribute_tested << ",\n"
         << "  \"profile_fused_candidate_scan_ms\": "
         << samples.query_profile.fused_candidate_scan_ms << ",\n"
         << "  \"profile_attribute_metadata_ms\": "
         << samples.query_profile.attribute_metadata_ms << ",\n"
         << "  \"profile_fused_spatial_attribute_scan\": "
         << (samples.query_profile.fused_spatial_attribute_scan
                 ? "true" : "false") << ",\n"
         << "  \"profile_feature_count\": "
         << samples.feature_profile.feature_count << ",\n"
         << "  \"profile_row_lookup_ms\": "
         << samples.feature_profile.row_lookup_ms << ",\n"
         << "  \"profile_field_materialization_ms\": "
         << samples.feature_profile.field_materialization_ms << ",\n"
         << "  \"profile_geometry_decode_ms\": "
         << samples.feature_profile.geometry_decode_ms << ",\n"
         << "  \"profile_wkb_write_ms\": "
         << samples.feature_profile.wkb_write_ms << ",\n"
         << "  \"profile_checksum_sink_ms\": "
         << samples.checksum_ms << ",\n"
         << "  \"execution_path\": \""
         << samples.execution_path << "\",\n"
         << "  \"correct\": "
         << (samples.correct ? "true" : "false") << "\n"
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

TEST_F(FeatureCursorBenchmarkTest, Point100KWkbFirstEvidence) {
    if (!enabled()) {
        GTEST_SKIP()
            << "set FAST_GDB_RUN_FEATURE_CURSOR_BENCHMARKS=1";
    }

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
        std::optional<RunResult> record_geometry;
        std::optional<RunResult> gdal;
        for (PathKind path_kind :
             kExecutionOrders[static_cast<size_t>(sample)]) {
            std::optional<RunResult> result;
            switch (path_kind) {
                case PathKind::Cursor:
                    result = run_cursor(
                        catalog, *resolved, request, false);
                    cursor = result;
                    break;
                case PathKind::RecordGeometry:
                    result = run_record_geometry(
                        catalog, *resolved, request);
                    record_geometry = result;
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
        ASSERT_TRUE(record_geometry.has_value());
        ASSERT_TRUE(gdal.has_value());
        ASSERT_TRUE(cursor->digest == record_geometry->digest);
        ASSERT_TRUE(cursor->digest == gdal->digest);
        ASSERT_EQ(cursor->execution_path, samples.execution_path);
        samples.digest = cursor->digest;
    }

    const auto profile =
        run_cursor(catalog, *resolved, request, true);
    ASSERT_TRUE(profile.has_value());
    ASSERT_TRUE(profile->digest == samples.digest);
    ASSERT_TRUE(profile->query_profile.fused_spatial_attribute_scan);
    ASSERT_GT(profile->query_profile.fused_candidate_count, 0U);
    ASSERT_EQ(profile->query_profile.spatial_match_count, 10000U);
    ASSERT_EQ(profile->query_profile.attribute_tested, 10000U);
    ASSERT_EQ(profile->feature_profile.feature_count,
              samples.digest.feature_count);
    samples.query_profile = profile->query_profile;
    samples.feature_profile = profile->feature_profile;
    samples.checksum_ms = profile->checksum_ms;

    const auto explicit_wkt =
        run_explicit_wkt(catalog, *resolved, request);
    ASSERT_TRUE(explicit_wkt.has_value());
    ASSERT_EQ(explicit_wkt->feature_count,
              samples.digest.feature_count);
    samples.explicit_wkt = *explicit_wkt;

    ASSERT_GT(samples.digest.feature_count, 0U);
    ASSERT_EQ(samples.cursor_ms.size(),
              static_cast<size_t>(kSamples));
    ASSERT_EQ(samples.record_geometry_ms.size(),
              static_cast<size_t>(kSamples));
    ASSERT_EQ(samples.gdal_ms.size(),
              static_cast<size_t>(kSamples));
    samples.correct = true;
    write_evidence(samples);
}
