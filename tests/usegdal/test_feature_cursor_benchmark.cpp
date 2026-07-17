#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
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

struct Samples {
    std::vector<double> cursor_ms;
    std::vector<double> legacy_ms;
    std::vector<double> gdal_ms;
    Digest digest;
    std::string execution_path;
    bool correct = false;
};

void write_evidence(const Samples& samples) {
    const char* configured = std::getenv("FAST_GDB_BENCHMARK_OUTPUT_DIR");
    const fs::path output_dir = configured ? configured : "benchmark_results";
    fs::create_directories(output_dir);
    const fs::path output =
        output_dir / "feature-cursor-100k-schema-v1.json";
    std::ofstream json(output);
    json << std::fixed << std::setprecision(3)
         << "{\n"
         << "  \"evidence_schema_version\": 1,\n"
         << "  \"scenario\": \"full-feature-spatial-where-100k\",\n"
         << "  \"timing_scope\": \"engine-or-dataset-open-through-last-feature\",\n"
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
        auto start = BenchmarkClock::now();
        QueryEngine cursor_engine(catalog, *resolved);
        ASSERT_TRUE(cursor_engine.open());
        const int cursor_value_index =
            fast_field_index(cursor_engine.table(), "value");
        const int cursor_payload_index =
            fast_field_index(cursor_engine.table(), "payload");
        ASSERT_GE(cursor_value_index, 0);
        ASSERT_GE(cursor_payload_index, 0);
        FeatureCursor cursor = cursor_engine.open_cursor(request);
        ASSERT_TRUE(cursor.error().empty()) << cursor.error();
        Digest cursor_digest;
        QueryFeature feature;
        while (cursor.next(feature)) {
            add_fast_feature(cursor_digest, feature,
                             cursor_value_index, cursor_payload_index);
        }
        ASSERT_TRUE(cursor.done());
        ASSERT_TRUE(cursor.error().empty());
        samples.cursor_ms.push_back(
            std::chrono::duration<double, std::milli>(
                BenchmarkClock::now() - start).count());
        if (samples.execution_path.empty())
            samples.execution_path = cursor.query_result().execution_path;
        ASSERT_EQ(cursor.query_result().execution_path,
                  samples.execution_path);

        start = BenchmarkClock::now();
        QueryEngine legacy_engine(catalog, *resolved);
        ASSERT_TRUE(legacy_engine.open());
        const int legacy_value_index =
            fast_field_index(legacy_engine.table(), "value");
        const int legacy_payload_index =
            fast_field_index(legacy_engine.table(), "payload");
        ASSERT_GE(legacy_value_index, 0);
        ASSERT_GE(legacy_payload_index, 0);
        const QueryResult query = legacy_engine.query(request);
        Digest legacy_digest;
        for (uint32_t fid : query.matched_fids) {
            QueryFeature legacy;
            legacy.fid = fid;
            ASSERT_TRUE(legacy_engine.read_by_fid(fid, legacy.record));
            ASSERT_TRUE(legacy_engine.table()->read_geometry_value(
                fid, legacy.geometry));
            add_fast_feature(legacy_digest, legacy,
                             legacy_value_index, legacy_payload_index);
        }
        samples.legacy_ms.push_back(
            std::chrono::duration<double, std::milli>(
                BenchmarkClock::now() - start).count());

        start = BenchmarkClock::now();
        GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
            path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr));
        ASSERT_NE(dataset, nullptr);
        OGRLayer* layer = dataset->GetLayerByName(kLayer);
        ASSERT_NE(layer, nullptr);
        const Digest gdal_digest = consume_gdal(layer);
        GDALClose(dataset);
        samples.gdal_ms.push_back(
            std::chrono::duration<double, std::milli>(
                BenchmarkClock::now() - start).count());

        ASSERT_TRUE(cursor_digest == legacy_digest);
        ASSERT_TRUE(cursor_digest == gdal_digest);
        samples.digest = cursor_digest;
    }

    ASSERT_GT(samples.digest.feature_count, 0U);
    samples.correct = true;
    write_evidence(samples);
}
