// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"
#include "test_fixture.h"

using namespace explorgdb;

namespace {

class SpatialQueryAdaptiveTest : public GdbTutorialFixture {
protected:
    std::string create_adaptive_point_gdb(size_t feature_count) {
        return create_point_gdb(feature_count, false);
    }

    std::string create_wide_point_gdb(size_t feature_count) {
        return create_point_gdb(feature_count, true);
    }

private:
    std::string create_point_gdb(size_t feature_count, bool include_wide_blob) {
        const auto suffix = include_wide_blob ? "wide" : "normal";
        const auto path = (std::filesystem::temp_directory_path() /
                           (std::string("fast_gdb_spatial_adaptive_") +
                            suffix + ".gdb")).string();
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
        if (include_wide_blob) {
            OGRFieldDefn payload_field("payload", OFTBinary);
            if (layer->CreateField(&payload_field) != OGRERR_NONE) {
                GDALClose(dataset);
                return {};
            }
        }

        const std::vector<GByte> small_payload(32U, 0x2aU);
        const std::vector<GByte> wide_payload(2U * 1024U * 1024U, 0x5aU);
        for (size_t index = 0; index < feature_count; ++index) {
            OGRFeature* feature =
                OGRFeature::CreateFeature(layer->GetLayerDefn());
            if (feature == nullptr) {
                GDALClose(dataset);
                return {};
            }
            feature->SetField("value", static_cast<int>(index));
            if (include_wide_blob) {
                const auto& payload = index == feature_count / 2U
                    ? wide_payload : small_payload;
                const int payload_idx = layer->GetLayerDefn()->GetFieldIndex("payload");
                feature->SetField(
                    payload_idx,
                    static_cast<int>(payload.size()),
                    payload.data());
            }
            OGRPoint point(
                static_cast<double>(index % 100U),
                static_cast<double>(index / 100U));
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

#ifdef _WIN32
class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const char* value)
        : name_(name) {
        const char* previous = std::getenv(name);
        if (previous != nullptr) {
            had_previous_ = true;
            previous_ = previous;
        }
        _putenv_s(name, value);
    }

    ~ScopedEnvironment() {
        _putenv_s(name_.c_str(), had_previous_ ? previous_.c_str() : "");
    }

private:
    std::string name_;
    std::string previous_;
    bool had_previous_ = false;
};
#endif

} // namespace

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialQueryAdaptiveTest,
       GeometryOnlyScannerMatchesCanonicalBlobLocator) {
    constexpr size_t kFeatureCount = 64;
    const std::string path = create_adaptive_point_gdb(kFeatureCount);
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    auto engine = open_engine(path, catalog);
    ASSERT_NE(engine, nullptr);
    ASSERT_NE(engine->table(), nullptr);

    std::vector<uint32_t> scanned_fids;
    const uint64_t scanned = engine->table()->scan_geometry_blobs(
        [&](uint32_t fid, const uint8_t* blob, size_t size, bool is_null) {
            EXPECT_FALSE(is_null);
            if (is_null || blob == nullptr || size == 0) {
                ADD_FAILURE() << "expected a non-null geometry blob";
                return false;
            }

            const uint8_t* canonical = nullptr;
            size_t canonical_size = 0;
            if (!engine->table()->peek_geometry_blob(
                    fid, canonical, canonical_size)) {
                ADD_FAILURE() << "canonical geometry lookup failed";
                return false;
            }
            EXPECT_EQ(size, canonical_size);
            EXPECT_EQ(std::memcmp(blob, canonical, size), 0);
            scanned_fids.push_back(fid);
            return true;
        });

    EXPECT_EQ(scanned, kFeatureCount);
    ASSERT_EQ(scanned_fids.size(), kFeatureCount);
    EXPECT_TRUE(std::is_sorted(scanned_fids.begin(), scanned_fids.end()));
    EXPECT_EQ(scanned_fids.front(), 0u);
    EXPECT_EQ(scanned_fids.back(), kFeatureCount - 1);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialQueryAdaptiveTest,
       HighDensityQueryBypassesSpatialIndexAndPreservesFids) {
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
    EXPECT_TRUE(result.spatial_metrics.spx_bypassed);
    EXPECT_TRUE(result.spatial_metrics.geometry_only_scan);
    EXPECT_GT(result.spatial_metrics.estimated_coverage, 0.90);
    EXPECT_EQ(result.spatial_metrics.candidate_count, kFeatureCount);
    EXPECT_EQ(result.spatial_metrics.bbox_contained, kFeatureCount);
    EXPECT_EQ(result.spatial_metrics.exact_tested, 0u);
    EXPECT_EQ(result.spatial_metrics.invalid_geometries, 0u);
    EXPECT_EQ(result.execution_path, "bbox:model:sequential-planned");

    const auto legacy = engine->query_bbox(
        request.xmin, request.ymin, request.xmax, request.ymax);
    EXPECT_EQ(legacy, result.matched_fids);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialQueryAdaptiveTest,
       BoundaryPointsStillProduceCorrectResult) {
    constexpr size_t kFeatureCount = 1200;
    const std::string path = create_adaptive_point_gdb(kFeatureCount);
    ASSERT_FALSE(path.empty());

    GdbCatalog catalog;
    auto engine = open_engine(path, catalog);
    ASSERT_NE(engine, nullptr);

    const QueryResult result = engine->query_bbox_unified(
        0.0, 0.0, 0.0, 0.0);
    ASSERT_EQ(result.matched_fids.size(), 1u);
    EXPECT_EQ(result.matched_fids.front(), 0u);
    EXPECT_FALSE(result.spatial_metrics.spx_bypassed);
    EXPECT_FALSE(result.spatial_metrics.geometry_only_scan);
    EXPECT_LE(result.spatial_metrics.bbox_contained,
              result.spatial_metrics.candidate_count);
    EXPECT_EQ(result.spatial_metrics.invalid_geometries, 0u);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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
    EXPECT_FALSE(result.spatial_metrics.spx_bypassed);
    EXPECT_FALSE(result.spatial_metrics.geometry_only_scan);
    EXPECT_LT(result.spatial_metrics.estimated_coverage, 0.10);
    EXPECT_LT(result.matched_fids.size(), kFeatureCount / 10);
    EXPECT_LT(result.spatial_metrics.candidate_ratio, 0.5);
    EXPECT_GT(result.spatial_metrics.bbox_contained, 0u);
    EXPECT_TRUE(result.execution_path == "bbox:model:spx-candidates" ||
                result.execution_path == "bbox:model:spx-candidates-batched");
    EXPECT_EQ(result.spatial_metrics.invalid_geometries, 0u);
}

#ifdef _WIN32
/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialQueryAdaptiveTest,
       WindowedMmapPreservesDenseAndSparseQueryResults) {
    ScopedEnvironment mmap_enabled("FAST_GDB_WINDOWS_MMAP", "1");
    ScopedEnvironment force_windowed("FAST_GDB_FORCE_WINDOWED_MMAP", "1");
    ScopedEnvironment trace("FAST_GDB_WINDOWS_IO_TRACE", "1");

    constexpr size_t kFeatureCount = 1200;
    const std::string path = create_adaptive_point_gdb(kFeatureCount);
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    auto engine = open_engine(path, catalog);
    ASSERT_NE(engine, nullptr);

    const QueryResult dense = engine->query_bbox_unified(
        -1.0, -1.0, 100.0, 20.0);
    ASSERT_EQ(dense.matched_fids.size(), kFeatureCount);
    EXPECT_EQ(dense.spatial_metrics.invalid_geometries, 0u);
    EXPECT_EQ(dense.execution_path, "bbox:model:sequential-planned");

    const QueryResult sparse = engine->query_bbox_unified(
        -0.1, -0.1, 1.1, 1.1);
    ASSERT_FALSE(sparse.matched_fids.empty());
    EXPECT_LT(sparse.matched_fids.size(), kFeatureCount / 10U);
    EXPECT_EQ(sparse.spatial_metrics.invalid_geometries, 0u);
    EXPECT_EQ(sparse.execution_path, "bbox:model:spx-candidates-batched");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialQueryAdaptiveTest,
       ForcedAsyncLaunchFailureFallsBackToSynchronousBatches) {
    ScopedEnvironment mmap_disabled("FAST_GDB_WINDOWS_MMAP", "0");
    ScopedEnvironment async_enabled("FAST_GDB_WINDOWS_ASYNC_IO", "1");
    ScopedEnvironment async_depth("FAST_GDB_WINDOWS_ASYNC_DEPTH", "4");
    ScopedEnvironment force_failure(
        "FAST_GDB_FORCE_ASYNC_LAUNCH_FAILURE", "1");
    ScopedEnvironment trace("FAST_GDB_WINDOWS_IO_TRACE", "1");

    constexpr size_t kFeatureCount = 5000;
    const std::string path = create_adaptive_point_gdb(kFeatureCount);
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    auto engine = open_engine(path, catalog);
    ASSERT_NE(engine, nullptr);

    testing::internal::CaptureStderr();
    const QueryResult result = engine->query_bbox_unified(
        -1.0, -1.0, 100.0, 60.0);
    const std::string trace_output = testing::internal::GetCapturedStderr();

    ASSERT_EQ(result.matched_fids.size(), kFeatureCount);
    EXPECT_EQ(result.spatial_metrics.invalid_geometries, 0u);
    EXPECT_NE(trace_output.find("mode=fallback-overlapped"),
              std::string::npos);
    EXPECT_NE(trace_output.find("overlapped_batches=0"),
              std::string::npos);
    EXPECT_NE(trace_output.find("failed=false"), std::string::npos);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(SpatialQueryAdaptiveTest,
       OneWideRecordDoesNotInflateEveryFallbackBatch) {
    ScopedEnvironment mmap_disabled("FAST_GDB_WINDOWS_MMAP", "0");
    ScopedEnvironment async_disabled("FAST_GDB_WINDOWS_ASYNC_IO", "0");
    ScopedEnvironment one_mib_batches("FAST_GDB_WINDOWS_BATCH_MB", "1");
    ScopedEnvironment trace("FAST_GDB_WINDOWS_IO_TRACE", "1");

    constexpr size_t kFeatureCount = 64;
    const std::string path = create_wide_point_gdb(kFeatureCount);
    ASSERT_FALSE(path.empty());
    GdbCatalog catalog;
    auto engine = open_engine(path, catalog);
    ASSERT_NE(engine, nullptr);
    ASSERT_NE(engine->table(), nullptr);

    testing::internal::CaptureStderr();
    uint64_t callback_count = 0;
    const uint64_t scanned = engine->table()->scan_geometry_blobs(
        [&](uint32_t, const uint8_t* blob, size_t size, bool is_null) {
            EXPECT_FALSE(is_null);
            EXPECT_NE(blob, nullptr);
            EXPECT_GT(size, 0u);
            ++callback_count;
            return true;
        });
    const std::string trace_output = testing::internal::GetCapturedStderr();

    EXPECT_EQ(scanned, kFeatureCount);
    EXPECT_EQ(callback_count, kFeatureCount);
    std::smatch match;
    const std::regex stats(
        "batch_reads=([0-9]+) bytes=([0-9]+) exact_reads=([0-9]+) "
        "exact_bytes=([0-9]+)");
    ASSERT_TRUE(std::regex_search(trace_output, match, stats));
    const unsigned long long batch_bytes = std::stoull(match[2].str());
    const unsigned long long exact_reads = std::stoull(match[3].str());
    EXPECT_LT(batch_bytes, 8ULL * 1024ULL * 1024ULL);
    EXPECT_GE(exact_reads, 1ULL);
}
#endif
