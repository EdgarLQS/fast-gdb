// S4.5 read-only runtime benchmark for fresh opens, random reads, scans, and filters.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "cpl_error.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"
#include "spatial_where_test_utils.h"
#include "test_fixture.h"

using namespace explorgdb;
namespace fs = std::filesystem;
using BenchmarkClock = std::chrono::steady_clock;

namespace {

constexpr const char* kLayerName = "s45_runtime_points";
constexpr size_t kFeatureCount = 100000;
constexpr size_t kSampleCount = 101;
constexpr size_t kRandomReadCount = 256;

bool enabled() {
    const char* value = std::getenv("FAST_GDB_RUN_RUNTIME_BENCHMARKS");
    return value != nullptr && std::string(value) == "1";
}

bool execute_sql(GDALDataset* dataset, const std::string& sql) {
    CPLErrorReset();
    OGRLayer* result = dataset->ExecuteSQL(sql.c_str(), nullptr, nullptr);
    if (result != nullptr) dataset->ReleaseResultSet(result);
    return CPLGetLastErrorType() < CE_Failure;
}

struct ProcessSnapshot {
    bool disk_read_available = false;
    uint64_t disk_read_bytes = 0;
};

ProcessSnapshot process_snapshot() {
#if defined(__APPLE__)
    rusage_info_v4 usage{};
    if (proc_pid_rusage(
            getpid(), RUSAGE_INFO_V4,
            reinterpret_cast<rusage_info_t*>(&usage)) == 0) {
        return {true, usage.ri_diskio_bytesread};
    }
#endif
    return {};
}

uint64_t process_peak_rss_bytes() {
#if defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return static_cast<uint64_t>(usage.ru_maxrss);
    }
#endif
    return 0;
}

bool process_peak_rss_available() {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

struct Digest {
    uint64_t hash = 1469598103934665603ULL;
    uint64_t feature_count = 0;

    void add_bytes(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ULL;
        }
    }

    template <typename T>
    void add_scalar(const T& value) {
        add_bytes(&value, sizeof(value));
    }

    bool operator==(const Digest& other) const {
        return hash == other.hash && feature_count == other.feature_count;
    }
};

std::vector<uint8_t> export_iso_wkb(OGRGeometry* geometry) {
    if (geometry == nullptr) return {};
    std::vector<uint8_t> wkb(static_cast<size_t>(geometry->WkbSize()));
    if (geometry->exportToWkb(
            wkbNDR, wkb.data(), wkbVariantIso) != OGRERR_NONE) {
        return {};
    }
    return wkb;
}

int fast_field_index(const GdbTableParser* table, const std::string& name) {
    if (table == nullptr) return -1;
    const auto& fields = table->fields();
    for (size_t index = 0; index < fields.size(); ++index) {
        if (fields[index].name == name) return static_cast<int>(index);
    }
    return -1;
}

void add_fast_feature(Digest& digest,
                      uint32_t fid,
                      const FeatureRecord& record,
                      const GeometryValue& geometry,
                      int value_index) {
    digest.add_scalar(fid);
    digest.add_scalar(std::get<int32_t>(
        record.field_values[static_cast<size_t>(value_index)]));
    digest.add_bytes(geometry.wkb.data(), geometry.wkb.size());
    ++digest.feature_count;
}

void add_gdal_feature(Digest& digest,
                      OGRFeature* feature,
                      const char* value_field = "value") {
    const GIntBig gdal_fid = feature->GetFID();
    EXPECT_GT(gdal_fid, 0);
    digest.add_scalar(static_cast<uint32_t>(gdal_fid - 1));
    digest.add_scalar(static_cast<int32_t>(
        feature->GetFieldAsInteger(value_field)));
    const std::vector<uint8_t> wkb = export_iso_wkb(feature->GetGeometryRef());
    digest.add_bytes(wkb.data(), wkb.size());
    ++digest.feature_count;
}

struct TimedSample {
    double milliseconds = 0.0;
    uint64_t disk_read_bytes = 0;
    bool disk_read_available = false;
};

struct Distribution {
    std::vector<TimedSample> samples;
};

template <typename Operation>
TimedSample measure(Operation&& operation) {
    const ProcessSnapshot before = process_snapshot();
    const auto start = BenchmarkClock::now();
    EXPECT_TRUE(operation());
    const double milliseconds = std::chrono::duration<double, std::milli>(
        BenchmarkClock::now() - start).count();
    const ProcessSnapshot after = process_snapshot();
    TimedSample sample;
    sample.milliseconds = milliseconds;
    sample.disk_read_available = before.disk_read_available &&
                                 after.disk_read_available;
    if (sample.disk_read_available &&
        after.disk_read_bytes >= before.disk_read_bytes) {
        sample.disk_read_bytes =
            after.disk_read_bytes - before.disk_read_bytes;
    }
    return sample;
}

double percentile(const Distribution& distribution, double fraction) {
    if (distribution.samples.empty()) return 0.0;
    std::vector<double> values;
    values.reserve(distribution.samples.size());
    for (const auto& sample : distribution.samples)
        values.push_back(sample.milliseconds);
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(
        std::ceil(fraction * static_cast<double>(values.size()))) - 1U;
    return values[std::min(index, values.size() - 1U)];
}

std::string json_escape(const std::string& value) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char character : value) {
        switch (character) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (character < 0x20) {
                    escaped += "\\u00";
                    escaped.push_back(kHex[character >> 4]);
                    escaped.push_back(kHex[character & 0x0f]);
                } else {
                    escaped.push_back(static_cast<char>(character));
                }
        }
    }
    return escaped;
}

uint64_t total_disk_read_bytes(const Distribution& distribution) {
    uint64_t total = 0;
    for (const auto& sample : distribution.samples)
        total += sample.disk_read_bytes;
    return total;
}

double median(const Distribution& distribution) {
    return percentile(distribution, 0.50);
}

bool disk_read_counter_available(const Distribution& distribution) {
    return std::all_of(
        distribution.samples.begin(), distribution.samples.end(),
        [](const TimedSample& sample) { return sample.disk_read_available; });
}

void write_distribution(std::ofstream& json,
                        const char* name,
                        const Distribution& distribution,
                        uint64_t operations_per_sample) {
    const double median_ms = percentile(distribution, 0.50);
    const double throughput = median_ms > 0.0
        ? static_cast<double>(operations_per_sample) * 1000.0 / median_ms
        : 0.0;
    json << "  \"" << name << "_median_ms\": " << median_ms << ",\n"
         << "  \"" << name << "_p95_ms\": "
         << percentile(distribution, 0.95) << ",\n"
         << "  \"" << name << "_p99_ms\": "
         << percentile(distribution, 0.99) << ",\n"
         << "  \"" << name << "_throughput_per_second\": "
         << throughput << ",\n"
         << "  \"" << name << "_disk_read_bytes_total\": "
         << total_disk_read_bytes(distribution) << ",\n"
         << "  \"" << name << "_disk_read_counter_available\": "
         << (disk_read_counter_available(distribution) ? "true" : "false")
         << ",\n";
}

bool open_fast_context(const std::string& path) {
    GdbCatalog catalog;
    if (!catalog.scan(path)) return false;
    CatalogResolver resolver(catalog);
    if (!resolver.load()) return false;
    const auto resolved = resolver.resolve(kLayerName);
    if (!resolved.has_value()) return false;
    QueryEngine engine(catalog, *resolved);
    return engine.open();
}

bool open_gdal_context(const std::string& path) {
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    if (dataset == nullptr) return false;
    const bool found = dataset->GetLayerByName(kLayerName) != nullptr;
    GDALClose(dataset);
    return found;
}

bool read_fast_random(QueryEngine& engine,
                      const std::vector<uint32_t>& fids,
                      int value_index,
                      Digest& digest) {
    for (uint32_t fid : fids) {
        FeatureRecord record;
        GeometryValue geometry;
        if (!engine.read_by_fid(fid, record) ||
            !engine.table()->read_geometry_value(fid, geometry)) {
            return false;
        }
        add_fast_feature(digest, fid, record, geometry, value_index);
    }
    return true;
}

bool read_gdal_random(OGRLayer* layer,
                      const std::vector<uint32_t>& fids,
                      Digest& digest,
                      const char* value_field = "value") {
    for (uint32_t fid : fids) {
        OGRFeature* feature = layer->GetFeature(
            static_cast<GIntBig>(fid) + 1);
        if (feature == nullptr) return false;
        add_gdal_feature(digest, feature, value_field);
        OGRFeature::DestroyFeature(feature);
    }
    return true;
}

bool scan_fast(QueryEngine& engine, int value_index, Digest& digest) {
    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    FeatureCursor cursor = engine.open_cursor(request);
    if (!cursor.error().empty()) return false;
    QueryFeature feature;
    while (cursor.next(feature)) {
        add_fast_feature(digest, feature.fid, feature.record,
                         feature.geometry, value_index);
    }
    return cursor.done() && cursor.error().empty();
}

bool scan_gdal(OGRLayer* layer,
               Digest& digest,
               const char* value_field = "value") {
    layer->SetAttributeFilter(nullptr);
    layer->SetSpatialFilter(nullptr);
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        add_gdal_feature(digest, feature, value_field);
        OGRFeature::DestroyFeature(feature);
    }
    return CPLGetLastErrorType() < CE_Failure;
}

std::vector<explorgdb::RowSlot> collect_gdal_fids(OGRLayer* layer,
                                                  double xmax,
                                                  double ymax) {
    std::vector<explorgdb::RowSlot> fids;
    layer->SetSpatialFilterRect(0.0, 0.0, xmax, ymax);
    if (layer->SetAttributeFilter("value >= 90") != OGRERR_NONE) return fids;
    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        const GIntBig gdal_fid = feature->GetFID();
        if (gdal_fid > 0)
            fids.push_back(static_cast<explorgdb::RowSlot>(gdal_fid - 1));
        OGRFeature::DestroyFeature(feature);
    }
    layer->SetSpatialFilter(nullptr);
    layer->SetAttributeFilter(nullptr);
    std::sort(fids.begin(), fids.end());
    return fids;
}

} // namespace

class ReaderRuntimeBenchmarkTest : public GdbTutorialFixture {
protected:
    std::string create_fixture() {
        const std::string path = spatial_where_test_utils::fixture_path(
            "fast_gdb_s45_runtime").string();
        GDALDataset* dataset = createGdb(path.c_str());
        if (dataset == nullptr) return {};
        OGRLayer* layer = dataset->CreateLayer(
            kLayerName, nullptr, wkbPoint, nullptr);
        if (layer == nullptr) {
            GDALClose(dataset);
            return {};
        }
        OGRFieldDefn value("value", OFTInteger);
        if (layer->CreateField(&value) != OGRERR_NONE) {
            GDALClose(dataset);
            return {};
        }
        const bool transaction_started =
            layer->StartTransaction() == OGRERR_NONE;
        for (size_t fid = 0; fid < kFeatureCount; ++fid) {
            OGRFeature* feature = OGRFeature::CreateFeature(
                layer->GetLayerDefn());
            if (feature == nullptr) {
                GDALClose(dataset);
                return {};
            }
            feature->SetField("value", static_cast<int>(fid % 100));
            OGRPoint point(static_cast<double>(fid % 1000),
                            static_cast<double>(fid / 1000));
            const OGRErr geometry_error = feature->SetGeometry(&point);
            const OGRErr create_error = geometry_error == OGRERR_NONE
                ? layer->CreateFeature(feature) : geometry_error;
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
        layer = dataset->GetLayerByName(kLayerName);
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
        const bool extent_updated = execute_sql(
            dataset, std::string("RECOMPUTE EXTENT ON ") + kLayerName);
        const bool index_created = extent_updated && execute_sql(
            dataset, std::string("CREATE INDEX value_idx ON ") +
                        kLayerName + "(value)");
        GDALClose(dataset);
        return index_created ? path : std::string{};
    }
};

TEST_F(ReaderRuntimeBenchmarkTest, ReadOnlyRuntimeProfile100K) {
    if (!enabled())
        GTEST_SKIP() << "set FAST_GDB_RUN_RUNTIME_BENCHMARKS=1";

    const std::string path = create_fixture();
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve(kLayerName);
    ASSERT_TRUE(resolved.has_value());
    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());
    const int value_index = fast_field_index(engine.table(), "value");
    ASSERT_GE(value_index, 0);

    GDALDataset* gdal_dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    ASSERT_NE(gdal_dataset, nullptr);
    OGRLayer* gdal_layer = gdal_dataset->GetLayerByName(kLayerName);
    ASSERT_NE(gdal_layer, nullptr);

    Distribution fast_open;
    Distribution gdal_open;
    Distribution fast_hot_open;
    Distribution gdal_hot_lookup;
    Distribution fast_random;
    Distribution gdal_random;
    Distribution fast_scan;
    Distribution gdal_scan;
    Distribution fast_combined;
    Distribution gdal_combined;
    Distribution fast_indexed;
    Distribution gdal_indexed;
    std::vector<uint32_t> random_fids;
    random_fids.reserve(kRandomReadCount);
    std::mt19937 rng(0x5A17U);
    std::uniform_int_distribution<uint32_t> fid_distribution(
        0, static_cast<uint32_t>(kFeatureCount - 1));
    for (size_t index = 0; index < kRandomReadCount; ++index)
        random_fids.push_back(fid_distribution(rng));

    QueryRequest selective_request;
    selective_request.kind = QueryKind::SpatialWhere;
    selective_request.xmin = 0.0;
    selective_request.ymin = 0.0;
    selective_request.xmax = 99.0;
    selective_request.ymax = 99.0;
    selective_request.where_clause = "value >= 90";

    QueryRequest indexed_request = selective_request;
    indexed_request.xmax = 999.0;
    indexed_request.ymax = 79.0;
    CombinedQueryMetrics indexed_metrics;

    Digest expected_scan_digest;
    std::vector<explorgdb::RowSlot> expected_selective_fids;
    std::vector<explorgdb::RowSlot> expected_indexed_fids;

    for (size_t sample_index = 0;
         sample_index < kSampleCount;
         ++sample_index) {
        TimedSample fast_open_sample;
        TimedSample gdal_open_sample;
        if (sample_index % 2 == 0) {
            fast_open_sample = measure([&] { return open_fast_context(path); });
            gdal_open_sample = measure([&] { return open_gdal_context(path); });
        } else {
            gdal_open_sample = measure([&] { return open_gdal_context(path); });
            fast_open_sample = measure([&] { return open_fast_context(path); });
        }
        fast_open.samples.push_back(fast_open_sample);
        gdal_open.samples.push_back(gdal_open_sample);

        TimedSample fast_hot_sample;
        TimedSample gdal_hot_sample;
        if (sample_index % 2 == 0) {
            fast_hot_sample = measure([&] { return engine.open(); });
            gdal_hot_sample = measure([&] {
                return gdal_dataset->GetLayerByName(kLayerName) != nullptr;
            });
        } else {
            gdal_hot_sample = measure([&] {
                return gdal_dataset->GetLayerByName(kLayerName) != nullptr;
            });
            fast_hot_sample = measure([&] { return engine.open(); });
        }
        fast_hot_open.samples.push_back(fast_hot_sample);
        gdal_hot_lookup.samples.push_back(gdal_hot_sample);

        Digest fast_random_digest;
        Digest gdal_random_digest;
        TimedSample fast_random_sample;
        TimedSample gdal_random_sample;
        if (sample_index % 2 == 0) {
            fast_random_sample = measure([&] {
                fast_random_digest = {};
                return read_fast_random(
                    engine, random_fids, value_index, fast_random_digest);
            });
            gdal_random_sample = measure([&] {
                gdal_random_digest = {};
                return read_gdal_random(
                    gdal_layer, random_fids, gdal_random_digest);
            });
        } else {
            gdal_random_sample = measure([&] {
                gdal_random_digest = {};
                return read_gdal_random(
                    gdal_layer, random_fids, gdal_random_digest);
            });
            fast_random_sample = measure([&] {
                fast_random_digest = {};
                return read_fast_random(
                    engine, random_fids, value_index, fast_random_digest);
            });
        }
        ASSERT_EQ(fast_random_digest, gdal_random_digest);
        fast_random.samples.push_back(fast_random_sample);
        gdal_random.samples.push_back(gdal_random_sample);

        Digest fast_scan_digest;
        Digest gdal_scan_digest;
        TimedSample fast_scan_sample;
        TimedSample gdal_scan_sample;
        if (sample_index % 2 == 0) {
            fast_scan_sample = measure([&] {
                fast_scan_digest = {};
                return scan_fast(engine, value_index, fast_scan_digest);
            });
            gdal_scan_sample = measure([&] {
                gdal_scan_digest = {};
                return scan_gdal(gdal_layer, gdal_scan_digest);
            });
        } else {
            gdal_scan_sample = measure([&] {
                gdal_scan_digest = {};
                return scan_gdal(gdal_layer, gdal_scan_digest);
            });
            fast_scan_sample = measure([&] {
                fast_scan_digest = {};
                return scan_fast(engine, value_index, fast_scan_digest);
            });
        }
        ASSERT_EQ(fast_scan_digest, gdal_scan_digest);
        ASSERT_EQ(fast_scan_digest.feature_count, kFeatureCount);
        expected_scan_digest = fast_scan_digest;
        fast_scan.samples.push_back(fast_scan_sample);
        gdal_scan.samples.push_back(gdal_scan_sample);

        QueryResult fast_selective_result;
        std::vector<explorgdb::RowSlot> gdal_selective_result;
        TimedSample fast_combined_sample;
        TimedSample gdal_combined_sample;
        if (sample_index % 2 == 0) {
            fast_combined_sample = measure([&] {
                fast_selective_result = engine.query(selective_request);
                return fast_selective_result.status == QueryStatus::Ok;
            });
            gdal_combined_sample = measure([&] {
                gdal_selective_result =
                    collect_gdal_fids(gdal_layer, 99.0, 99.0);
                return !gdal_selective_result.empty();
            });
        } else {
            gdal_combined_sample = measure([&] {
                gdal_selective_result =
                    collect_gdal_fids(gdal_layer, 99.0, 99.0);
                return !gdal_selective_result.empty();
            });
            fast_combined_sample = measure([&] {
                fast_selective_result = engine.query(selective_request);
                return fast_selective_result.status == QueryStatus::Ok;
            });
        }
        ASSERT_EQ(fast_selective_result.matched_fids,
                  gdal_selective_result);
        expected_selective_fids = fast_selective_result.matched_fids;
        fast_combined.samples.push_back(fast_combined_sample);
        gdal_combined.samples.push_back(gdal_combined_sample);

        QueryResult fast_indexed_result;
        std::vector<explorgdb::RowSlot> gdal_indexed_result;
        TimedSample fast_indexed_sample;
        TimedSample gdal_indexed_sample;
        if (sample_index % 2 == 0) {
            fast_indexed_sample = measure([&] {
                fast_indexed_result = engine.query(indexed_request);
                return fast_indexed_result.status == QueryStatus::Ok;
            });
            gdal_indexed_sample = measure([&] {
                gdal_indexed_result =
                    collect_gdal_fids(gdal_layer, 999.0, 79.0);
                return !gdal_indexed_result.empty();
            });
        } else {
            gdal_indexed_sample = measure([&] {
                gdal_indexed_result =
                    collect_gdal_fids(gdal_layer, 999.0, 79.0);
                return !gdal_indexed_result.empty();
            });
            fast_indexed_sample = measure([&] {
                fast_indexed_result = engine.query(indexed_request);
                return fast_indexed_result.status == QueryStatus::Ok;
            });
        }
        ASSERT_EQ(fast_indexed_result.matched_fids, gdal_indexed_result);
        indexed_metrics = fast_indexed_result.combined_metrics;
        expected_indexed_fids = fast_indexed_result.matched_fids;
        fast_indexed.samples.push_back(fast_indexed_sample);
        gdal_indexed.samples.push_back(gdal_indexed_sample);
    }

    GDALClose(gdal_dataset);
    ASSERT_EQ(expected_scan_digest.feature_count, kFeatureCount);
    ASSERT_FALSE(expected_selective_fids.empty());
    ASSERT_FALSE(expected_indexed_fids.empty());
    ASSERT_TRUE(indexed_metrics.used_attribute_index);
    ASSERT_GT(indexed_metrics.attribute_index_pages_visited, 0U);

    const char* configured = std::getenv("FAST_GDB_BENCHMARK_OUTPUT_DIR");
    const fs::path output_dir = configured != nullptr && *configured != '\0'
        ? fs::path(configured)
        : fs::temp_directory_path() / "fast-gdb-benchmark-results";
    fs::create_directories(output_dir);
    std::ofstream json(output_dir / "s4.5-runtime-100k.json");
    ASSERT_TRUE(json.good());
    json << std::fixed << std::setprecision(6)
         << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"dataset\": \"synthetic GDAL OpenFileGDB; 100000 points; value integer; value_idx ATX\",\n"
         << "  \"gdal_version\": \"" << GDALVersionInfo("RELEASE_NAME") << "\",\n"
         << "  \"sample_count\": " << kSampleCount << ",\n"
         << "  \"random_reads_per_sample\": " << kRandomReadCount << ",\n"
         << "  \"open_semantics\": \"fresh reader/dataset handles; operating-system file cache not cleared\",\n"
         << "  \"hot_semantics\": \"Fast QueryEngine::open on an open engine; GDAL layer lookup on an open dataset; reused reader/dataset for read workloads\",\n"
         << "  \"cold_open_measurement\": \"fresh_context_open; physical cold-open was not measured\",\n"
         << "  \"cold_cache_clear_performed\": false,\n"
         << "  \"percentile_method\": \"nearest rank; 101 samples\",\n"
         << "  \"throughput_semantics\": \"open/s for opens; feature reads/s for random reads; features/s for scans and returned features/s for filters\",\n"
         << "  \"disk_read_counter\": \"macOS proc_pid_rusage ri_diskio_bytesread; physical disk I/O only, cache hits excluded\",\n";
    write_distribution(json, "fast_fresh_open", fast_open, 1);
    write_distribution(json, "gdal_fresh_open", gdal_open, 1);
    write_distribution(json, "fast_hot_open_call", fast_hot_open, 1);
    write_distribution(json, "gdal_hot_layer_lookup", gdal_hot_lookup, 1);
    write_distribution(json, "fast_random_read", fast_random, kRandomReadCount);
    write_distribution(json, "gdal_random_read", gdal_random, kRandomReadCount);
    write_distribution(json, "fast_full_scan", fast_scan, kFeatureCount);
    write_distribution(json, "gdal_full_scan", gdal_scan, kFeatureCount);
    write_distribution(json, "fast_spatial_where_10pct", fast_combined,
                        expected_selective_fids.size());
    write_distribution(json, "gdal_spatial_where_10pct", gdal_combined,
                        expected_selective_fids.size());
    write_distribution(json, "fast_spatial_where_indexed", fast_indexed,
                        expected_indexed_fids.size());
    write_distribution(json, "gdal_spatial_where_indexed", gdal_indexed,
                        expected_indexed_fids.size());
    json << "  \"selective_result_count\": "
         << expected_selective_fids.size() << ",\n"
         << "  \"indexed_result_count\": " << expected_indexed_fids.size()
         << ",\n"
         << "  \"attribute_index_used\": "
         << (indexed_metrics.used_attribute_index ? "true" : "false") << ",\n"
         << "  \"attribute_index_page_count\": "
         << indexed_metrics.attribute_index_page_count << ",\n"
         << "  \"attribute_index_pages_visited\": "
         << indexed_metrics.attribute_index_pages_visited << ",\n"
         << "  \"attribute_index_entries_scanned\": "
         << indexed_metrics.attribute_index_entries_scanned << ",\n"
         << "  \"median_fast_not_slower_gate\": "
         << ((median(fast_open) <= median(gdal_open) &&
              median(fast_random) <= median(gdal_random) &&
              median(fast_scan) <= median(gdal_scan) &&
              median(fast_combined) <= median(gdal_combined) &&
              median(fast_indexed) <= median(gdal_indexed))
                 ? "\"pass\"" : "\"fail\"") << ",\n"
         << "  \"median_fast_not_slower_gate_rule\": \"Fast median <= GDAL median for fresh open, random read, full scan, selective and indexed SpatialWhere\",\n"
         << "  \"process_peak_rss_bytes\": " << process_peak_rss_bytes()
         << ",\n"
         << "  \"process_peak_rss_available\": "
         << (process_peak_rss_available() ? "true" : "false") << ",\n"
         << "  \"process_peak_rss_scope\": \"whole benchmark process, includes fixture creation\",\n"
         << "  \"correct\": true\n"
         << "}\n";
    EXPECT_TRUE(json.good());
}

TEST_F(ReaderRuntimeBenchmarkTest, ReadOnlyRuntimeProfileConfigured) {
    if (!enabled())
        GTEST_SKIP() << "set FAST_GDB_RUN_RUNTIME_BENCHMARKS=1";

    const char* path_value =
        std::getenv("FAST_GDB_RUNTIME_BENCHMARK_PATH");
    if (path_value == nullptr || *path_value == '\0')
        GTEST_SKIP() << "set FAST_GDB_RUNTIME_BENCHMARK_PATH";
    const std::string path(path_value);

    const char* layer_value =
        std::getenv("FAST_GDB_RUNTIME_BENCHMARK_LAYER");
    const std::string layer_name =
        layer_value == nullptr || *layer_value == '\0'
            ? "features" : layer_value;
    const char* field_value =
        std::getenv("FAST_GDB_RUNTIME_BENCHMARK_FIELD");
    const std::string value_field =
        field_value == nullptr || *field_value == '\0'
            ? "feat_id" : field_value;

    size_t trial_count = 5;
    if (const char* trial_value =
            std::getenv("FAST_GDB_RUNTIME_BENCHMARK_TRIALS")) {
        char* end = nullptr;
        const long parsed = std::strtol(trial_value, &end, 10);
        if (end != trial_value && *end == '\0' && parsed >= 1 && parsed <= 25)
            trial_count = static_cast<size_t>(parsed);
    }

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve(layer_name);
    ASSERT_TRUE(resolved.has_value());
    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());
    const int value_index = fast_field_index(engine.table(), value_field);
    ASSERT_GE(value_index, 0);

    const size_t feature_count = engine.table()->active_feature_count();
    ASSERT_GT(feature_count, 0U);
    ASSERT_LE(feature_count,
              static_cast<size_t>(std::numeric_limits<uint32_t>::max()));

    GDALDataset* gdal_dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr));
    ASSERT_NE(gdal_dataset, nullptr);
    OGRLayer* gdal_layer = gdal_dataset->GetLayerByName(layer_name.c_str());
    ASSERT_NE(gdal_layer, nullptr);
    const GIntBig gdal_feature_count = gdal_layer->GetFeatureCount();
    ASSERT_GE(gdal_feature_count, 0);
    ASSERT_EQ(feature_count, static_cast<size_t>(gdal_feature_count));
    ASSERT_GE(gdal_layer->GetLayerDefn()->GetFieldIndex(value_field.c_str()), 0);

    std::vector<uint32_t> random_fids;
    random_fids.reserve(kRandomReadCount);
    std::mt19937 rng(0x10A5U);
    std::uniform_int_distribution<uint32_t> fid_distribution(
        0, static_cast<uint32_t>(feature_count - 1));
    for (size_t index = 0; index < kRandomReadCount; ++index)
        random_fids.push_back(fid_distribution(rng));

    Distribution fast_random;
    Distribution gdal_random;
    Distribution fast_scan;
    Distribution gdal_scan;
    for (size_t sample_index = 0; sample_index < trial_count; ++sample_index) {
        Digest fast_random_digest;
        Digest gdal_random_digest;
        TimedSample fast_random_sample;
        TimedSample gdal_random_sample;
        if (sample_index % 2 == 0) {
            fast_random_sample = measure([&] {
                fast_random_digest = {};
                return read_fast_random(
                    engine, random_fids, value_index, fast_random_digest);
            });
            gdal_random_sample = measure([&] {
                gdal_random_digest = {};
                return read_gdal_random(
                    gdal_layer, random_fids, gdal_random_digest,
                    value_field.c_str());
            });
        } else {
            gdal_random_sample = measure([&] {
                gdal_random_digest = {};
                return read_gdal_random(
                    gdal_layer, random_fids, gdal_random_digest,
                    value_field.c_str());
            });
            fast_random_sample = measure([&] {
                fast_random_digest = {};
                return read_fast_random(
                    engine, random_fids, value_index, fast_random_digest);
            });
        }
        ASSERT_EQ(fast_random_digest, gdal_random_digest)
            << "random-read sample " << sample_index;
        fast_random.samples.push_back(fast_random_sample);
        gdal_random.samples.push_back(gdal_random_sample);

        Digest fast_scan_digest;
        Digest gdal_scan_digest;
        TimedSample fast_scan_sample;
        TimedSample gdal_scan_sample;
        if (sample_index % 2 == 0) {
            fast_scan_sample = measure([&] {
                fast_scan_digest = {};
                return scan_fast(engine, value_index, fast_scan_digest);
            });
            gdal_scan_sample = measure([&] {
                gdal_scan_digest = {};
                return scan_gdal(
                    gdal_layer, gdal_scan_digest, value_field.c_str());
            });
        } else {
            gdal_scan_sample = measure([&] {
                gdal_scan_digest = {};
                return scan_gdal(
                    gdal_layer, gdal_scan_digest, value_field.c_str());
            });
            fast_scan_sample = measure([&] {
                fast_scan_digest = {};
                return scan_fast(engine, value_index, fast_scan_digest);
            });
        }
        ASSERT_EQ(fast_scan_digest, gdal_scan_digest)
            << "full-scan sample " << sample_index;
        ASSERT_EQ(fast_scan_digest.feature_count, feature_count);
        fast_scan.samples.push_back(fast_scan_sample);
        gdal_scan.samples.push_back(gdal_scan_sample);
    }
    GDALClose(gdal_dataset);

    const char* configured_output =
        std::getenv("FAST_GDB_BENCHMARK_OUTPUT_DIR");
    const fs::path output_dir =
        configured_output != nullptr && *configured_output != '\0'
            ? fs::path(configured_output)
            : fs::temp_directory_path() / "fast-gdb-benchmark-results";
    fs::create_directories(output_dir);
    std::ofstream json(output_dir / "s4.5-runtime-configured.json");
    ASSERT_TRUE(json.good());
    json << std::fixed << std::setprecision(6)
         << "{\n"
         << "  \"dataset_path\": \"" << json_escape(path) << "\",\n"
         << "  \"layer\": \"" << json_escape(layer_name) << "\",\n"
         << "  \"value_field\": \"" << json_escape(value_field) << "\",\n"
         << "  \"feature_count\": " << feature_count << ",\n"
         << "  \"gdal_version\": \"" << GDALVersionInfo("RELEASE_NAME")
         << "\",\n"
         << "  \"sample_count\": " << trial_count << ",\n"
         << "  \"random_reads_per_sample\": " << kRandomReadCount << ",\n"
         << "  \"percentile_method\": \"nearest rank\",\n"
         << "  \"cache_semantics\": \"steady state; OS file cache not cleared\",\n";
    write_distribution(json, "fast_random_read", fast_random,
                       kRandomReadCount);
    write_distribution(json, "gdal_random_read", gdal_random,
                       kRandomReadCount);
    write_distribution(json, "fast_full_scan", fast_scan, feature_count);
    write_distribution(json, "gdal_full_scan", gdal_scan, feature_count);
    json << "  \"random_read_digest_equal\": true,\n"
         << "  \"full_scan_digest_equal\": true,\n"
         << "  \"process_peak_rss_bytes\": " << process_peak_rss_bytes()
         << ",\n"
         << "  \"process_peak_rss_scope\": \"whole benchmark process\"\n"
         << "}\n";
    ASSERT_TRUE(json.good());
}

TEST_F(ReaderRuntimeBenchmarkTest, ReadOnlyRuntimeProfileBackendIsolated) {
    if (!enabled())
        GTEST_SKIP() << "set FAST_GDB_RUN_RUNTIME_BENCHMARKS=1";

    const char* backend_value =
        std::getenv("FAST_GDB_RUNTIME_BENCHMARK_BACKEND");
    if (backend_value == nullptr || *backend_value == '\0')
        GTEST_SKIP() << "set FAST_GDB_RUNTIME_BENCHMARK_BACKEND to fast or gdal";
    const std::string backend(backend_value);
    ASSERT_TRUE(backend == "fast" || backend == "gdal") << backend;

    const char* path_value =
        std::getenv("FAST_GDB_RUNTIME_BENCHMARK_PATH");
    if (path_value == nullptr || *path_value == '\0')
        GTEST_SKIP() << "set FAST_GDB_RUNTIME_BENCHMARK_PATH";
    const std::string path(path_value);

    const char* layer_value =
        std::getenv("FAST_GDB_RUNTIME_BENCHMARK_LAYER");
    const std::string layer_name =
        layer_value == nullptr || *layer_value == '\0'
            ? "features" : layer_value;
    const char* field_value =
        std::getenv("FAST_GDB_RUNTIME_BENCHMARK_FIELD");
    const std::string value_field =
        field_value == nullptr || *field_value == '\0'
            ? "feat_id" : field_value;

    size_t trial_count = 5;
    if (const char* trial_value =
            std::getenv("FAST_GDB_RUNTIME_BENCHMARK_TRIALS")) {
        char* end = nullptr;
        const long parsed = std::strtol(trial_value, &end, 10);
        if (end != trial_value && *end == '\0' && parsed >= 1 && parsed <= 25)
            trial_count = static_cast<size_t>(parsed);
    }

    size_t feature_count = 0;
    Distribution random_reads;
    Distribution full_scans;
    Digest random_digest_reference;
    Digest scan_digest_reference;
    bool has_reference_digest = false;

    if (backend == "fast") {
        GdbCatalog catalog;
        ASSERT_TRUE(catalog.scan(path));
        CatalogResolver resolver(catalog);
        ASSERT_TRUE(resolver.load());
        const auto resolved = resolver.resolve(layer_name);
        ASSERT_TRUE(resolved.has_value());

        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open());
        const int value_index = fast_field_index(engine.table(), value_field);
        ASSERT_GE(value_index, 0);
        feature_count = engine.table()->active_feature_count();

        ASSERT_GT(feature_count, 0U);
        ASSERT_LE(feature_count, static_cast<size_t>(
                                     std::numeric_limits<uint32_t>::max()));
        std::vector<uint32_t> random_fids;
        random_fids.reserve(kRandomReadCount);
        std::mt19937 rng(0x10A5U);
        std::uniform_int_distribution<uint32_t> fid_distribution(
            0, static_cast<uint32_t>(feature_count - 1));
        for (size_t index = 0; index < kRandomReadCount; ++index)
            random_fids.push_back(fid_distribution(rng));

        for (size_t sample_index = 0; sample_index < trial_count;
             ++sample_index) {
            Digest random_digest;
            const TimedSample random_sample = measure([&] {
                random_digest = {};
                return read_fast_random(engine, random_fids, value_index,
                                        random_digest);
            });
            if (has_reference_digest) {
                ASSERT_EQ(random_digest, random_digest_reference)
                    << "random-read sample " << sample_index;
            } else {
                random_digest_reference = random_digest;
            }
            random_reads.samples.push_back(random_sample);

            Digest scan_digest;
            const TimedSample scan_sample = measure([&] {
                scan_digest = {};
                return scan_fast(engine, value_index, scan_digest);
            });
            ASSERT_EQ(scan_digest.feature_count, feature_count);
            if (has_reference_digest) {
                ASSERT_EQ(scan_digest, scan_digest_reference)
                    << "full-scan sample " << sample_index;
            } else {
                scan_digest_reference = scan_digest;
                has_reference_digest = true;
            }
            full_scans.samples.push_back(scan_sample);
        }
    } else {
        std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(
            static_cast<GDALDataset*>(GDALOpenEx(
                path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                nullptr, nullptr, nullptr)), GDALClose);
        ASSERT_NE(dataset, nullptr);
        OGRLayer* layer = dataset->GetLayerByName(layer_name.c_str());
        ASSERT_NE(layer, nullptr);
        const GIntBig gdal_count = layer->GetFeatureCount();
        ASSERT_GT(gdal_count, 0);
        feature_count = static_cast<size_t>(gdal_count);
        ASSERT_GE(layer->GetLayerDefn()->GetFieldIndex(value_field.c_str()), 0);

        ASSERT_LE(feature_count,
                  static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
        std::vector<uint32_t> random_fids;
        random_fids.reserve(kRandomReadCount);
        std::mt19937 rng(0x10A5U);
        std::uniform_int_distribution<uint32_t> fid_distribution(
            0, static_cast<uint32_t>(feature_count - 1));
        for (size_t index = 0; index < kRandomReadCount; ++index)
            random_fids.push_back(fid_distribution(rng));

        for (size_t sample_index = 0; sample_index < trial_count;
             ++sample_index) {
            Digest random_digest;
            const TimedSample random_sample = measure([&] {
                random_digest = {};
                return read_gdal_random(layer, random_fids, random_digest,
                                        value_field.c_str());
            });
            if (has_reference_digest) {
                ASSERT_EQ(random_digest, random_digest_reference)
                    << "random-read sample " << sample_index;
            } else {
                random_digest_reference = random_digest;
            }
            random_reads.samples.push_back(random_sample);

            Digest scan_digest;
            const TimedSample scan_sample = measure([&] {
                scan_digest = {};
                return scan_gdal(layer, scan_digest, value_field.c_str());
            });
            ASSERT_EQ(scan_digest.feature_count, feature_count);
            if (has_reference_digest) {
                ASSERT_EQ(scan_digest, scan_digest_reference)
                    << "full-scan sample " << sample_index;
            } else {
                scan_digest_reference = scan_digest;
                has_reference_digest = true;
            }
            full_scans.samples.push_back(scan_sample);
        }
    }

    ASSERT_GT(feature_count, 0U);
    ASSERT_LE(feature_count,
              static_cast<size_t>(std::numeric_limits<uint32_t>::max()));

    ASSERT_EQ(random_digest_reference.feature_count, kRandomReadCount);
    ASSERT_EQ(scan_digest_reference.feature_count, feature_count);
    const char* configured_output =
        std::getenv("FAST_GDB_BENCHMARK_OUTPUT_DIR");
    const fs::path output_dir =
        configured_output != nullptr && *configured_output != '\0'
            ? fs::path(configured_output)
            : fs::temp_directory_path() / "fast-gdb-benchmark-results";
    fs::create_directories(output_dir);
    std::ofstream json(output_dir / ("s4.5-runtime-isolated-" + backend +
                                    ".json"));
    ASSERT_TRUE(json.good());
    json << std::fixed << std::setprecision(6)
         << "{\n"
         << "  \"backend\": \"" << backend << "\",\n"
         << "  \"dataset_path\": \"" << json_escape(path) << "\",\n"
         << "  \"layer\": \"" << json_escape(layer_name) << "\",\n"
         << "  \"value_field\": \"" << json_escape(value_field) << "\",\n"
         << "  \"feature_count\": " << feature_count << ",\n"
         << "  \"gdal_version\": \"" << GDALVersionInfo("RELEASE_NAME")
         << "\",\n"
         << "  \"sample_count\": " << trial_count << ",\n"
         << "  \"random_reads_per_sample\": " << kRandomReadCount << ",\n"
         << "  \"cache_semantics\": \"steady state; OS file cache not cleared\",\n"
         << "  \"percentile_method\": \"nearest rank\",\n";
    write_distribution(json, "random_read", random_reads, kRandomReadCount);
    write_distribution(json, "full_scan", full_scans, feature_count);
    json << "  \"random_digest_hash\": "
         << random_digest_reference.hash << ",\n"
         << "  \"random_digest_count\": "
         << random_digest_reference.feature_count << ",\n"
         << "  \"full_scan_digest_hash\": " << scan_digest_reference.hash
         << ",\n"
         << "  \"full_scan_digest_count\": "
         << scan_digest_reference.feature_count << ",\n"
         << "  \"process_peak_rss_bytes\": " << process_peak_rss_bytes()
         << ",\n"
         << "  \"process_peak_rss_available\": "
         << (process_peak_rss_available() ? "true" : "false") << ",\n"
         << "  \"process_peak_rss_scope\": \"one data backend opened; test process still links both implementations\",\n"
         << "  \"correct\": true\n"
         << "}\n";
    EXPECT_TRUE(json.good());
}
