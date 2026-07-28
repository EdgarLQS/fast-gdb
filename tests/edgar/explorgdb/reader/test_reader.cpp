#include <gtest/gtest.h>

#include "reader.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace explorgdb;

namespace {

void append_field_value(std::string& digest, const FieldValue& value) {
    digest += std::to_string(value.index()) + ":";
    if (std::holds_alternative<std::nullptr_t>(value)) {
        digest += "null";
    } else if (const auto* number = std::get_if<int32_t>(&value)) {
        digest += std::to_string(*number);
    } else if (const auto* number = std::get_if<int64_t>(&value)) {
        digest += std::to_string(*number);
    } else if (const auto* number = std::get_if<double>(&value)) {
        digest += std::to_string(*number);
    } else if (const auto* offset = std::get_if<DateTimeOffsetValue>(&value)) {
        digest += std::to_string(offset->date) + "/" +
                  std::to_string(offset->offset_minutes);
    } else if (const auto* text = std::get_if<std::string>(&value)) {
        digest += std::to_string(text->size()) + ":" + *text;
    } else if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&value)) {
        for (uint8_t byte : *bytes) {
            digest += byte < 16 ? "0" : "";
            digest += std::to_string(byte);
        }
    }
    digest += ";";
}

void append_record(std::string& digest, const FeatureRecord& record) {
    digest += "record/" + std::to_string(record.fid) + "/";
    digest += std::to_string(record.nullable_flags.size()) + ":";
    for (uint8_t flag : record.nullable_flags) digest += std::to_string(flag);
    digest += "/";
    for (const auto& value : record.field_values) {
        append_field_value(digest, value);
    }
}

void append_result(std::string& digest, const QueryResult& result) {
    digest += "status/" + std::to_string(static_cast<int>(result.status)) +
              "/path/" + result.execution_path + "/fallback/" +
              result.fallback_reason + "/";
    for (uint32_t fid : result.matched_fids) {
        digest += std::to_string(fid) + ",";
    }
    if (result.record.has_value()) append_record(digest, *result.record);
}

struct DigestQueryFields {
    std::string numeric;
    std::string text;
    std::string numeric_index;
    std::string text_index;
    bool has_geometry = false;
};

DigestQueryFields digest_query_fields(const Layer& layer) {
    DigestQueryFields result{
        "ObjectId", "missing_string", "ObjectId", "missing_string", false};
    for (const auto& field : layer.fields()) {
        if (field.type == FieldType::Geometry) result.has_geometry = true;
        if (result.text == "missing_string" &&
            (field.type == FieldType::String || field.type == FieldType::XML ||
             field.type == FieldType::UUID_1 || field.type == FieldType::UUID_2)) {
            result.text = field.name;
            result.text_index = field.name;
            if (field.name == "NAME") result.text_index = "name_index";
        }
        if (result.numeric == "ObjectId" &&
            (field.type == FieldType::Int16 || field.type == FieldType::Int32 ||
             field.type == FieldType::Int64 || field.type == FieldType::Float32 ||
             field.type == FieldType::Float64 || field.type == FieldType::ObjectId)) {
            result.numeric = field.name;
            result.numeric_index = field.name;
            if (field.name == "GB1999") {
                result.numeric_index = "GB1999_and_TOWNS_INDEX";
            }
        }
    }
    return result;
}

std::vector<QueryRequest> digest_requests(const DigestQueryFields& fields) {
    std::vector<QueryRequest> requests;
    QueryRequest sequential;
    sequential.kind = QueryKind::SequentialScan;
    sequential.limit = 16;
    sequential.sort_fids = true;
    requests.push_back(sequential);

    QueryRequest spatial;
    spatial.kind = QueryKind::SpatialBbox;
    spatial.xmin = -1e12;
    spatial.ymin = -1e12;
    spatial.xmax = 1e12;
    spatial.ymax = 1e12;
    requests.push_back(spatial);

    QueryRequest attribute_double;
    attribute_double.kind = QueryKind::AttributeDouble;
    attribute_double.index_name = fields.numeric_index;
    attribute_double.double_value = 1e12;
    attribute_double.attr_op = AttrOp::Lt;
    requests.push_back(attribute_double);
    QueryRequest attribute_string;
    attribute_string.kind = QueryKind::AttributeString;
    attribute_string.index_name = fields.text_index;
    attribute_string.attr_op = AttrOp::Ne;
    requests.push_back(attribute_string);

    QueryRequest where;
    where.kind = QueryKind::WhereClause;
    where.where_clause = fields.numeric + " >= 0";
    requests.push_back(where);
    QueryRequest spatial_where = where;
    spatial_where.kind = QueryKind::SpatialWhere;
    spatial_where.xmin = -1e12;
    spatial_where.ymin = -1e12;
    spatial_where.xmax = 1e12;
    spatial_where.ymax = 1e12;
    requests.push_back(spatial_where);
    return requests;
}

void append_cursor_digest(std::string& digest, Layer& layer,
                          const DigestQueryFields& fields) {
    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    request.limit = 16;
    request.sort_fids = true;
    auto cursor = layer.open_cursor(request);
    digest += "cursor/" + std::to_string(static_cast<int>(cursor.query_result().status));
    QueryFeature feature;
    while (cursor.next(feature)) {
        digest += "/" + std::to_string(feature.fid);
        append_record(digest, feature.record);
        digest += "/geometry/" + std::to_string(static_cast<int>(feature.geometry.status)) +
                  "/" + std::to_string(feature.geometry.wkb.size());
        for (uint8_t byte : feature.geometry.wkb) digest += ":" + std::to_string(byte);
    }
    digest += "/cursor_error/" + cursor.error();
    digest += "/geometry_expected/";
    digest += fields.has_geometry ? "1" : "0";
}

std::string run_reader_digest(const std::filesystem::path& source,
                              const std::string& layer_name) {
    ReaderError error;
    auto reader = Reader::open(source.string(), {}, &error);
    if (!reader) return "open-error/" + std::to_string(static_cast<int>(error.status));
    auto layer = reader->open_layer(layer_name, &error);
    if (!layer) return "layer-error/" + std::to_string(static_cast<int>(error.status));

    std::string digest;
    FeatureRecord record;
    digest += layer->read_by_fid(0, record) ? "fid/ok/" : "fid/empty/";
    if (!record.field_values.empty()) append_record(digest, record);

    const auto fields = digest_query_fields(*layer);
    for (const auto& request : digest_requests(fields)) {
        append_result(digest, layer->query(request));
    }
    append_cursor_digest(digest, *layer, fields);
    return digest;
}

struct DigestTarget {
    std::filesystem::path source;
    std::string layer_name;
};

std::vector<DigestTarget> discover_digest_targets(
    const std::vector<std::filesystem::path>& sources) {
    std::vector<DigestTarget> targets;
    for (const auto& source : sources) {
        ReaderError error;
        auto reader = Reader::open(source.string(), {}, &error);
        if (!reader) continue;
        size_t opened_for_source = 0;
        for (const auto& name : reader->layer_names()) {
            if (reader->open_layer(name, &error)) {
                targets.push_back({source, name});
                if (++opened_for_source == 3U) break;
            }
        }
    }
    return targets;
}

}  // namespace

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

    const LayerMetadataSnapshot metadata = layer->metadata_snapshot();
    EXPECT_EQ(metadata.name, layer->name());
    EXPECT_EQ(metadata.fields.size(), layer->fields().size());
    EXPECT_EQ(metadata.capabilities.can_read_layer(),
              layer->capabilities().can_read_layer());

    const MetadataReadResult metadata_result = layer->read_metadata();
    ASSERT_TRUE(metadata_result.ok()) << metadata_result.error.message;
    EXPECT_EQ(metadata_result.snapshot.name, metadata.name);
    EXPECT_EQ(metadata_result.snapshot.fields.size(), metadata.fields.size());
    EXPECT_EQ(metadata_result.snapshot.capabilities.can_read_layer(),
              metadata.capabilities.can_read_layer());

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

    request.kind = QueryKind::WhereClause;
    request.where_clause = "not a valid where expression";
    request.cancel_requested = {};
    EXPECT_EQ(layer->query(request).status, QueryStatus::InvalidRequest);

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

TEST(ReaderConcurrencyTest, IndependentReadersReturnIdenticalFeatureDigests) {
#ifndef FAST_GDB_SOURCE_DIR
    GTEST_SKIP() << "source directory is not configured";
#else
    const std::vector<std::filesystem::path> sources = {
        std::filesystem::path(FAST_GDB_SOURCE_DIR) /
            "test_data/benchmark/wide_50_gdal.gdb",
        std::filesystem::path(FAST_GDB_SOURCE_DIR) /
            "test_data/gdb/参数化数据_liqs.gdb"};
    const auto targets = discover_digest_targets(sources);
    ASSERT_FALSE(targets.empty()) << "no stable Reader fixture layer opened";
    std::vector<std::string> expected;
    expected.reserve(targets.size());
    for (const auto& target : targets) {
        expected.push_back(run_reader_digest(target.source, target.layer_name));
    }

    for (const size_t worker_count : {size_t{2}, size_t{4}, size_t{8}}) {
        std::vector<size_t> assignments(worker_count);
        std::vector<std::string> digests(worker_count);
        std::atomic<size_t> failures{0};
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (size_t worker = 0; worker < worker_count; ++worker) {
            workers.emplace_back([&, worker] {
                assignments[worker] = worker % targets.size();
                digests[worker] = run_reader_digest(
                    targets[assignments[worker]].source,
                    targets[assignments[worker]].layer_name);
                if (digests[worker].rfind("open-error/", 0) == 0 ||
                    digests[worker].rfind("layer-error/", 0) == 0) {
                    ++failures;
                }
            });
        }
        for (auto& worker : workers) worker.join();
        ASSERT_EQ(failures.load(), 0U);
        for (size_t worker = 0; worker < worker_count; ++worker) {
            EXPECT_EQ(digests[worker], expected[assignments[worker]])
                << "worker=" << worker;
        }
    }
#endif
}
