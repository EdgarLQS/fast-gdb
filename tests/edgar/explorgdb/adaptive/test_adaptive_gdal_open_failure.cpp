#include <gtest/gtest.h>

#include "adaptive_backends.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

using namespace explorgdb;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

std::atomic<uint64_t> g_open_failure_sequence{0};

fs::path unique_open_failure_directory() {
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return fs::temp_directory_path() /
           ("fast_gdb_adaptive_open_failure_" + std::to_string(ticks) + "_" +
            std::to_string(g_open_failure_sequence.fetch_add(1)));
}

}  // namespace

TEST(AdaptiveGdalOpenFailureRegressionTest,
     MissingDatasetRemainsGdalOpenFailed) {
    GDALAllRegister();
    GDALDriver* driver =
        GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    ASSERT_NE(driver, nullptr);
    ASSERT_STREQ(driver->GetMetadataItem(GDAL_DCAP_CREATE), "YES");

    const fs::path directory = unique_open_failure_directory();
    const fs::path gdb_path = directory / "missing-after-binding.gdb";
    fs::create_directories(directory);

    GDALDataset* dataset = driver->Create(
        gdb_path.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(dataset, nullptr);
    OGRLayer* layer = dataset->CreateLayer(
        "open_failure_points", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
    ASSERT_NE(feature, nullptr);
    OGRPoint point(1.0, 1.0);
    ASSERT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
    ASSERT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
    OGRFeature::DestroyFeature(feature);
    GDALClose(dataset);

    InProcessGdbCoordinator coordinator;
    const auto loaded = load_adaptive_layer_binding(
        coordinator, gdb_path.string(), "open_failure_points");
    ASSERT_TRUE(loaded.ok) << loaded.error;
    AdaptiveReadSession session = make_adaptive_read_session(
        coordinator, gdb_path.string(), loaded.binding);

    auto prepared = coordinator.prepare_external_update(
        gdb_path.string(), 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);

    std::error_code remove_error;
    fs::remove_all(gdb_path, remove_error);
    ASSERT_FALSE(remove_error) << remove_error.message();

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    auto cursor = session.open_cursor(
        request, ConcurrentReadPolicy::GdalUnverified);
    EXPECT_EQ(cursor.status(), AdaptiveReadStatus::GdalOpenFailed);
    EXPECT_EQ(cursor.backend(), AdaptiveReadBackend::GdalOpenFileGDB);
    EXPECT_EQ(cursor.consistency(),
              AdaptiveReadConsistency::UnverifiedConcurrentRead);
    EXPECT_FALSE(cursor.error().empty());

    EXPECT_EQ(prepared.token.notify_update_closed(true),
              CoordinationStatus::Ok);

    std::error_code cleanup_error;
    fs::remove_all(directory, cleanup_error);
}
