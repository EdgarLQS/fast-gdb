#include <gtest/gtest.h>

#include <algorithm>
#include <iomanip>
#include <optional>

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "gdb_tablx.h"
#include "reader.h"
#include "test_paths.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
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

struct OpenReadTarget {
    std::filesystem::path source;
    std::string layer_name;
    RowSlot fid = 0;
    std::string expected_digest;
};

std::optional<RowSlot> first_active_fid(
    const std::filesystem::path& source,
    const std::string& layer_name) {
    ReaderError error;
    auto reader = Reader::open(source.string(), {}, &error);
    if (!reader) return std::nullopt;
    auto layer = reader->open_layer(layer_name, &error);
    if (!layer) return std::nullopt;

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    request.limit = 1;
    request.sort_fids = true;
    const QueryResult result = layer->query(request);
    if (result.status != QueryStatus::Ok || result.matched_fids.empty())
        return std::nullopt;
    return result.matched_fids.front();
}

std::string read_record_digest(const OpenReadTarget& target) {
    ReaderError error;
    auto reader = Reader::open(target.source.string(), {}, &error);
    if (!reader)
        return "open-error/" + std::to_string(static_cast<int>(error.status));
    auto layer = reader->open_layer(target.layer_name, &error);
    if (!layer)
        return "layer-error/" + std::to_string(static_cast<int>(error.status));

    FeatureRecord record;
    if (!layer->read_by_fid(target.fid, record)) return "read-error";
    std::string digest;
    append_record(digest, record);
    return digest;
}

double nearest_rank_percentile_ms(std::vector<double> samples,
                                  size_t numerator,
                                  size_t denominator) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t rank =
        (numerator * samples.size() + denominator - 1) / denominator;
    return samples[std::max<size_t>(1, rank) - 1];
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(ReaderContractTest, ReportsStructuredArgumentErrors) {
    ReaderError error;
    const auto reader = Reader::open("", {}, &error);

    EXPECT_FALSE(reader.has_value());
    EXPECT_EQ(error.status, ReaderStatus::InvalidArgument);
    EXPECT_STREQ(reader_status_name(error.status), "invalid_argument");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(ReaderContractTest, QueryDefaultsExposeBoundedOptions) {
    QueryRequest request;

    EXPECT_EQ(request.kind, QueryKind::SequentialScan);
    EXPECT_EQ(request.offset, 0U);
    EXPECT_EQ(request.limit, 0U);
    EXPECT_EQ(request.max_result_features, 0U);
    EXPECT_FALSE(request.field_projection.has_value());
    EXPECT_TRUE(request.include_geometry);
    EXPECT_STREQ(query_status_name(QueryStatus::Cancelled), "cancelled");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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
    EXPECT_EQ(metadata_result.snapshot.workspace_domains.size(),
              metadata.workspace_domains.size());
    EXPECT_EQ(metadata_result.snapshot.relationship_definitions.size(),
              metadata.relationship_definitions.size());

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

TEST(ReaderContractTest, CursorCanSkipOutputGeometryDecoding) {
#ifndef FAST_GDB_SOURCE_DIR
    GTEST_SKIP() << "source directory is not configured";
#else
    const auto source = std::filesystem::path(FAST_GDB_SOURCE_DIR) /
        "test_data/gdb/acceptance_metadata.gdb";
    ReaderError error;
    auto reader = Reader::open(source.string(), {}, &error);
    ASSERT_TRUE(reader.has_value()) << error.message;
    auto layer = reader->open_layer("roads", &error);
    ASSERT_TRUE(layer.has_value()) << error.message;
    ASSERT_NE(std::find_if(
        layer->fields().begin(), layer->fields().end(),
        [](const FieldDescriptor& field) {
            return field.type == FieldType::Geometry;
        }), layer->fields().end());

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    request.profile_feature_reads = true;
    request.include_geometry = false;
    {
        auto cursor = layer->open_cursor(request);
        ASSERT_EQ(cursor.query_result().status, QueryStatus::Ok);
        QueryFeature feature;
        ASSERT_TRUE(cursor.next(feature));
        EXPECT_EQ(feature.geometry.status, GeometryStatus::NotRequested);
        EXPECT_EQ(cursor.query_result().feature_cursor_metrics.feature_count,
                  1U);
        EXPECT_DOUBLE_EQ(
            cursor.query_result().feature_cursor_metrics.geometry_decode_ms,
            0.0);
        EXPECT_DOUBLE_EQ(
            cursor.query_result().feature_cursor_metrics.wkb_write_ms, 0.0);
    }

    request.kind = QueryKind::SpatialWhere;
    request.where_clause = "road_id = 1";
    request.xmin = -180.0;
    request.ymin = -90.0;
    request.xmax = 180.0;
    request.ymax = 90.0;
    {
        auto cursor = layer->open_cursor(request);
        ASSERT_EQ(cursor.query_result().status, QueryStatus::Ok)
            << cursor.query_result().error;
        QueryFeature feature;
        ASSERT_TRUE(cursor.next(feature));
        EXPECT_EQ(feature.geometry.status, GeometryStatus::NotRequested);
        EXPECT_EQ(cursor.query_result().feature_cursor_metrics.feature_count,
                  1U);
        EXPECT_DOUBLE_EQ(
            cursor.query_result().feature_cursor_metrics.geometry_decode_ms,
            0.0);
    }

    request.kind = QueryKind::SequentialScan;
    request.where_clause.clear();
    request.include_geometry = true;
    auto cursor = layer->open_cursor(request);
    ASSERT_EQ(cursor.query_result().status, QueryStatus::Ok);
    QueryFeature feature;
    ASSERT_TRUE(cursor.next(feature));
    EXPECT_NE(feature.geometry.status, GeometryStatus::NotRequested);
#endif
}

TEST(ReaderSnapshotTest, DetectsSameSizeChangeWithRestoredMtime) {
#ifndef FAST_GDB_SOURCE_DIR
    GTEST_SKIP() << "source directory is not configured";
#else
    namespace fs = std::filesystem;
    const auto source = fs::path(FAST_GDB_SOURCE_DIR) /
        "test_data/gdb/acceptance_metadata.gdb";
    const auto target = fs::temp_directory_path() /
        ("fast_gdb_reader_identity_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".gdb");
    struct RemoveDirectory {
        fs::path path;
        ~RemoveDirectory() {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{target};
    fs::copy(source, target, fs::copy_options::recursive);

    ReaderError error;
    auto reader = Reader::open(target.string(), {}, &error);
    ASSERT_TRUE(reader.has_value()) << error.message;
    std::optional<Layer> layer;
    for (const auto& name : reader->layer_names()) {
        auto candidate = reader->open_layer(name, &error);
        if (candidate) {
            layer.emplace(std::move(*candidate));
            break;
        }
    }
    ASSERT_TRUE(layer.has_value()) << error.message;

    fs::path table_path;
    for (const auto& entry : fs::directory_iterator(target)) {
        if (entry.is_regular_file() &&
            entry.path().extension() == ".gdbtable") {
            table_path = entry.path();
            break;
        }
    }
    ASSERT_FALSE(table_path.empty());
    const auto original_mtime = fs::last_write_time(table_path);
    {
        std::fstream file(table_path,
                          std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        char byte = 0;
        file.read(&byte, 1);
        ASSERT_TRUE(file.good());
        byte = static_cast<char>(byte ^ 0x01);
        file.seekp(0);
        file.write(&byte, 1);
        ASSERT_TRUE(file.good());
    }
    fs::last_write_time(table_path, original_mtime);
    EXPECT_EQ(fs::last_write_time(table_path), original_mtime);

    EXPECT_FALSE(reader->source_is_current());
    EXPECT_FALSE(layer->source_is_current());
    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    EXPECT_EQ(layer->query(request).status, QueryStatus::SourceChanged);
    const auto cursor = layer->open_cursor(request);
    EXPECT_EQ(cursor.query_result().status, QueryStatus::SourceChanged);
    FeatureRecord record;
    EXPECT_FALSE(layer->read_by_fid(0, record));
#endif
}

TEST(ReaderContractTest, OpensUtf8PathWithChineseDirectoryAndReadsFeature) {
#ifndef FAST_GDB_SOURCE_DIR
    GTEST_SKIP() << "source directory is not configured";
#else
    explorgdb_test_paths::Utf8PathGdbFixture fixture;
    ASSERT_FALSE(fixture.error()) << fixture.error().message();

    ReaderError error;
    auto reader = Reader::open(fixture.path().u8string(), {}, &error);
    ASSERT_TRUE(reader.has_value()) << error.message;
    auto layer = reader->open_layer("roads", &error);
    ASSERT_TRUE(layer.has_value()) << error.message;
    FeatureRecord record;
    EXPECT_TRUE(layer->read_by_fid(0, record));
#endif
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

TEST(ReaderContractTest, ReadsWorkspaceMetadataWithoutOpeningALayer) {
#ifndef FAST_GDB_SOURCE_DIR
    GTEST_SKIP() << "source directory is not configured";
#else
    const auto source = std::filesystem::path(FAST_GDB_SOURCE_DIR) /
        "test_data/gdb/acceptance_metadata.gdb";
    ReaderError error;
    auto reader = Reader::open(source.string(), {}, &error);
    ASSERT_TRUE(reader.has_value()) << error.message;

    const auto metadata = reader->read_workspace_metadata();
    ASSERT_TRUE(metadata.ok()) << metadata.error.message;
    EXPECT_GE(metadata.snapshot.domains.size(), 4U);
    EXPECT_EQ(metadata.snapshot.domains_xml.status,
              MetadataXmlStatus::Available);
    EXPECT_GE(metadata.snapshot.relationship_classes.size(), 3U);
    EXPECT_EQ(metadata.snapshot.relationship_classes_xml.status,
              MetadataXmlStatus::Available);
    EXPECT_TRUE(metadata.snapshot.attachments.empty());
    EXPECT_EQ(metadata.snapshot.attachments_xml.status,
              MetadataXmlStatus::Empty);
    EXPECT_GE(metadata.snapshot.dataset_groups.size(), 2U);
    EXPECT_EQ(metadata.snapshot.feature_datasets_xml.status,
              MetadataXmlStatus::Available);
    const auto transport = std::find_if(
        metadata.snapshot.dataset_groups.begin(),
        metadata.snapshot.dataset_groups.end(), [](const auto& group) {
            return group.group_path == "\\TransportFD";
        });
    ASSERT_NE(transport, metadata.snapshot.dataset_groups.end());
    EXPECT_NE(transport->raw_definition.find("esriDTFeatureDataset"),
              std::string::npos);
    ASSERT_EQ(metadata.snapshot.system_table_audit.size(), 6U);
    EXPECT_TRUE(std::all_of(
        metadata.snapshot.system_table_audit.begin(),
        metadata.snapshot.system_table_audit.end(), [](const auto& audit) {
            return audit.status == "ok" || audit.status == "empty" ||
                   audit.status == "missing";
        }));

    const auto relationship = std::find_if(
        metadata.snapshot.relationship_classes.begin(),
        metadata.snapshot.relationship_classes.end(),
        [](const RelationshipClassInfo& item) {
            return item.name == "roads_inspections_rel";
        });
    ASSERT_NE(relationship, metadata.snapshot.relationship_classes.end());
    EXPECT_EQ(relationship->cardinality, "esriRelCardinalityOneToMany");
    EXPECT_EQ(relationship->origin_class_names,
              (std::vector<std::string>{"roads"}));
    EXPECT_EQ(relationship->destination_class_names,
              (std::vector<std::string>{"road_inspections"}));
    ASSERT_FALSE(relationship->keys.empty());
    EXPECT_EQ(relationship->keys.front().object_key_name, "OBJECTID");
    EXPECT_NE(relationship->raw_definition.find("DERelationshipClassInfo"),
              std::string::npos);
#endif
}

TEST(ReaderContractTest, ReadsBinaryFieldsWithNullEmptyAndSizeLimitSemantics) {
#ifndef FAST_GDB_SOURCE_DIR
    GTEST_SKIP() << "source directory is not configured";
#else
    const auto source = std::filesystem::path(FAST_GDB_SOURCE_DIR) /
        "test_data/gdb/acceptance_metadata.gdb";
    ReaderError error;
    auto reader = Reader::open(source.string(), {}, &error);
    ASSERT_TRUE(reader.has_value()) << error.message;
    auto layer = reader->open_layer("all_field_types", &error);
    ASSERT_TRUE(layer.has_value()) << error.message;

    const auto null_value = layer->read_binary_field_by_fid(0, "blob_fld", 64);
    EXPECT_EQ(null_value.status, BinaryValueReadStatus::NullValue);
    EXPECT_TRUE(null_value.bytes.empty());

    const auto empty_value =
        layer->read_binary_field_by_fid(1, "BLOB_FLD", 64);
    EXPECT_EQ(empty_value.status, BinaryValueReadStatus::Available);
    EXPECT_EQ(empty_value.byte_size, 0U);
    EXPECT_TRUE(empty_value.bytes.empty());

    const auto small_value =
        layer->read_binary_field_by_fid(2, "blob_fld", 64);
    EXPECT_EQ(small_value.status, BinaryValueReadStatus::Available);
    EXPECT_EQ(small_value.byte_size, 6U);
    EXPECT_EQ(small_value.bytes,
              (std::vector<std::uint8_t>{0x5c, 0x78, 0x30, 0x30, 0x01, 0x02}));

    const auto text_value =
        layer->read_binary_field_by_fid(3, "blob_fld", 64);
    EXPECT_EQ(text_value.status, BinaryValueReadStatus::Available);
    EXPECT_EQ(text_value.byte_size, 21U);
    EXPECT_EQ(text_value.bytes,
              (std::vector<std::uint8_t>{
                  0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x42, 0x69, 0x6e,
                  0x61, 0x72, 0x79, 0x20, 0x57, 0x6f, 0x72, 0x6c, 0x64,
                  0x21, 0x00, 0xff}));

    const auto limited =
        layer->read_binary_field_by_fid(5, "blob_fld", 64);
    EXPECT_EQ(limited.status, BinaryValueReadStatus::LimitExceeded);
    EXPECT_EQ(limited.byte_size, 201U);
    EXPECT_TRUE(limited.bytes.empty());

    EXPECT_EQ(layer->read_binary_field_by_fid(2, "missing_blob", 64).status,
              BinaryValueReadStatus::FieldNotFound);
    EXPECT_EQ(layer->read_binary_field_by_fid(2, "text_fld", 64).status,
              BinaryValueReadStatus::NotBinaryField);
    EXPECT_EQ(layer->read_binary_field_by_fid(1000, "blob_fld", 64).status,
              BinaryValueReadStatus::FeatureNotFound);
#endif
}

TEST(ReaderContractTest, ReadsArcGisAttachmentFixtureWhenConfigured) {
    const char* dataset_value =
        std::getenv("FAST_GDB_ATTACHMENT_DATASET");
    const char* expected_value =
        std::getenv("FAST_GDB_ATTACHMENT_EXPECTED_TSV");
    if (dataset_value == nullptr || *dataset_value == '\0' ||
        expected_value == nullptr || *expected_value == '\0') {
        GTEST_SKIP() << "Set FAST_GDB_ATTACHMENT_DATASET and "
                        "FAST_GDB_ATTACHMENT_EXPECTED_TSV to an ArcGIS Pro "
                        "attachment fixture";
    }

    const auto dataset = std::filesystem::path(dataset_value);
    const auto expected_path = std::filesystem::path(expected_value);
    ReaderError error;
    auto reader = Reader::open(dataset.string(), {}, &error);
    ASSERT_TRUE(reader.has_value()) << error.message;

    const auto workspace = reader->read_workspace_metadata();
    ASSERT_TRUE(workspace.ok()) << workspace.error.message;
    ASSERT_EQ(workspace.snapshot.attachments.size(), 1U);
    const auto& attachment = workspace.snapshot.attachments.front();
    EXPECT_TRUE(attachment.attachment_table_present);
    EXPECT_EQ(attachment.origin_class_name, "attachment_features");
    EXPECT_EQ(attachment.attachment_table_name,
              "attachment_features__ATTACH");
    EXPECT_EQ(attachment.cardinality, "esriRelCardinalityOneToMany");
    EXPECT_NE(attachment.raw_definition.find("IsAttachmentRelationship"),
              std::string::npos);
    const auto has_key = [&attachment](const std::string& name) {
        return std::any_of(
            attachment.keys.begin(), attachment.keys.end(),
            [&name](const RelationshipClassKeyInfo& key) {
                auto equal_ignore_case = [](const std::string& lhs,
                                            const std::string& rhs) {
                    return lhs.size() == rhs.size() &&
                        std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                            [](unsigned char left, unsigned char right) {
                                return std::tolower(left) ==
                                       std::tolower(right);
                            });
                };
                return equal_ignore_case(key.object_key_name, name) ||
                       equal_ignore_case(key.class_key_name, name);
            });
    };
    EXPECT_TRUE(has_key("GlobalID"));
    EXPECT_TRUE(has_key("REL_GLOBALID"));
    EXPECT_TRUE(std::any_of(
        attachment.keys.begin(), attachment.keys.end(),
        [](const RelationshipClassKeyInfo& key) {
            return key.object_key_name == "GlobalID" &&
                   key.key_role == "esriRelKeyRoleOriginPrimary";
        }));
    EXPECT_TRUE(std::any_of(
        attachment.keys.begin(), attachment.keys.end(),
        [](const RelationshipClassKeyInfo& key) {
            return key.object_key_name == "REL_GLOBALID" &&
                   key.key_role == "esriRelKeyRoleDestinationForeign";
        }));

    auto parent_layer = reader->open_layer("attachment_features", &error);
    ASSERT_TRUE(parent_layer.has_value()) << error.message;
    auto layer = reader->open_layer(attachment.attachment_table_name, &error);
    ASSERT_TRUE(layer.has_value()) << error.message;
    const auto field_index = [](const Layer& candidate,
                                const std::string& name)
            -> std::optional<std::size_t> {
        const auto& fields = candidate.fields();
        for (std::size_t index = 0; index < fields.size(); ++index) {
            if (fields[index].name.size() != name.size()) continue;
            if (std::equal(fields[index].name.begin(), fields[index].name.end(),
                           name.begin(), [](unsigned char left,
                                            unsigned char right) {
                               return std::tolower(left) ==
                                      std::tolower(right);
                           })) {
                return index;
            }
        }
        return std::nullopt;
    };
    const auto object_id_index = field_index(*layer, "OBJECTID");
    const auto related_id_index = field_index(*layer, "REL_GLOBALID");
    const auto name_index = field_index(*layer, "ATT_NAME");
    const auto size_index = field_index(*layer, "DATA_SIZE");
    const auto parent_key_index = field_index(*parent_layer, "PARENT_KEY");
    const auto parent_globalid_index = field_index(*parent_layer, "GlobalID");
    ASSERT_TRUE(object_id_index.has_value());
    ASSERT_TRUE(related_id_index.has_value());
    ASSERT_TRUE(name_index.has_value());
    ASSERT_TRUE(size_index.has_value());
    ASSERT_TRUE(parent_key_index.has_value());
    ASSERT_TRUE(parent_globalid_index.has_value());
    EXPECT_EQ(layer->active_feature_count(), 4U);
    EXPECT_EQ(parent_layer->active_feature_count(), 3U);

    std::ifstream expected_stream{expected_path};
    ASSERT_TRUE(expected_stream.is_open()) << expected_path.string();
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(expected_stream, line)));
    EXPECT_EQ(line,
              "row_slot\tobject_id\tparent_key\tparent_globalid\t"
              "attachment_name\tdata_size\tsha256\tpayload_path");

    const auto split_tsv = [](const std::string& value) {
        std::vector<std::string> fields;
        std::stringstream stream(value);
        std::string field;
        while (std::getline(stream, field, '\t')) fields.push_back(field);
        return fields;
    };
    const auto normalize_guid = [](std::string value) {
        value.erase(std::remove(value.begin(), value.end(), '{'), value.end());
        value.erase(std::remove(value.begin(), value.end(), '}'), value.end());
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    };
    const auto as_text = [](const FieldValue& value)
            -> std::optional<std::string> {
        if (const auto* text = std::get_if<std::string>(&value)) return *text;
        return std::nullopt;
    };
    const auto as_integer = [](const FieldValue& value)
            -> std::optional<int64_t> {
        if (const auto* number = std::get_if<int32_t>(&value)) return *number;
        if (const auto* number = std::get_if<int64_t>(&value)) return *number;
        return std::nullopt;
    };

    std::map<std::string, std::string> parent_global_ids;
    for (RowSlot row_slot = 0; row_slot < 3; ++row_slot) {
        FeatureRecord record;
        ASSERT_TRUE(parent_layer->read_by_fid(row_slot, record));
        ASSERT_LT(*parent_key_index, record.field_values.size());
        ASSERT_LT(*parent_globalid_index, record.field_values.size());
        const auto parent_key = as_text(
            record.field_values[*parent_key_index]);
        const auto parent_global_id = as_text(
            record.field_values[*parent_globalid_index]);
        ASSERT_TRUE(parent_key.has_value());
        ASSERT_TRUE(parent_global_id.has_value());
        EXPECT_TRUE(parent_global_ids.emplace(
            *parent_key, *parent_global_id).second);
    }
    ASSERT_EQ(parent_global_ids.size(), 3U);
    ASSERT_TRUE(parent_global_ids.count("PARENT-03") > 0U);

    std::map<std::string, std::size_t> attachments_per_parent;
    std::size_t checked_rows = 0;
    while (std::getline(expected_stream, line)) {
        if (line.empty()) continue;
        const auto columns = split_tsv(line);
        ASSERT_EQ(columns.size(), 8U) << line;
        const RowSlot row_slot = static_cast<RowSlot>(
            std::stoull(columns[0]));
        const ObjectId object_id = static_cast<ObjectId>(
            std::stoull(columns[1]));
        const std::size_t expected_size = static_cast<std::size_t>(
            std::stoull(columns[5]));
        ASSERT_GT(object_id, 0U);
        EXPECT_EQ(row_slot + 1U, object_id);

        FeatureRecord record;
        ASSERT_TRUE(layer->read_by_fid(row_slot, record))
            << "attachment row slot " << row_slot;
        ASSERT_LT(*object_id_index, record.field_values.size());
        ASSERT_LT(*related_id_index, record.field_values.size());
        ASSERT_LT(*name_index, record.field_values.size());
        ASSERT_LT(*size_index, record.field_values.size());
        const auto record_object_id = as_integer(
            record.field_values[*object_id_index]);
        ASSERT_TRUE(record_object_id.has_value());
        EXPECT_EQ(static_cast<ObjectId>(*record_object_id), object_id);
        const auto related_id = as_text(
            record.field_values[*related_id_index]);
        ASSERT_TRUE(related_id.has_value());
        EXPECT_EQ(normalize_guid(*related_id), normalize_guid(columns[3]));
        const auto parent_global_id = parent_global_ids.find(columns[2]);
        ASSERT_NE(parent_global_id, parent_global_ids.end());
        EXPECT_EQ(normalize_guid(*related_id),
                  normalize_guid(parent_global_id->second));
        const auto attachment_name = as_text(
            record.field_values[*name_index]);
        ASSERT_TRUE(attachment_name.has_value());
        EXPECT_EQ(*attachment_name, columns[4]);
        const auto record_size = as_integer(record.field_values[*size_index]);
        ASSERT_TRUE(record_size.has_value());
        EXPECT_EQ(static_cast<std::size_t>(*record_size), expected_size);

        const std::filesystem::path payload_path =
            expected_path.parent_path() /
            std::filesystem::u8path(columns[7]);
        std::ifstream payload_stream{payload_path, std::ios::binary};
        ASSERT_TRUE(payload_stream.is_open()) << payload_path.string();
        std::vector<std::uint8_t> expected_bytes;
        char byte = 0;
        while (payload_stream.get(byte)) {
            expected_bytes.push_back(static_cast<std::uint8_t>(
                static_cast<unsigned char>(byte)));
        }
        ASSERT_EQ(expected_bytes.size(), expected_size);

        const auto binary = layer->read_binary_field_by_fid(
            row_slot, "DATA", expected_size);
        ASSERT_EQ(binary.status, BinaryValueReadStatus::Available)
            << binary.diagnostic;
        EXPECT_EQ(binary.byte_size, expected_size);
        EXPECT_EQ(binary.bytes, expected_bytes);

        const std::size_t limited_bytes =
            expected_size > 65536U ? 65536U : expected_size - 1U;
        const auto limited = layer->read_binary_field_by_fid(
            row_slot, "DATA", limited_bytes);
        EXPECT_EQ(limited.status, BinaryValueReadStatus::LimitExceeded);
        EXPECT_EQ(limited.byte_size, expected_size);
        EXPECT_TRUE(limited.bytes.empty());

        ++attachments_per_parent[columns[2]];
        ++checked_rows;
    }
    EXPECT_EQ(checked_rows, 4U);
    EXPECT_EQ(attachments_per_parent["PARENT-01"], 2U);
    EXPECT_EQ(attachments_per_parent["PARENT-02"], 2U);
    EXPECT_EQ(attachments_per_parent.count("PARENT-03"), 0U);
}

TEST(ReaderContractTest, InventoriesEveryCatalogObjectAndReadsItByUuid) {
#ifndef FAST_GDB_SOURCE_DIR
    GTEST_SKIP() << "source directory is not configured";
#else
    const auto source = std::filesystem::path(FAST_GDB_SOURCE_DIR) /
        "test_data/gdb/acceptance_metadata.gdb";
    ReaderError error;
    auto reader = Reader::open(source.string(), {}, &error);
    ASSERT_TRUE(reader.has_value()) << error.message;

    const auto metadata = reader->read_workspace_metadata();
    ASSERT_TRUE(metadata.ok()) << metadata.error.message;
    const auto& inventory = metadata.snapshot.catalog_objects;
    EXPECT_EQ(inventory.status, CatalogInventoryStatus::Complete);
    EXPECT_GE(inventory.physical_row_slots, 19U);
    EXPECT_EQ(inventory.active_rows, 19U);
    ASSERT_EQ(inventory.objects.size(), 19U);
    EXPECT_TRUE(inventory.issues.empty());

    const std::string roads_uuid =
        "{E32B8E0E-76FF-4792-A874-31FD8B282224}";
    const auto canonical_uuid = [](std::string value) {
        std::string canonical;
        for (const unsigned char byte : value) {
            if (byte == '{' || byte == '}' || byte == '-') continue;
            canonical.push_back(static_cast<char>(std::tolower(byte)));
        }
        return canonical;
    };
    const auto roads_summary = std::find_if(
        inventory.objects.begin(), inventory.objects.end(),
        [&roads_uuid, &canonical_uuid](const CatalogObjectSummary& object) {
            return canonical_uuid(object.uuid) == canonical_uuid(roads_uuid);
        });
    ASSERT_NE(roads_summary, inventory.objects.end());
    EXPECT_EQ(roads_summary->name, "roads");
    EXPECT_EQ(roads_summary->path, "\\TransportFD\\roads");
    EXPECT_EQ(roads_summary->type_name, "Feature Class");
    EXPECT_EQ(roads_summary->dataset_type, "esriDTFeatureClass");
    EXPECT_EQ(roads_summary->geometry_type, "esriGeometryPolyline");
    EXPECT_EQ(roads_summary->advanced_kind, AdvancedObjectKind::None);
    EXPECT_TRUE(roads_summary->physical_table_resolves);
    EXPECT_TRUE(std::all_of(
        inventory.objects.begin(), inventory.objects.end(),
        [](const CatalogObjectSummary& object) {
            return object.advanced_kind == AdvancedObjectKind::None;
        }));

    const auto roads = reader->read_catalog_object_by_uuid(roads_uuid);
    ASSERT_EQ(roads.status, CatalogObjectReadStatus::Available)
        << roads.diagnostic;
    ASSERT_TRUE(roads.object.has_value());
    EXPECT_EQ(canonical_uuid(roads.object->summary.uuid),
              canonical_uuid(roads_uuid));
    EXPECT_EQ(roads.object->summary.type_name, "Feature Class");
    EXPECT_EQ(roads.object->summary.dataset_type, "esriDTFeatureClass");
    EXPECT_FALSE(roads.object->attributes.empty());
    ASSERT_TRUE(roads.object->definition_present);
    EXPECT_NE(roads.object->raw_definition.find("DEFeatureClassInfo"),
              std::string::npos);
    const auto definition_attribute = std::find_if(
        roads.object->attributes.begin(), roads.object->attributes.end(),
        [](const CatalogObjectAttribute& attribute) {
            return attribute.name == "Definition";
        });
    ASSERT_NE(definition_attribute, roads.object->attributes.end());
    const auto* definition =
        std::get_if<std::string>(&definition_attribute->value);
    ASSERT_NE(definition, nullptr);
    EXPECT_EQ(*definition, roads.object->raw_definition);
    const auto properties = std::find_if(
        roads.object->attributes.begin(), roads.object->attributes.end(),
        [](const CatalogObjectAttribute& attribute) {
            return attribute.name == "Properties";
        });
    ASSERT_NE(properties, roads.object->attributes.end());
    ASSERT_TRUE(std::holds_alternative<int32_t>(properties->value));
    EXPECT_EQ(std::get<int32_t>(properties->value), 1);
    const auto defaults = std::find_if(
        roads.object->attributes.begin(), roads.object->attributes.end(),
        [](const CatalogObjectAttribute& attribute) {
            return attribute.name == "Defaults";
        });
    ASSERT_NE(defaults, roads.object->attributes.end());
    EXPECT_EQ(defaults->type, FieldType::Binary);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(defaults->value));

    const auto missing = reader->read_catalog_object_by_uuid(
        "{00000000-0000-0000-0000-000000000000}");
    EXPECT_EQ(missing.status, CatalogObjectReadStatus::NotFound);
#endif
}

TEST(ReaderContractTest, ReportsCatalogRowsThatFailToDecode) {
#ifndef FAST_GDB_SOURCE_DIR
    GTEST_SKIP() << "source directory is not configured";
#else
    const auto source = std::filesystem::path(FAST_GDB_SOURCE_DIR) /
        "test_data/gdb/acceptance_metadata.gdb";
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(source.string()));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto items = resolver.resolve("GDB_Items");
    ASSERT_TRUE(items.has_value());

    GdbTablxParser tablx(items->tablx_path);
    ASSERT_TRUE(tablx.parse());
    const auto& offsets = tablx.offsets();
    const auto active = std::find_if(
        offsets.rbegin(), offsets.rend(),
        [](std::uint64_t offset) { return offset != 0; });
    ASSERT_NE(active, offsets.rend());
    const auto row_slot = static_cast<std::uint64_t>(
        offsets.rend() - active - 1);
    const auto file_offset = *active;

    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto copy_path = std::filesystem::temp_directory_path() /
        ("fast-gdb-catalog-integrity-" + suffix + ".gdb");
    struct TempDirectoryCleanup {
        std::filesystem::path path;
        ~TempDirectoryCleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{copy_path};
    std::error_code copy_error;
    std::filesystem::copy(
        source, copy_path, std::filesystem::copy_options::recursive,
        copy_error);
    ASSERT_FALSE(copy_error) << copy_error.message();

    const auto relative_table =
        std::filesystem::path(items->table_path).lexically_relative(source);
    const auto copied_table = copy_path / relative_table;
    std::fstream table_file(copied_table,
                            std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(table_file.is_open());
    table_file.seekp(static_cast<std::streamoff>(file_offset));
    const char invalid_length[4] = {
        static_cast<char>(0xff), static_cast<char>(0xff),
        static_cast<char>(0xff), static_cast<char>(0xff)};
    table_file.write(invalid_length, sizeof(invalid_length));
    table_file.close();
    ASSERT_TRUE(table_file.good());

    ReaderError error;
    auto reader = Reader::open(copy_path.string(), {}, &error);
    ASSERT_TRUE(reader.has_value()) << error.message;
    const auto metadata = reader->read_workspace_metadata();
    ASSERT_TRUE(metadata.ok()) << metadata.error.message;
    const auto& inventory = metadata.snapshot.catalog_objects;
    EXPECT_EQ(inventory.status, CatalogInventoryStatus::Partial);
    EXPECT_EQ(inventory.active_rows, 19U);
    EXPECT_EQ(inventory.decoded_rows, 18U);
    EXPECT_EQ(inventory.objects.size(), 18U);
    const auto failed_row = std::find_if(
        inventory.issues.begin(), inventory.issues.end(),
        [row_slot](const CatalogIntegrityIssue& issue) {
            return issue.row_slot == row_slot;
        });
    ASSERT_NE(failed_row, inventory.issues.end());
    EXPECT_TRUE(
        failed_row->code == CatalogIntegrityIssueCode::RowReadFailed ||
        failed_row->code == CatalogIntegrityIssueCode::RowShapeMismatch);
#endif
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(ReaderConcurrencyTest, IndependentReadersReturnIdenticalFeatureDigests) {
#ifndef FAST_GDB_SOURCE_DIR
    GTEST_SKIP() << "source directory is not configured";
#else
    std::vector<DigestTarget> targets;
    const char* supplied_curve_path =
        std::getenv("FAST_GDB_CURVE_DATASET");
    if (supplied_curve_path != nullptr && *supplied_curve_path != '\0') {
        const std::filesystem::path supplied_source(supplied_curve_path);
        ReaderError error;
        auto supplied_reader = Reader::open(supplied_source.string(), {}, &error);
        ASSERT_TRUE(supplied_reader.has_value()) << error.message;
        const std::vector<std::string> supplied_layers = {
            "CRS_CGCS2000_Point_FC",
            "CRS_WGS84_Point_FC",
            "CRS_Xian80_Point_FC"};
        for (const auto& layer_name : supplied_layers) {
            ASSERT_TRUE(supplied_reader->open_layer(layer_name, &error))
                << layer_name << ": " << error.message;
            targets.push_back({supplied_source, layer_name});
        }
        supplied_reader.reset();
    }
    const std::vector<std::filesystem::path> fixture_sources = {
        std::filesystem::path(FAST_GDB_SOURCE_DIR) /
            "test_data/benchmark/wide_50_gdal.gdb",
        std::filesystem::path(FAST_GDB_SOURCE_DIR) /
            "test_data/gdb/参数化数据_liqs.gdb"};
    const auto fixture_targets = discover_digest_targets(fixture_sources);
    targets.insert(targets.end(), fixture_targets.begin(), fixture_targets.end());
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

TEST(ReaderConcurrencyBenchmarkTest, SharedLayerOpenAndFidReadProfile) {
    const char* enabled =
        std::getenv("FAST_GDB_READER_CONTENTION_BENCHMARK");
    if (enabled == nullptr || std::string(enabled) != "1")
        GTEST_SKIP() << "set FAST_GDB_READER_CONTENTION_BENCHMARK=1";

    const char* dataset_value = std::getenv("FAST_GDB_CURVE_DATASET");
    const char* output_value =
        std::getenv("FAST_GDB_READER_CONTENTION_OUTPUT");
    if (dataset_value == nullptr || *dataset_value == '\0' ||
        output_value == nullptr || *output_value == '\0') {
        GTEST_SKIP() << "set testcurve dataset and benchmark output paths";
    }

    size_t iterations_per_thread = 25;
    if (const char* value =
            std::getenv("FAST_GDB_READER_CONTENTION_ITERATIONS")) {
        const unsigned long long parsed = std::strtoull(value, nullptr, 10);
        if (parsed > 0 && parsed <= 10000ULL)
            iterations_per_thread = static_cast<size_t>(parsed);
    }

    const std::filesystem::path source(dataset_value);
    ASSERT_TRUE(std::filesystem::is_directory(source)) << source.string();
    const std::vector<std::string> layer_names = {
        "CRS_CGCS2000_Point_FC",
        "CRS_WGS84_Point_FC",
        "CRS_Xian80_Point_FC"};
    std::vector<OpenReadTarget> targets;
    targets.reserve(layer_names.size());
    for (const auto& layer_name : layer_names) {
        const auto fid = first_active_fid(source, layer_name);
        ASSERT_TRUE(fid.has_value()) << "no active row in " << layer_name;
        OpenReadTarget target{source, layer_name, *fid, {}};
        target.expected_digest = read_record_digest(target);
        ASSERT_FALSE(target.expected_digest == "read-error" ||
                     target.expected_digest.rfind("open-error/", 0) == 0 ||
                     target.expected_digest.rfind("layer-error/", 0) == 0)
            << layer_name << ": " << target.expected_digest;
        targets.push_back(std::move(target));
    }

    struct ProfileRow {
        size_t thread_count = 0;
        std::string target_mode;
        double throughput_ops_per_second = 0.0;
        std::vector<double> latency_ms;
        size_t failures = 0;
    };
    std::vector<ProfileRow> rows;
    for (const bool rotate_layers : {false, true}) {
        for (const size_t thread_count : {size_t{1}, size_t{2},
                                          size_t{4}, size_t{8}}) {
            struct WorkerResult {
                std::chrono::steady_clock::time_point start;
                std::chrono::steady_clock::time_point finish;
                std::vector<double> latency_ms;
                size_t failures = 0;
            };
            std::atomic<size_t> ready{0};
            std::atomic<bool> start{false};
            std::vector<WorkerResult> workers(thread_count);
            std::vector<std::thread> threads;
            threads.reserve(thread_count);
            for (size_t worker = 0; worker < thread_count; ++worker) {
                threads.emplace_back([&, worker] {
                    WorkerResult& result = workers[worker];
                    result.latency_ms.reserve(iterations_per_thread);
                    ready.fetch_add(1, std::memory_order_release);
                    while (!start.load(std::memory_order_acquire))
                        std::this_thread::yield();

                    result.start = std::chrono::steady_clock::now();
                    for (size_t iteration = 0;
                         iteration < iterations_per_thread; ++iteration) {
                        const size_t target_index = rotate_layers
                            ? (worker + iteration) % targets.size()
                            : 0;
                        const auto before = std::chrono::steady_clock::now();
                        const std::string digest =
                            read_record_digest(targets[target_index]);
                        const auto after = std::chrono::steady_clock::now();
                        result.latency_ms.push_back(
                            std::chrono::duration<double, std::milli>(
                                after - before).count());
                        if (digest != targets[target_index].expected_digest)
                            ++result.failures;
                    }
                    result.finish = std::chrono::steady_clock::now();
                });
            }

            while (ready.load(std::memory_order_acquire) != thread_count)
                std::this_thread::yield();
            start.store(true, std::memory_order_release);
            for (auto& thread : threads) thread.join();

            ProfileRow row;
            row.thread_count = thread_count;
            row.target_mode = rotate_layers ? "shared_three_layer_set"
                                            : "one_hot_layer";
            auto earliest = workers.front().start;
            auto latest = workers.front().finish;
            for (const auto& worker : workers) {
                earliest = std::min(earliest, worker.start);
                latest = std::max(latest, worker.finish);
                row.failures += worker.failures;
                row.latency_ms.insert(row.latency_ms.end(),
                                      worker.latency_ms.begin(),
                                      worker.latency_ms.end());
            }
            const double elapsed_seconds =
                std::chrono::duration<double>(latest - earliest).count();
            const size_t operation_count =
                thread_count * iterations_per_thread;
            row.throughput_ops_per_second = elapsed_seconds > 0.0
                ? static_cast<double>(operation_count) / elapsed_seconds
                : 0.0;
            EXPECT_EQ(row.failures, 0U)
                << row.target_mode << " threads=" << thread_count;
            rows.push_back(std::move(row));
        }
    }

    const std::filesystem::path output_path(output_value);
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path());
    std::ofstream csv(output_path);
    ASSERT_TRUE(csv.good()) << output_path.string();
    csv << "target_mode,threads,iterations_per_thread,"
           "throughput_open_read_ops_per_second,latency_samples,"
           "p50_ms,p95_ms,p99_ms,min_ms,max_ms,failures\n"
        << std::fixed << std::setprecision(3);
    for (const auto& row : rows) {
        const auto [minimum, maximum] = std::minmax_element(
            row.latency_ms.begin(), row.latency_ms.end());
        csv << row.target_mode << ',' << row.thread_count << ','
            << iterations_per_thread << ','
            << row.throughput_ops_per_second << ','
            << row.latency_ms.size() << ','
            << nearest_rank_percentile_ms(row.latency_ms, 50, 100) << ','
            << nearest_rank_percentile_ms(row.latency_ms, 95, 100) << ','
            << nearest_rank_percentile_ms(row.latency_ms, 99, 100) << ','
            << (row.latency_ms.empty() ? 0.0 : *minimum) << ','
            << (row.latency_ms.empty() ? 0.0 : *maximum) << ','
            << row.failures << '\n';
    }
    EXPECT_TRUE(csv.good());
}
