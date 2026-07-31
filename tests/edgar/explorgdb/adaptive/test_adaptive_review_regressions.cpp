// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include "adaptive_backends.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

using namespace explorgdb;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

BackendReadResult successful_read(const QueryRequest&) {
    QueryResult result;
    result.execution_path = "review-fixture";
    return BackendReadResult::success(std::move(result));
}

std::atomic<uint64_t> g_review_sequence{0};

fs::path unique_review_directory() {
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return fs::temp_directory_path() /
           ("fast_gdb_adaptive_review_" + std::to_string(ticks) + "_" +
            std::to_string(g_review_sequence.fetch_add(1)));
}

}  // namespace

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReviewRegressionTest,
     ThrowingFastCursorCloseKeepsWriterFailClosed) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "throwing-fast-close-review.gdb";

    AdaptiveReadSession session(
        coordinator, path,
        successful_read,
        successful_read,
        [](const QueryRequest&) {
            BackendCursor cursor;
            cursor.next = [](QueryFeature&, std::string&) { return false; };
            cursor.close = [] {
                throw std::runtime_error("simulated fast close failure");
            };
            return cursor;
        });

    QueryRequest request;
    auto cursor = session.open_cursor(request);
    ASSERT_EQ(cursor.status(), AdaptiveReadStatus::Ok);

    QueryFeature feature;
    EXPECT_FALSE(cursor.next(feature));
    EXPECT_EQ(cursor.status(), AdaptiveReadStatus::FastBackendReadFailed);
    EXPECT_NE(cursor.error().find("simulated fast close failure"),
              std::string::npos);

    const auto state = coordinator.state(path);
    EXPECT_EQ(state.fast_reader_count, 1U)
        << "cleanup failure must retain the Writer barrier";

    auto writer = coordinator.prepare_external_update(path, 1ms);
    EXPECT_EQ(writer.status, CoordinationStatus::ReadersActive);
    EXPECT_FALSE(coordinator.state(path).writer_pending);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReviewRegressionTest,
     ThrowingGdalCursorCloseIsReportedAsUnverifiedReadFailure) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "throwing-gdal-close-review.gdb";
    auto prepared = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);

    AdaptiveReadSession session(
        coordinator, path,
        successful_read,
        successful_read,
        {},
        [](const QueryRequest&) {
            BackendCursor cursor;
            cursor.next = [](QueryFeature&, std::string&) { return false; };
            cursor.close = [] {
                throw std::runtime_error("simulated GDAL close failure");
            };
            return cursor;
        });

    QueryRequest request;
    auto cursor = session.open_cursor(
        request, ConcurrentReadPolicy::GdalUnverified);
    ASSERT_EQ(cursor.status(), AdaptiveReadStatus::Ok);

    QueryFeature feature;
    EXPECT_FALSE(cursor.next(feature));
    EXPECT_EQ(cursor.status(), AdaptiveReadStatus::GdalReadFailed);
    EXPECT_EQ(cursor.consistency(),
              AdaptiveReadConsistency::UnverifiedConcurrentRead);
    EXPECT_NE(cursor.error().find("simulated GDAL close failure"),
              std::string::npos);

    EXPECT_EQ(prepared.token.notify_update_closed(true),
              CoordinationStatus::Ok);
}

class AdaptiveGdalContractRegressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        GDALAllRegister();
        driver_ = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
        ASSERT_NE(driver_, nullptr);
        ASSERT_STREQ(driver_->GetMetadataItem(GDAL_DCAP_CREATE), "YES");

        directory_ = unique_review_directory();
        gdb_path_ = directory_ / "review.gdb";
        fs::create_directories(directory_);

        GDALDataset* dataset = driver_->Create(
            gdb_path_.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        ASSERT_NE(dataset, nullptr);

        OGRLayer* layer = dataset->CreateLayer(
            "review_points", nullptr, wkbPoint, nullptr);
        ASSERT_NE(layer, nullptr);

        OGRFieldDefn value_field("value", OFTInteger);
        OGRFieldDefn name_field("name", OFTString);
        name_field.SetWidth(32);
        ASSERT_EQ(layer->CreateField(&value_field), OGRERR_NONE);
        ASSERT_EQ(layer->CreateField(&name_field), OGRERR_NONE);

        for (int index = 0; index < 2; ++index) {
            OGRFeature* feature = OGRFeature::CreateFeature(
                layer->GetLayerDefn());
            ASSERT_NE(feature, nullptr);
            feature->SetField("value", index + 1);
            if (index == 0) feature->SetField("name", "alpha");
            OGRPoint point(index + 1.0, index + 1.0);
            ASSERT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
            ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
            OGRFeature::DestroyFeature(feature);
        }
        GDALClose(dataset);

        loaded_ = load_adaptive_layer_binding(
            coordinator_, gdb_path_.string(), "review_points");
        ASSERT_TRUE(loaded_.ok) << loaded_.error;
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(directory_, ignored);
    }

    InProcessGdbCoordinator coordinator_;
    GDALDriver* driver_ = nullptr;
    fs::path directory_;
    fs::path gdb_path_;
    AdaptiveLayerBindingResult loaded_;
};

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(AdaptiveGdalContractRegressionTest,
       EmptyWhereAndSpatialWhereNeverBecomeFullScans) {
    GdalOpenFileGdbReadBackend backend(
        gdb_path_.string(), loaded_.binding);

    QueryRequest where;
    where.kind = QueryKind::WhereClause;
    where.where_clause = " \t\n";
    const auto where_result = backend.read(where);
    ASSERT_TRUE(where_result.ok) << where_result.error;
    EXPECT_TRUE(where_result.result.matched_fids.empty());
    EXPECT_EQ(where_result.result.fallback_reason, "empty where clause");

    QueryRequest combined;
    combined.kind = QueryKind::SpatialWhere;
    combined.xmin = 0.0;
    combined.ymin = 0.0;
    combined.xmax = 10.0;
    combined.ymax = 10.0;
    combined.where_clause = "";
    const auto combined_result = backend.read(combined);
    ASSERT_TRUE(combined_result.ok) << combined_result.error;
    EXPECT_TRUE(combined_result.result.matched_fids.empty());
    EXPECT_EQ(combined_result.result.fallback_reason, "empty where clause");

    BackendCursor cursor = backend.open_cursor(where);
    QueryFeature feature;
    std::string error;
    EXPECT_FALSE(cursor.next(feature, error));
    EXPECT_TRUE(error.empty());
    cursor.close();
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(AdaptiveGdalContractRegressionTest,
       NullInNonNullableBindingFailsQueryAndCursor) {
    AdaptiveLayerBinding strict = loaded_.binding;
    const auto found = std::find_if(
        strict.fields.begin(), strict.fields.end(),
        [](const FieldDescriptor& field) { return field.name == "name"; });
    ASSERT_NE(found, strict.fields.end());
    found->flag = static_cast<uint8_t>(found->flag & ~1U);

    GdalOpenFileGdbReadBackend backend(
        gdb_path_.string(), std::move(strict));

    QueryRequest by_fid;
    by_fid.kind = QueryKind::ReadByFid;
    by_fid.fid = 1;
    const auto result = backend.read(by_fid);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.failure, BackendFailureKind::Read);
    EXPECT_NE(result.error.find("non-nullable field: name"),
              std::string::npos);

    BackendCursor cursor = backend.open_cursor(by_fid);
    QueryFeature feature;
    std::string error;
    EXPECT_FALSE(cursor.next(feature, error));
    EXPECT_NE(error.find("non-nullable field: name"), std::string::npos);
    cursor.close();
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(AdaptiveGdalContractRegressionTest,
       FilterSetupFailureIsClassifiedAsGdalReadFailure) {
    AdaptiveReadSession session = make_adaptive_read_session(
        coordinator_, gdb_path_.string(), loaded_.binding);

    auto prepared = coordinator_.prepare_external_update(
        gdb_path_.string(), 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);

    QueryRequest request;
    request.kind = QueryKind::AttributeDouble;
    request.index_name = "missing_index";
    request.double_value = 1.0;

    auto cursor = session.open_cursor(
        request, ConcurrentReadPolicy::GdalUnverified);
    ASSERT_EQ(cursor.status(), AdaptiveReadStatus::Ok);

    QueryFeature feature;
    EXPECT_FALSE(cursor.next(feature));
    EXPECT_EQ(cursor.status(), AdaptiveReadStatus::GdalReadFailed);
    EXPECT_EQ(cursor.consistency(),
              AdaptiveReadConsistency::UnverifiedConcurrentRead);
    EXPECT_NE(cursor.error().find("not bound to an OGR field"),
              std::string::npos);

    EXPECT_EQ(prepared.token.notify_update_closed(true),
              CoordinationStatus::Ok);
}
