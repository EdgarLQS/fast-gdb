#include "unified.h"

#include "adaptive_reader.h"
#include "test_paths.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>

namespace {

using namespace fast_gdb::unified;

std::string fixture_path() {
    return explorgdb_test_paths::test_data_path(
        "test_data/benchmark/wide_50_gdal.gdb").string();
}

TEST(UnifiedFastFacadeTest, OpensDatasetFromGdalStylePath) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset) << dataset.error().message;

    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names) << names.error().message;
    ASSERT_FALSE(names.value().empty());
    EXPECT_EQ(dataset.value().backend_report().selected, Backend::FastGdb);
}

TEST(UnifiedFastFacadeTest, FreezesSchemaAndStreamsOwnedFeatures) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);

    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer) << layer.error().message;
    ASSERT_FALSE(layer.value().schema().fields.empty());

    Query query;
    query.limit = 2;
    query.order = ResultOrder::FidAscending;
    auto cursor_result = layer.value().open_cursor(query);
    ASSERT_TRUE(cursor_result) << cursor_result.error().message;
    auto cursor = std::move(cursor_result).value();

    auto first = cursor.next();
    ASSERT_TRUE(first) << first.error().message;
    if (first.value()) {
        EXPECT_GT(first.value()->fid, 0);
        EXPECT_EQ(first.value()->fields.size(),
                  layer.value().schema().fields.size());
    }
}

TEST(UnifiedFastFacadeTest, ReadAllPublishesWithinLimits) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset);
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer);

    Query query;
    query.limit = 3;
    auto batch = layer.value().read_all(query);
    ASSERT_TRUE(batch) << batch.error().message;
    EXPECT_LE(batch.value().features.size(), 3U);
    EXPECT_GT(batch.value().materialized_bytes, 0U);

    ReadAllOptions tiny;
    tiny.max_materialized_bytes = sizeof(Feature) - 1;
    auto limited = layer.value().read_all(query, tiny);
    EXPECT_FALSE(limited);
    EXPECT_EQ(limited.error().code, ErrorCode::ResultLimitExceeded);
}

TEST(UnifiedFastFacadeTest, RejectsInvalidPublicFid) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset);
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer);

    auto feature = layer.value().read_by_fid(-1);
    EXPECT_FALSE(feature);
    EXPECT_EQ(feature.error().code, ErrorCode::Unsupported);
}

TEST(UnifiedFastFacadeTest, DeadlineFailsBeforeCursorPublication) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset);
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer);

    Query query;
    query.deadline = std::chrono::steady_clock::now();
    auto cursor = layer.value().open_cursor(query);
    EXPECT_FALSE(cursor);
    EXPECT_EQ(cursor.error().code, ErrorCode::DeadlineExceeded);
}

TEST(UnifiedFastFacadeTest, CancellationBeforeFirstPublicationIsAtomic) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset);
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer);

    bool cancelled = false;
    Query query;
    query.cancel_requested = [&cancelled] { return cancelled; };
    auto cursor = layer.value().open_cursor(query);
    ASSERT_TRUE(cursor);
    cancelled = true;
    auto first = cursor.value().next();
    EXPECT_FALSE(first);
    EXPECT_EQ(first.error().code, ErrorCode::Cancelled);
}

TEST(UnifiedFastFacadeTest, ExposesFeatureDatasetGroups) {
    const auto source = explorgdb_test_paths::test_data_path(
        "test_data/gdb/acceptance_metadata.gdb").string();
    auto dataset = Dataset::open(source);
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto root = dataset.value().root_group();
    ASSERT_TRUE(root);
    auto groups = root.value().groups();
    ASSERT_TRUE(groups);
    auto transport = std::find_if(
        groups.value().begin(), groups.value().end(),
        [](const GroupInfo& group) { return group.name == "TransportFD"; });
    ASSERT_NE(transport, groups.value().end());

    auto opened = root.value().open_group("TransportFD");
    ASSERT_TRUE(opened) << opened.error().message;
    auto layers = opened.value().layers();
    ASSERT_TRUE(layers);
    EXPECT_NE(std::find_if(
        layers.value().begin(), layers.value().end(),
        [](const LayerInfo& layer) { return layer.name == "roads"; }),
        layers.value().end());

    auto roads = dataset.value().open_layer_by_path("/TransportFD/roads");
    ASSERT_TRUE(roads) << roads.error().message;
}

TEST(UnifiedFastFacadeTest, NativeExtensionsAreFastOnlyAndBounded) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset);
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer);
    auto extensions = layer.value().fast_extensions();
    ASSERT_TRUE(extensions) << extensions.error().message;

    NativeReadLimits tiny;
    tiny.max_raw_bytes = 1;
    auto limited = extensions.value().read_native_by_fid(1, tiny);
    EXPECT_FALSE(limited);
    EXPECT_EQ(limited.error().code, ErrorCode::ResultLimitExceeded);

    auto native = extensions.value().read_native_by_fid(1);
    ASSERT_TRUE(native) << native.error().message;
    EXPECT_EQ(native.value().row_slot, 0U);
    EXPECT_FALSE(native.value().raw_record.empty());
    EXPECT_FALSE(native.value().descriptors.empty());
}

TEST(UnifiedFastFacadeTest, ReportsCapabilitiesAndQueryPlan) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset);
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer);

    auto capabilities = layer.value().capabilities();
    ASSERT_TRUE(capabilities);
    EXPECT_TRUE(capabilities.value().streaming);
    EXPECT_TRUE(capabilities.value().native_extensions);

    Query query;
    query.order = ResultOrder::FidAscending;
    auto plan = layer.value().explain(query);
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan.value().backend, Backend::FastGdb);
    EXPECT_TRUE(plan.value().materializes_fids);
}

TEST(UnifiedFastFacadeTest, SharesCoordinatorAndFailsClosedDuringWriter) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset);
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer);

    explorgdb::InProcessGdbCoordinator coordinator;
    auto update = coordinator.prepare_external_update(
        fixture_path(), std::chrono::milliseconds(0));
    ASSERT_TRUE(update);
    ASSERT_EQ(update.token.notify_update_opened(),
              explorgdb::CoordinationStatus::Ok);

    auto cursor = layer.value().open_cursor();
    EXPECT_FALSE(cursor);
    EXPECT_EQ(cursor.error().code, ErrorCode::SourceBusy);
    EXPECT_EQ(update.token.notify_update_closed(true),
              explorgdb::CoordinationStatus::Ok);
}

#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
TEST(UnifiedGdalFacadeTest, ExplicitConcurrentPolicyUsesUnverifiedGdal) {
    OpenOptions options;
    options.concurrent_read = ConcurrentReadPolicy::GdalUnverified;
    auto dataset = Dataset::open(fixture_path(), options);
    ASSERT_TRUE(dataset);
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer);

    explorgdb::InProcessGdbCoordinator coordinator;
    auto update = coordinator.prepare_external_update(
        fixture_path(), std::chrono::milliseconds(0));
    ASSERT_TRUE(update);
    ASSERT_EQ(update.token.notify_update_opened(),
              explorgdb::CoordinationStatus::Ok);

    auto cursor = layer.value().open_cursor();
    ASSERT_TRUE(cursor) << cursor.error().message;
    EXPECT_EQ(cursor.value().backend_report().selected,
              Backend::GdalOpenFileGDB);
    EXPECT_EQ(update.token.notify_update_closed(true),
              explorgdb::CoordinationStatus::Ok);
}

TEST(UnifiedGdalFacadeTest, AutoFallsBackForWhitelistedQueryGap) {
    const auto source = explorgdb_test_paths::test_data_path(
        "test_data/gdb/acceptance_metadata.gdb").string();
    auto dataset = Dataset::open(source);
    ASSERT_TRUE(dataset);
    auto layer = dataset.value().open_layer("all_field_types");
    ASSERT_TRUE(layer) << layer.error().message;

    Query query;
    query.attribute_filter = "text_fld LIKE '%'";
    auto cursor_result = layer.value().open_cursor(query);
    ASSERT_TRUE(cursor_result) << cursor_result.error().message;
    auto cursor = std::move(cursor_result).value();
    EXPECT_EQ(cursor.backend_report().selected,
              Backend::GdalOpenFileGDB);
    EXPECT_EQ(cursor.backend_report().fallback_reason,
              FailureKind::UnsupportedQuery);
}

TEST(UnifiedGdalFacadeTest, GdalOnlyUsesOfficialOpenFileGdb) {
    OpenOptions options;
    options.backend = BackendPreference::GdalOnly;
    auto dataset = Dataset::open(fixture_path(), options);
    ASSERT_TRUE(dataset) << dataset.error().message;
    EXPECT_EQ(dataset.value().backend_report().selected,
              Backend::GdalOpenFileGDB);

    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    ASSERT_FALSE(names.value().empty());
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer) << layer.error().message;

    Query query;
    query.limit = 2;
    query.order = ResultOrder::FidAscending;
    auto batch = layer.value().read_all(query);
    ASSERT_TRUE(batch) << batch.error().message;
    EXPECT_LE(batch.value().features.size(), 2U);
    EXPECT_EQ(batch.value().backend_report.selected,
              Backend::GdalOpenFileGDB);
    EXPECT_FALSE(layer.value().fast_extensions());
}

TEST(UnifiedGdalFacadeTest, GdalDeadlineFailsBeforeCursorPublication) {
    OpenOptions options;
    options.backend = BackendPreference::GdalOnly;
    auto dataset = Dataset::open(fixture_path(), options);
    ASSERT_TRUE(dataset);
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer);

    Query query;
    query.deadline = std::chrono::steady_clock::now();
    auto cursor = layer.value().open_cursor(query);
    EXPECT_FALSE(cursor);
    EXPECT_EQ(cursor.error().code, ErrorCode::DeadlineExceeded);
}

TEST(UnifiedGdalFacadeTest, FastAndGdalSchemasExposeParityInputs) {
    const auto source = explorgdb_test_paths::test_data_path(
        "test_data/gdb/acceptance_metadata.gdb").string();
    auto fast_dataset = Dataset::open(source);
    ASSERT_TRUE(fast_dataset);
    OpenOptions options;
    options.backend = BackendPreference::GdalOnly;
    auto gdal_dataset = Dataset::open(source, options);
    ASSERT_TRUE(gdal_dataset);
    auto fast_layer = fast_dataset.value().open_layer("all_field_types");
    auto gdal_layer = gdal_dataset.value().open_layer("all_field_types");
    ASSERT_TRUE(fast_layer);
    ASSERT_TRUE(gdal_layer);
    const auto& fast = fast_layer.value().schema();
    const auto& gdal = gdal_layer.value().schema();
    ASSERT_EQ(fast.fields.size(), gdal.fields.size());
    for (std::size_t i = 0; i < fast.fields.size(); ++i) {
        SCOPED_TRACE(fast.fields[i].name);
        EXPECT_EQ(fast.fields[i].domain_name, gdal.fields[i].domain_name);
        EXPECT_EQ(fast.fields[i].default_value, gdal.fields[i].default_value);
    }
}

TEST(UnifiedGdalFacadeTest, GdalOnlyPreservesFeatureDatasetGroups) {
    const auto source = explorgdb_test_paths::test_data_path(
        "test_data/gdb/acceptance_metadata.gdb").string();
    OpenOptions options;
    options.backend = BackendPreference::GdalOnly;
    auto dataset = Dataset::open(source, options);
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto root = dataset.value().root_group();
    ASSERT_TRUE(root);
    auto groups = root.value().groups();
    ASSERT_TRUE(groups);
    EXPECT_NE(std::find_if(
        groups.value().begin(), groups.value().end(),
        [](const GroupInfo& group) {
            return group.name == "TransportFD";
        }), groups.value().end());
    auto roads = dataset.value().open_layer_by_path("/TransportFD/roads");
    ASSERT_TRUE(roads) << roads.error().message;
}

TEST(UnifiedGdalFacadeTest, RealAwsImmutablePrefixReadsWhenConfigured) {
    const char* fixture = std::getenv("FAST_GDB_AWS_S3_FIXTURE");
    if (fixture == nullptr || *fixture == '\0') {
        GTEST_SKIP() << "FAST_GDB_AWS_S3_FIXTURE is not configured";
    }
    OpenOptions options;
    options.backend = BackendPreference::GdalOnly;
    options.remote_source = RemoteSourcePolicy::ImmutablePrefixRequired;
    auto dataset = Dataset::open(fixture, options);
    ASSERT_TRUE(dataset) << dataset.error().message;
    EXPECT_EQ(dataset.value().consistency_report().consistency,
              Consistency::ImmutablePrefixAssumed);
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    ASSERT_FALSE(names.value().empty());
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer);
    Query query;
    query.limit = 1;
    auto cursor = layer.value().open_cursor(query);
    ASSERT_TRUE(cursor) << cursor.error().message;
    auto first = cursor.value().next();
    ASSERT_TRUE(first) << first.error().message;
}
#endif

}  // namespace
