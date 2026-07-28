#include <gtest/gtest.h>

#include "reader.h"

#include <filesystem>
#include <utility>

using namespace explorgdb;

TEST(ReaderContractTest, ReportsStructuredArgumentErrors) {
    ReaderError error;
    const auto reader = Reader::open("", {}, &error);

    EXPECT_FALSE(reader.has_value());
    EXPECT_EQ(error.status, ReaderStatus::InvalidArgument);
    EXPECT_STREQ(reader_status_name(error.status), "invalid_argument");
}

TEST(ReaderContractTest, QueryDefaultsExposeBoundedOptions) {
    QueryRequest request;

    EXPECT_EQ(request.kind, QueryKind::SequentialScan);
    EXPECT_EQ(request.offset, 0U);
    EXPECT_EQ(request.limit, 0U);
    EXPECT_EQ(request.max_result_features, 0U);
    EXPECT_FALSE(request.field_projection.has_value());
    EXPECT_STREQ(query_status_name(QueryStatus::Cancelled), "cancelled");
}

TEST(ReaderContractTest, OpensLayerAndAppliesCursorOptions) {
#ifndef FAST_GDB_SOURCE_DIR
    GTEST_SKIP() << "source directory is not configured";
#else
    const auto source = std::filesystem::path(FAST_GDB_SOURCE_DIR) /
        "test_data/benchmark/wide_50_gdal.gdb";
    ReaderError error;
    auto reader = Reader::open(source.string(), {}, &error);
    ASSERT_TRUE(reader.has_value()) << error.message;
    ASSERT_FALSE(reader->layer_names().empty());

    std::optional<Layer> layer;
    for (const auto& name : reader->layer_names()) {
        auto candidate = reader->open_layer(name, &error);
        if (candidate.has_value()) {
            layer.emplace(std::move(*candidate));
            break;
        }
    }
    ASSERT_TRUE(layer.has_value()) << error.message;

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    request.limit = 2;
    request.sort_fids = true;
    request.field_projection = std::vector<size_t>{0};
    {
        auto cursor = layer->open_cursor(request);
        ASSERT_EQ(cursor.query_result().status, QueryStatus::Ok);

        QueryFeature feature;
        if (cursor.next(feature)) {
            ASSERT_EQ(feature.record.materialized_fields.size(),
                      feature.record.field_values.size());
            EXPECT_EQ(feature.record.materialized_fields[0], 1U);
            for (size_t index = 1;
                 index < feature.record.materialized_fields.size(); ++index) {
                EXPECT_EQ(feature.record.materialized_fields[index], 0U);
            }
        }
    }

    request.cancel_requested = [] { return true; };
    const QueryResult cancelled = layer->query(request);
    EXPECT_EQ(cancelled.status, QueryStatus::Cancelled);

    request.cancel_requested = {};
    request.field_projection =
        std::vector<size_t>{layer->fields().size()};
    const QueryResult invalid_projection = layer->query(request);
    EXPECT_EQ(invalid_projection.status, QueryStatus::InvalidRequest);
#endif
}

TEST(ReaderContractTest, RejectsLayerWhenFidBudgetIsZero) {
#ifndef FAST_GDB_SOURCE_DIR
    GTEST_SKIP() << "source directory is not configured";
#else
    const auto source = std::filesystem::path(FAST_GDB_SOURCE_DIR) /
        "test_data/benchmark/wide_50_gdal.gdb";
    ReaderOptions options;
    options.max_fid_slots = 0;
    ReaderError error;
    auto reader = Reader::open(source.string(), options, &error);
    ASSERT_TRUE(reader.has_value()) << error.message;
    for (const auto& name : reader->layer_names()) {
        EXPECT_FALSE(reader->open_layer(name, &error).has_value());
        if (error.status == ReaderStatus::FidRangeUnsupported) return;
    }
    FAIL() << "no non-empty layer exceeded the zero FID budget";
#endif
}
