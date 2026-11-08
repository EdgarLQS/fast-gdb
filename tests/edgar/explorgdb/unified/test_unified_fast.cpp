// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include "unified.h"

#include "adaptive_reader.h"
#include "test_paths.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#endif

namespace {

using namespace fast_gdb::unified;

std::string fixture_path() {
    return explorgdb_test_paths::test_data_path(
        "test_data/benchmark/wide_50_gdal.gdb").string();
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(UnifiedFastFacadeTest, OpensDatasetFromGdalStylePath) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset) << dataset.error().message;

    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names) << names.error().message;
    ASSERT_FALSE(names.value().empty());
    EXPECT_EQ(dataset.value().backend_report().selected, Backend::FastGdb);
}

TEST(UnifiedFastFacadeTest, OpensUtf8PathWithChineseDirectoryAndReadsFeature) {
    explorgdb_test_paths::Utf8PathGdbFixture fixture;
    ASSERT_FALSE(fixture.error()) << fixture.error().message();

    OpenOptions options;
    options.backend = BackendPreference::FastOnly;
    auto dataset = Dataset::open(fixture.path().u8string(), options);
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto layer = dataset.value().open_layer("roads");
    ASSERT_TRUE(layer) << layer.error().message;
    auto feature = layer.value().read_by_fid(1);
    ASSERT_TRUE(feature) << feature.error().message;
    EXPECT_EQ(feature.value().fid, 1);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

TEST(UnifiedFastFacadeTest, ProjectsAttributesAndSkipsOutputGeometryDecode) {
    OpenOptions options;
    options.backend = BackendPreference::FastOnly;
    const auto source = explorgdb_test_paths::test_data_path(
        "test_data/gdb/acceptance_metadata.gdb").string();
    auto dataset = Dataset::open(source, options);
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto layer = dataset.value().open_layer_by_path("/TransportFD/roads");
    ASSERT_TRUE(layer) << layer.error().message;

    const auto projected = std::find_if(
        layer.value().schema().fields.begin(),
        layer.value().schema().fields.end(),
        [](const FieldDefinition& field) { return field.name == "road_id"; });
    ASSERT_NE(projected, layer.value().schema().fields.end());
    const auto projected_index = static_cast<std::size_t>(
        projected - layer.value().schema().fields.begin());

    Query query;
    query.projected_fields = {"road_id"};
    query.include_geometry = false;
    auto cursor_result = layer.value().open_cursor(query);
    ASSERT_TRUE(cursor_result) << cursor_result.error().message;
    auto cursor = std::move(cursor_result).value();
    auto next = cursor.next();
    ASSERT_TRUE(next) << next.error().message;
    ASSERT_TRUE(next.value());
    EXPECT_EQ(next.value()->geometry.state, GeometryState::NotRequested);
    ASSERT_EQ(next.value()->fields.size(), layer.value().schema().fields.size());
    EXPECT_EQ(next.value()->fields[projected_index].state, ValueState::Value);
    for (std::size_t index = 0; index < next.value()->fields.size(); ++index) {
        if (index != projected_index) {
            EXPECT_EQ(next.value()->fields[index].state, ValueState::Unset);
        }
    }
}

TEST(UnifiedFastFacadeTest,
     ReusesCatalogSchemaAndQueryPreparationWithinGeneration) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names) << names.error().message;
    ASSERT_FALSE(names.value().empty());

    Query query;
    const LayerSchema* first_schema = nullptr;
    {
        auto first_layer = dataset.value().open_layer(names.value().front());
        ASSERT_TRUE(first_layer) << first_layer.error().message;
        first_schema = &first_layer.value().schema();

        const auto numeric_field = std::find_if(
            first_schema->fields.begin(), first_schema->fields.end(),
            [](const FieldDefinition& field) {
                return field.type == FieldType::Integer ||
                       field.type == FieldType::Integer64 ||
                       field.type == FieldType::Real;
            });
        ASSERT_NE(numeric_field, first_schema->fields.end());
        query.attribute_filter = numeric_field->name + " >= 0";

        auto first_cursor_result = first_layer.value().open_cursor(query);
        ASSERT_TRUE(first_cursor_result)
            << first_cursor_result.error().message;
        auto first_cursor = std::move(first_cursor_result).value();
        EXPECT_FALSE(first_cursor.query_report().query_preparation_cache_hit);
        first_cursor.close();
    }

    auto second_layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(second_layer) << second_layer.error().message;
    EXPECT_EQ(first_schema, &second_layer.value().schema());

    auto second_cursor_result = second_layer.value().open_cursor(query);
    ASSERT_TRUE(second_cursor_result) << second_cursor_result.error().message;
    auto second_cursor = std::move(second_cursor_result).value();
    EXPECT_TRUE(second_cursor.query_report().query_preparation_cache_hit);
    second_cursor.close();

    auto refreshed = dataset.value().refresh();
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    auto refreshed_layer =
        refreshed.value().open_layer(names.value().front());
    ASSERT_TRUE(refreshed_layer) << refreshed_layer.error().message;
    auto refreshed_cursor_result =
        refreshed_layer.value().open_cursor(query);
    ASSERT_TRUE(refreshed_cursor_result)
        << refreshed_cursor_result.error().message;
    auto refreshed_cursor = std::move(refreshed_cursor_result).value();
    EXPECT_FALSE(refreshed_cursor.query_report().query_preparation_cache_hit);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(UnifiedFastFacadeTest, RejectsInvalidQueryBeforeBackendSelection) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset);
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer);

    Query query;
    query.attribute_filter.assign(64U * 1024U + 1U, 'x');
    auto cursor = layer.value().open_cursor(query);
    EXPECT_FALSE(cursor);
    EXPECT_EQ(cursor.error().code, ErrorCode::InvalidRequest);

#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
    OpenOptions options;
    options.backend = BackendPreference::GdalOnly;
    auto gdal_dataset = Dataset::open(fixture_path(), options);
    ASSERT_TRUE(gdal_dataset);
    auto gdal_layer = gdal_dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(gdal_layer);
    Query invalid_projection;
    invalid_projection.projected_fields = {"missing_field"};
    auto gdal_cursor = gdal_layer.value().open_cursor(invalid_projection);
    EXPECT_FALSE(gdal_cursor);
    EXPECT_EQ(gdal_cursor.error().code, ErrorCode::InvalidRequest);
#endif
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(UnifiedFastFacadeTest, BoundsFidAscendingCandidateMemory) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset);
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer);

    Query query;
    query.order = ResultOrder::FidAscending;
    query.max_ordered_fid_bytes = sizeof(Fid);
    auto cursor = layer.value().open_cursor(query);
    EXPECT_FALSE(cursor);
    EXPECT_EQ(cursor.error().code, ErrorCode::ResultLimitExceeded);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

    auto opened = root.value().open_group("transportfd");
    ASSERT_TRUE(opened) << opened.error().message;
    auto layers = opened.value().layers();
    ASSERT_TRUE(layers);
    EXPECT_NE(std::find_if(
        layers.value().begin(), layers.value().end(),
        [](const LayerInfo& layer) { return layer.name == "roads"; }),
        layers.value().end());

    auto roads = dataset.value().open_layer_by_path("/transportfd/ROADS");
    ASSERT_TRUE(roads) << roads.error().message;

    auto named = dataset.value().open_layer("ALL_FIELD_TYPES");
    ASSERT_TRUE(named) << named.error().message;
}

TEST(UnifiedFastFacadeTest, ReadByFidHandlesSparseAndDeletedRows) {
    const auto source = explorgdb_test_paths::test_data_path(
        "test_data/gdb/acceptance_metadata.gdb").string();
    auto dataset = Dataset::open(source);
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto layer = dataset.value().open_layer("parcels");
    ASSERT_TRUE(layer) << layer.error().message;

    for (const Fid fid : {1, 8, 20, 24, 25, 29}) {
        SCOPED_TRACE(fid);
        const auto feature = layer.value().read_by_fid(fid);
        ASSERT_TRUE(feature) << feature.error().message;
        EXPECT_EQ(feature.value().fid, fid);
    }
    for (const Fid deleted_fid : {9, 19, 30}) {
        SCOPED_TRACE(deleted_fid);
        const auto feature = layer.value().read_by_fid(deleted_fid);
        ASSERT_FALSE(feature);
        EXPECT_EQ(feature.error().code, ErrorCode::FeatureNotFound);
    }
}

TEST(UnifiedFastFacadeTest, ReadsDenseV4GdalReferenceThroughUnifiedApi) {
    const auto source = explorgdb_test_paths::test_data_path(
        "test_data/gdb/synthetic_v4_dense_gdal_reference.gdb").string();
    OpenOptions options;
    options.backend = BackendPreference::FastOnly;
    auto dataset = Dataset::open(source, options);
    ASSERT_TRUE(dataset) << dataset.error().message;

    auto layer = dataset.value().open_layer("v4probe");
    ASSERT_TRUE(layer) << layer.error().message;
    EXPECT_EQ(layer.value().schema().name, "v4probe");
    const auto name_field = std::find_if(
        layer.value().schema().fields.begin(),
        layer.value().schema().fields.end(),
        [](const FieldDefinition& field) { return field.name == "name"; });
    const auto wide_field = std::find_if(
        layer.value().schema().fields.begin(),
        layer.value().schema().fields.end(),
        [](const FieldDefinition& field) { return field.name == "wide_value"; });
    ASSERT_NE(name_field, layer.value().schema().fields.end());
    ASSERT_NE(wide_field, layer.value().schema().fields.end());
    const auto name_index = static_cast<std::size_t>(
        name_field - layer.value().schema().fields.begin());
    const auto wide_index = static_cast<std::size_t>(
        wide_field - layer.value().schema().fields.begin());

    for (Fid fid = 1; fid <= 4; ++fid) {
        const auto feature = layer.value().read_by_fid(fid);
        ASSERT_TRUE(feature) << feature.error().message;
        EXPECT_EQ(feature.value().fid, fid);
        ASSERT_TRUE(feature.value().fields[name_index].value.has_value());
        ASSERT_TRUE(feature.value().fields[wide_index].value.has_value());
        EXPECT_EQ(std::get<std::string>(
                      *feature.value().fields[name_index].value),
                  "row-" + std::to_string(fid - 1));
        EXPECT_EQ(std::get<std::int64_t>(
                      *feature.value().fields[wide_index].value),
                  1099511627776LL + fid - 1);
    }

    Query spatial_query;
    spatial_query.spatial_filter = Envelope{1.5, 1.5, 2.5, 2.5};
    auto spatial_result = layer.value().read_all(spatial_query);
    ASSERT_TRUE(spatial_result) << spatial_result.error().message;
    ASSERT_EQ(spatial_result.value().features.size(), 1u);
    EXPECT_EQ(spatial_result.value().features.front().fid, 3);
}

TEST(UnifiedFastFacadeTest, ExposesWorkspaceAndLayerMetadataSnapshots) {
    const auto source = explorgdb_test_paths::test_data_path(
        "test_data/gdb/acceptance_metadata.gdb").string();
    auto dataset = Dataset::open(source);
    ASSERT_TRUE(dataset) << dataset.error().message;

    auto workspace = dataset.value().metadata();
    ASSERT_TRUE(workspace) << workspace.error().message;
    EXPECT_EQ(workspace.value().domains.source, MetadataSource::FastGdb);
    ASSERT_EQ(workspace.value().domains.status, MetadataStatus::Available);
    const auto status_domain = std::find_if(
        workspace.value().domains.value.begin(),
        workspace.value().domains.value.end(),
        [](const DomainMetadata& domain) {
            return domain.name == "road_status_domain";
        });
    ASSERT_NE(status_domain, workspace.value().domains.value.end());
    EXPECT_FALSE(status_domain->raw_xml.empty());
    EXPECT_EQ(status_domain->coded_values.size(), 3U);

    EXPECT_EQ(workspace.value().feature_datasets.status,
              MetadataStatus::Available);
    EXPECT_EQ(workspace.value().catalog_objects.status,
              CatalogInventoryStatus::Complete);
    EXPECT_EQ(workspace.value().catalog_objects.active_rows, 19U);
    const auto catalog_roads = std::find_if(
        workspace.value().catalog_objects.objects.begin(),
        workspace.value().catalog_objects.objects.end(),
        [](const CatalogObjectSummaryMetadata& object) {
            return object.name == "roads";
        });
    ASSERT_NE(catalog_roads,
              workspace.value().catalog_objects.objects.end());
    EXPECT_EQ(catalog_roads->path, "\\TransportFD\\roads");
    EXPECT_EQ(catalog_roads->type_name, "Feature Class");
    EXPECT_EQ(catalog_roads->dataset_type, "esriDTFeatureClass");
    EXPECT_EQ(catalog_roads->geometry_type, "esriGeometryPolyline");
    EXPECT_EQ(catalog_roads->advanced_kind, AdvancedObjectKind::None);
    EXPECT_TRUE(catalog_roads->physical_table_resolves);

    const auto roads_object = dataset.value().catalog_object_by_uuid(
        "{E32B8E0E-76FF-4792-A874-31FD8B282224}");
    ASSERT_EQ(roads_object.status, CatalogObjectLookupStatus::Available)
        << roads_object.diagnostic;
    ASSERT_TRUE(roads_object.object.has_value());
    EXPECT_NE(roads_object.object->raw_definition.find("DEFeatureClassInfo"),
              std::string::npos);
    const auto roads_definition = std::find_if(
        roads_object.object->attributes.begin(),
        roads_object.object->attributes.end(),
        [](const CatalogObjectAttributeMetadata& attribute) {
            return attribute.name == "Definition";
        });
    ASSERT_NE(roads_definition, roads_object.object->attributes.end());
    ASSERT_TRUE(roads_definition->value.has_value());
    const auto* roads_definition_text =
        std::get_if<std::string>(&*roads_definition->value);
    ASSERT_NE(roads_definition_text, nullptr);
    EXPECT_EQ(*roads_definition_text, roads_object.object->raw_definition);
    const auto roads_properties = std::find_if(
        roads_object.object->attributes.begin(),
        roads_object.object->attributes.end(),
        [](const CatalogObjectAttributeMetadata& attribute) {
            return attribute.name == "Properties";
        });
    ASSERT_NE(roads_properties, roads_object.object->attributes.end());
    ASSERT_TRUE(roads_properties->value.has_value());
    const auto* roads_properties_value =
        std::get_if<std::int32_t>(&*roads_properties->value);
    ASSERT_NE(roads_properties_value, nullptr);
    EXPECT_EQ(*roads_properties_value, 1);
    EXPECT_EQ(dataset.value().catalog_object_by_uuid(
                  "{00000000-0000-0000-0000-000000000000}").status,
              CatalogObjectLookupStatus::NotFound);

    EXPECT_EQ(workspace.value().relationships.source, MetadataSource::FastGdb);
    ASSERT_EQ(workspace.value().relationships.status, MetadataStatus::Available);
    const auto road_relationship = std::find_if(
        workspace.value().relationships.value.begin(),
        workspace.value().relationships.value.end(),
        [](const RelationshipClassMetadata& relationship) {
            return relationship.name == "roads_inspections_rel";
        });
    ASSERT_NE(road_relationship, workspace.value().relationships.value.end());
    EXPECT_EQ(road_relationship->origin_class_names,
              (std::vector<std::string>{"roads"}));
    EXPECT_EQ(road_relationship->destination_class_names,
              (std::vector<std::string>{"road_inspections"}));
    EXPECT_EQ(road_relationship->origin_primary_key, "OBJECTID");
    EXPECT_EQ(road_relationship->destination_foreign_key, "road_id");
    EXPECT_EQ(workspace.value().attachments.source, MetadataSource::FastGdb);
    EXPECT_EQ(workspace.value().attachments.status, MetadataStatus::Empty);
    EXPECT_TRUE(workspace.value().attachments.value.empty());

    EXPECT_NE(std::find_if(
        workspace.value().feature_datasets.value.begin(),
        workspace.value().feature_datasets.value.end(),
        [](const FeatureDatasetMetadata& feature_dataset) {
            return feature_dataset.name == "TransportFD" &&
                   std::any_of(feature_dataset.members.begin(),
                               feature_dataset.members.end(),
                       [](const LayerInfo& member) {
                           return member.name == "roads";
                       });
        }), workspace.value().feature_datasets.value.end());
    EXPECT_NE(std::find_if(
        workspace.value().raw_payloads.begin(),
        workspace.value().raw_payloads.end(),
        [](const RawMetadataPayload& payload) {
            return payload.kind == "feature_dataset_definition" &&
                   !payload.text.empty();
        }), workspace.value().raw_payloads.end());

    auto roads = dataset.value().open_layer("roads");
    ASSERT_TRUE(roads) << roads.error().message;
    auto layer_metadata = roads.value().metadata();
    ASSERT_TRUE(layer_metadata) << layer_metadata.error().message;
    EXPECT_EQ(layer_metadata.value().definition.status,
              MetadataStatus::Available);
    EXPECT_EQ(layer_metadata.value().definition.source,
              MetadataSource::FastGdb);
    EXPECT_EQ(layer_metadata.value().field_domains.status,
              MetadataStatus::Available);
    EXPECT_EQ(layer_metadata.value().subtypes.status,
              MetadataStatus::Available);
    EXPECT_EQ(layer_metadata.value().relationships.status,
              MetadataStatus::Available);
    EXPECT_EQ(layer_metadata.value().attachments.status,
              MetadataStatus::Empty);
    EXPECT_TRUE(layer_metadata.value().attachments.value.empty());
    EXPECT_NE(std::find_if(
        layer_metadata.value().raw_payloads.begin(),
        layer_metadata.value().raw_payloads.end(),
        [](const RawMetadataPayload& payload) {
            return payload.kind == "layer_definition" &&
                   !payload.text.empty();
        }), layer_metadata.value().raw_payloads.end());
    const auto status_binding = std::find_if(
        layer_metadata.value().field_domains.value.begin(),
        layer_metadata.value().field_domains.value.end(),
        [](const FieldDomainBinding& binding) {
            return binding.field_name == "status";
        });
    ASSERT_NE(status_binding,
              layer_metadata.value().field_domains.value.end());
    ASSERT_TRUE(status_binding->domain.has_value());
    EXPECT_EQ(status_binding->domain->name, "road_status_domain");
    EXPECT_NE(std::find_if(
        layer_metadata.value().relationships.value.begin(),
        layer_metadata.value().relationships.value.end(),
        [](const RelationshipClassMetadata& relationship) {
            return relationship.name == "roads_inspections_rel";
        }), layer_metadata.value().relationships.value.end());
    ASSERT_FALSE(layer_metadata.value().subtypes.value.empty());
    const auto highway = std::find_if(
        layer_metadata.value().subtypes.value.begin(),
        layer_metadata.value().subtypes.value.end(),
        [](const SubtypeMetadata& subtype) {
            return subtype.name == "Highway";
        });
    ASSERT_NE(highway, layer_metadata.value().subtypes.value.end());
    const auto status_override = std::find_if(
        highway->field_overrides.begin(), highway->field_overrides.end(),
        [](const SubtypeFieldOverride& field) {
            return field.field_name == "status";
        });
    ASSERT_NE(status_override, highway->field_overrides.end());
    EXPECT_EQ(status_override->domain_name, "road_status_domain");
    const auto speed_override = std::find_if(
        highway->field_overrides.begin(), highway->field_overrides.end(),
        [](const SubtypeFieldOverride& field) {
            return field.field_name == "speed_limit";
        });
    ASSERT_NE(speed_override, highway->field_overrides.end());
    EXPECT_EQ(speed_override->domain_name, "speed_range_domain");
    ASSERT_TRUE(speed_override->domain.has_value());
    EXPECT_FALSE(speed_override->domain->min_value.empty());
    EXPECT_FALSE(speed_override->domain->max_value.empty());
}

TEST(UnifiedFastFacadeTest, ReadsBinaryFieldsUnderPayloadBudget) {
    const auto source = explorgdb_test_paths::test_data_path(
        "test_data/gdb/acceptance_metadata.gdb").string();
    OpenOptions options;
    options.backend = BackendPreference::FastOnly;
    auto dataset = Dataset::open(source, options);
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto layer = dataset.value().open_layer("all_field_types");
    ASSERT_TRUE(layer) << layer.error().message;

    const auto null_value = layer.value().read_binary_field_by_fid(
        1, "blob_fld", 64);
    ASSERT_TRUE(null_value) << null_value.error().message;
    EXPECT_TRUE(null_value.value().is_null);

    const auto empty_value = layer.value().read_binary_field_by_fid(
        2, "BLOB_FLD", 64);
    ASSERT_TRUE(empty_value) << empty_value.error().message;
    EXPECT_FALSE(empty_value.value().is_null);
    EXPECT_EQ(empty_value.value().byte_size, 0U);
    EXPECT_TRUE(empty_value.value().bytes.empty());

    const auto small_value = layer.value().read_binary_field_by_fid(
        3, "blob_fld", 64);
    ASSERT_TRUE(small_value) << small_value.error().message;
    EXPECT_EQ(small_value.value().byte_size, 6U);
    EXPECT_EQ(small_value.value().bytes,
              (std::vector<std::uint8_t>{0x5c, 0x78, 0x30, 0x30, 0x01, 0x02}));

    const auto limited = layer.value().read_binary_field_by_fid(
        6, "blob_fld", 64);
    EXPECT_FALSE(limited);
    EXPECT_EQ(limited.error().code, ErrorCode::ResultLimitExceeded);

    const auto missing_field = layer.value().read_binary_field_by_fid(
        3, "missing_blob", 64);
    EXPECT_FALSE(missing_field);
    EXPECT_EQ(missing_field.error().code, ErrorCode::InvalidRequest);
    const auto wrong_type = layer.value().read_binary_field_by_fid(
        3, "text_fld", 64);
    EXPECT_FALSE(wrong_type);
    EXPECT_EQ(wrong_type.error().code, ErrorCode::InvalidRequest);
    const auto missing_feature = layer.value().read_binary_field_by_fid(
        1000, "blob_fld", 64);
    EXPECT_FALSE(missing_feature);
    EXPECT_EQ(missing_feature.error().code, ErrorCode::FeatureNotFound);
}

TEST(UnifiedFastFacadeTest, ReadsArcGisAttachmentPayloadsWhenConfigured) {
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

    OpenOptions options;
    options.backend = BackendPreference::FastOnly;
    auto dataset = Dataset::open(dataset_value, options);
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto layer = dataset.value().open_layer(
        "attachment_features__ATTACH");
    ASSERT_TRUE(layer) << layer.error().message;

    const std::filesystem::path expected_path(expected_value);
    std::ifstream expected_stream{expected_path};
    ASSERT_TRUE(expected_stream.is_open()) << expected_path.string();
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(expected_stream, line)));
    EXPECT_EQ(line,
              "row_slot\tobject_id\tparent_key\tparent_globalid\t"
              "attachment_name\tdata_size\tsha256\tpayload_path");

    std::size_t checked_rows = 0;
    while (std::getline(expected_stream, line)) {
        if (line.empty()) continue;
        std::vector<std::string> columns;
        std::stringstream row_stream(line);
        std::string column;
        while (std::getline(row_stream, column, '\t')) {
            columns.push_back(column);
        }
        ASSERT_EQ(columns.size(), 8U) << line;

        const std::uint64_t row_slot = std::stoull(columns[0]);
        const Fid object_id = static_cast<Fid>(std::stoull(columns[1]));
        const std::uint64_t expected_size = std::stoull(columns[5]);
        EXPECT_EQ(object_id, row_slot + 1U);
        const auto payload_path = expected_path.parent_path() /
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

        const auto binary = layer.value().read_binary_field_by_fid(
            object_id, "DATA", expected_size);
        ASSERT_TRUE(binary) << binary.error().message;
        EXPECT_FALSE(binary.value().is_null);
        EXPECT_EQ(binary.value().byte_size, expected_size);
        EXPECT_EQ(binary.value().bytes, expected_bytes);

        if (expected_size > 65536U) {
            const auto limited = layer.value().read_binary_field_by_fid(
                object_id, "DATA", 65536U);
            EXPECT_FALSE(limited);
            EXPECT_EQ(limited.error().code, ErrorCode::ResultLimitExceeded);
        }
        ++checked_rows;
    }
    EXPECT_EQ(checked_rows, 4U);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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
    EXPECT_EQ(limited.error().code, ErrorCode::ResultLimitExceeded)
        << limited.error().message;

    auto native = extensions.value().read_native_by_fid(1);
    ASSERT_TRUE(native) << native.error().message;
    EXPECT_EQ(native.value().row_slot, 0U);
    EXPECT_FALSE(native.value().raw_record.empty());
    EXPECT_FALSE(native.value().descriptors.empty());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(UnifiedFastFacadeTest, SharesCoordinatorAndFailsClosedDuringWriter) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset);
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer);
    auto extensions = layer.value().fast_extensions();
    ASSERT_TRUE(extensions);

    explorgdb::InProcessGdbCoordinator coordinator;
    auto update = coordinator.prepare_external_update(
        fixture_path(), std::chrono::milliseconds(0));
    ASSERT_TRUE(update);
    ASSERT_EQ(update.token.notify_update_opened(),
              explorgdb::CoordinationStatus::Ok);

    auto cursor = layer.value().open_cursor();
    EXPECT_FALSE(cursor);
    EXPECT_EQ(cursor.error().code, ErrorCode::SourceBusy);
    auto native = extensions.value().read_native_by_fid(1);
    EXPECT_FALSE(native);
    EXPECT_EQ(native.error().code, ErrorCode::SourceBusy);
    EXPECT_EQ(update.token.notify_update_closed(true),
              explorgdb::CoordinationStatus::Ok);

    auto expired = layer.value().open_cursor();
    EXPECT_FALSE(expired);
    EXPECT_EQ(expired.error().code, ErrorCode::SourceChanged);

    auto reopened = Dataset::open(fixture_path());
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto reopened_layer = reopened.value().open_layer(names.value().front());
    ASSERT_TRUE(reopened_layer) << reopened_layer.error().message;
    EXPECT_TRUE(reopened_layer.value().open_cursor());
}

TEST(UnifiedFastFacadeTest, RefreshBlocksActiveCursorAndExpiresOldHandles) {
    auto dataset = Dataset::open(fixture_path());
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names) << names.error().message;
    ASSERT_FALSE(names.value().empty());
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer) << layer.error().message;
    const auto old_generation = dataset.value().consistency_report().generation;

    auto cursor_result = layer.value().open_cursor();
    ASSERT_TRUE(cursor_result) << cursor_result.error().message;
    auto cursor = std::move(cursor_result).value();
    auto blocked = dataset.value().refresh();
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code, ErrorCode::SourceBusy);
    cursor.close();

    explorgdb::InProcessGdbCoordinator coordinator;
    auto update = coordinator.prepare_external_update(
        fixture_path(), std::chrono::milliseconds(0));
    ASSERT_TRUE(update);
    ASSERT_EQ(update.token.notify_update_opened(),
              explorgdb::CoordinationStatus::Ok);
    ASSERT_EQ(update.token.notify_update_closed(true),
              explorgdb::CoordinationStatus::Ok);

    auto refreshed = dataset.value().refresh();
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    EXPECT_GT(refreshed.value().consistency_report().generation,
              old_generation);

    auto expired_names = dataset.value().layer_names();
    EXPECT_FALSE(expired_names);
    EXPECT_EQ(expired_names.error().code, ErrorCode::ReaderExpired);
    auto expired_cursor = layer.value().open_cursor();
    EXPECT_FALSE(expired_cursor);
    EXPECT_EQ(expired_cursor.error().code, ErrorCode::ReaderExpired);

    auto refreshed_layer = refreshed.value().open_layer(names.value().front());
    ASSERT_TRUE(refreshed_layer) << refreshed_layer.error().message;
    auto fresh_cursor = refreshed_layer.value().open_cursor();
    EXPECT_TRUE(fresh_cursor) << fresh_cursor.error().message;
}

TEST(UnifiedFastFacadeTest, FailedRefreshLeavesDatasetExpired) {
    namespace fs = std::filesystem;
    const fs::path source = explorgdb_test_paths::test_data_path(
        "test_data/benchmark/wide_50_gdal.gdb");
    const fs::path copy = fs::temp_directory_path() /
        "fast_gdb_refresh_failure.gdb";
    const fs::path parked = fs::temp_directory_path() /
        "fast_gdb_refresh_failure.parked";
    struct Cleanup {
        fs::path first;
        fs::path second;
        ~Cleanup() {
            std::error_code error;
            fs::remove_all(first, error);
            fs::remove_all(second, error);
        }
    } cleanup{copy, parked};
    std::error_code error;
    fs::remove_all(copy, error);
    error.clear();
    fs::remove_all(parked, error);
    error.clear();
    fs::copy(source, copy, fs::copy_options::recursive, error);
    ASSERT_FALSE(error) << error.message();

    OpenOptions options;
    options.backend = BackendPreference::FastOnly;
    auto dataset = Dataset::open(copy.string(), options);
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names) << names.error().message;
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer) << layer.error().message;

    fs::rename(copy, parked, error);
    ASSERT_FALSE(error) << error.message();
    auto refreshed = dataset.value().refresh();
    EXPECT_FALSE(refreshed);
    EXPECT_EQ(refreshed.error().code, ErrorCode::SourceNotFound);
    fs::rename(parked, copy, error);
    ASSERT_FALSE(error) << error.message();

    auto stale_names = dataset.value().layer_names();
    EXPECT_FALSE(stale_names);
    EXPECT_EQ(stale_names.error().code, ErrorCode::ReaderExpired);
    auto stale_cursor = layer.value().open_cursor();
    EXPECT_FALSE(stale_cursor);
    EXPECT_EQ(stale_cursor.error().code, ErrorCode::ReaderExpired);
}

#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
TEST(UnifiedGdalFacadeTest, CurvePolicyPreservesAndLinearizesWithProvenance) {
    const char* path = std::getenv("FAST_GDB_CURVE_DATASET");
    if (path == nullptr || !std::filesystem::is_directory(path)) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to a curve-bearing .gdb";
    }

    GDALAllRegister();
    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    auto* reference = static_cast<GDALDataset*>(GDALOpenEx(
        path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
        allowed_drivers, nullptr, nullptr));
    ASSERT_NE(reference, nullptr);
    std::string curve_layer_name;
    Fid curve_fid = -1;
    for (int index = 0;
         index < reference->GetLayerCount() && curve_fid < 0; ++index) {
        OGRLayer* layer = reference->GetLayer(index);
        ASSERT_NE(layer, nullptr);
        layer->ResetReading();
        while (OGRFeature* feature = layer->GetNextFeature()) {
            const OGRGeometry* geometry = feature->GetGeometryRef();
            if (geometry != nullptr && geometry->hasCurveGeometry(TRUE) &&
                feature->GetFID() > 0) {
                curve_layer_name = layer->GetName();
                curve_fid = static_cast<Fid>(feature->GetFID());
                OGRFeature::DestroyFeature(feature);
                break;
            }
            OGRFeature::DestroyFeature(feature);
        }
    }
    GDALClose(reference);
    ASSERT_GT(curve_fid, 0) << "GDAL found no curved feature in the fixture";

    auto read_fid = [curve_fid](Layer& layer,
                                const Query& query) -> std::optional<Feature> {
        auto cursor_result = layer.open_cursor(query);
        if (!cursor_result) {
            ADD_FAILURE() << cursor_result.error().message;
            return std::nullopt;
        }
        auto cursor = std::move(cursor_result).value();
        for (;;) {
            auto next = cursor.next();
            if (!next) {
                ADD_FAILURE() << next.error().message;
                return std::nullopt;
            }
            if (!next.value()) return std::nullopt;
            if (next.value()->fid == curve_fid) return *next.value();
            if (next.value()->fid > curve_fid) return std::nullopt;
        }
    };

    OpenOptions fast_options;
    fast_options.backend = BackendPreference::FastOnly;
    auto fast_dataset = Dataset::open(path, fast_options);
    ASSERT_TRUE(fast_dataset) << fast_dataset.error().message;
    auto fast_layer = fast_dataset.value().open_layer(curve_layer_name);
    ASSERT_TRUE(fast_layer) << fast_layer.error().message;

    Query preserve_query;
    preserve_query.order = ResultOrder::FidAscending;
    auto preserved = read_fid(fast_layer.value(), preserve_query);
    ASSERT_TRUE(preserved.has_value());
    EXPECT_EQ(preserved->geometry.state, GeometryState::Unsupported);
    EXPECT_TRUE(preserved->geometry.source_was_curve);
    EXPECT_EQ(preserved->geometry.source_payload_state,
              GeometrySourcePayloadState::Preserved);
    EXPECT_FALSE(preserved->geometry.source_payload.empty());
    EXPECT_EQ(preserved->geometry.backend, GeometryBackend::Reject);

    Query linearize_query = preserve_query;
    linearize_query.geometry.curve_policy = CurveReadPolicy::Linearize;
    linearize_query.geometry.max_angle_step_degrees = 2.5;
    linearize_query.geometry.max_segments_per_curve = 128;
    auto linearized = read_fid(fast_layer.value(), linearize_query);
    ASSERT_TRUE(linearized.has_value());
    EXPECT_EQ(linearized->geometry.state, GeometryState::Value);
    EXPECT_TRUE(linearized->geometry.source_was_curve);
    EXPECT_TRUE(linearized->geometry.linearized);
    EXPECT_DOUBLE_EQ(
        linearized->geometry.linearization_max_angle_step_degrees, 2.5);
    EXPECT_EQ(linearized->geometry.linearization_max_segments_per_curve,
              128u);
    EXPECT_EQ(linearized->geometry.source_payload_state,
              GeometrySourcePayloadState::Preserved);

    Query limited_query = preserve_query;
    limited_query.geometry.max_source_payload_bytes = 1;
    auto limited = read_fid(fast_layer.value(), limited_query);
    ASSERT_TRUE(limited.has_value());
    EXPECT_EQ(limited->geometry.state, GeometryState::Unsupported);
    EXPECT_TRUE(limited->geometry.source_payload.empty());
    EXPECT_EQ(limited->geometry.source_payload_state,
              GeometrySourcePayloadState::LimitExceeded);

    OpenOptions gdal_options;
    gdal_options.backend = BackendPreference::GdalOnly;
    auto gdal_dataset = Dataset::open(path, gdal_options);
    ASSERT_TRUE(gdal_dataset) << gdal_dataset.error().message;
    auto gdal_layer = gdal_dataset.value().open_layer(curve_layer_name);
    ASSERT_TRUE(gdal_layer) << gdal_layer.error().message;
    auto gdal_curve = read_fid(gdal_layer.value(), preserve_query);
    ASSERT_TRUE(gdal_curve.has_value());
    EXPECT_TRUE(gdal_curve->geometry.source_was_curve);
    EXPECT_FALSE(gdal_curve->geometry.wkb.empty());
    EXPECT_EQ(gdal_curve->geometry.backend, GeometryBackend::Gdal);

    Query gdal_linearize_query = linearize_query;
    gdal_linearize_query.geometry.max_segments_per_curve = 0;
    auto gdal_linear = read_fid(gdal_layer.value(), gdal_linearize_query);
    ASSERT_TRUE(gdal_linear.has_value());
    EXPECT_TRUE(gdal_linear->geometry.linearized);
    EXPECT_EQ(gdal_linear->geometry.backend, GeometryBackend::Gdal);
    EXPECT_DOUBLE_EQ(
        gdal_linear->geometry.linearization_max_angle_step_degrees, 2.5);

    auto gdal_limited = read_fid(gdal_layer.value(), linearize_query);
    ASSERT_TRUE(gdal_limited.has_value());
    EXPECT_EQ(gdal_limited->geometry.state, GeometryState::Unsupported);
    EXPECT_NE(gdal_limited->geometry.diagnostic.find("segment-count"),
              std::string::npos);
}

TEST(UnifiedGdalFacadeTest, SuppliedSparseObjectIdsMapThroughPublicApi) {
    const char* path = std::getenv("FAST_GDB_CURVE_DATASET");
    if (path == nullptr || !std::filesystem::is_directory(path)) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to testcurve.gdb";
    }

    GDALAllRegister();
    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    auto close_dataset = [](GDALDataset* dataset) {
        if (dataset != nullptr) GDALClose(dataset);
    };
    std::unique_ptr<GDALDataset, decltype(close_dataset)> reference(
        static_cast<GDALDataset*>(GDALOpenEx(
            path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
            allowed_drivers, nullptr, nullptr)),
        close_dataset);
    ASSERT_NE(reference, nullptr) << path;

    OGRLayer* reference_layer = reference->GetLayerByName("Point_FIDGap");
    if (reference_layer == nullptr) {
        GTEST_SKIP() << "FAST_GDB_CURVE_DATASET has no Point_FIDGap layer";
    }

    struct ExpectedRow {
        Fid fid = -1;
        std::string name;
        Envelope point_envelope;
    };
    std::vector<ExpectedRow> expected;
    reference_layer->ResetReading();
    while (OGRFeature* raw_feature = reference_layer->GetNextFeature()) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
            feature(raw_feature, OGRFeature::DestroyFeature);
        const GIntBig gdal_fid = feature->GetFID();
        ASSERT_GT(gdal_fid, 0);
        const OGRGeometry* geometry = feature->GetGeometryRef();
        ASSERT_NE(geometry, nullptr);
        OGREnvelope envelope;
        geometry->getEnvelope(&envelope);
        expected.push_back({
            static_cast<Fid>(gdal_fid), feature->GetFieldAsString("Name"),
            {envelope.MinX, envelope.MinY, envelope.MaxX, envelope.MaxY}});
    }
    ASSERT_EQ(expected.size(), 4U);
    EXPECT_EQ((std::vector<Fid>{expected[0].fid, expected[1].fid,
                                expected[2].fid, expected[3].fid}),
              (std::vector<Fid>{1, 2, 6, 7}));

    OpenOptions options;
    options.backend = BackendPreference::FastOnly;
    auto dataset = Dataset::open(path, options);
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto layer = dataset.value().open_layer("Point_FIDGap");
    ASSERT_TRUE(layer) << layer.error().message;
    const auto name_field = std::find_if(
        layer.value().schema().fields.begin(),
        layer.value().schema().fields.end(),
        [](const FieldDefinition& field) { return field.name == "Name"; });
    ASSERT_NE(name_field, layer.value().schema().fields.end());
    const std::size_t name_index = static_cast<std::size_t>(
        name_field - layer.value().schema().fields.begin());

    const auto expect_feature = [&](const Feature& feature,
                                    const ExpectedRow& row) {
        EXPECT_EQ(feature.fid, row.fid);
        ASSERT_LT(name_index, feature.fields.size());
        ASSERT_EQ(feature.fields[name_index].state, ValueState::Value);
        ASSERT_TRUE(feature.fields[name_index].value.has_value());
        const auto* name =
            std::get_if<std::string>(&*feature.fields[name_index].value);
        ASSERT_NE(name, nullptr);
        EXPECT_EQ(*name, row.name);
    };

    for (const auto& row : expected) {
        SCOPED_TRACE(row.fid);
        const auto feature = layer.value().read_by_fid(row.fid);
        ASSERT_TRUE(feature) << feature.error().message;
        expect_feature(feature.value(), row);
    }
    for (const Fid missing_fid : {3, 4, 5}) {
        SCOPED_TRACE(missing_fid);
        const auto feature = layer.value().read_by_fid(missing_fid);
        ASSERT_FALSE(feature);
        EXPECT_EQ(feature.error().code, ErrorCode::FeatureNotFound);
    }

    Query ordered_query;
    ordered_query.order = ResultOrder::FidAscending;
    const auto batch = layer.value().read_all(ordered_query);
    ASSERT_TRUE(batch) << batch.error().message;
    ASSERT_EQ(batch.value().features.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        expect_feature(batch.value().features[index], expected[index]);
    }

    for (const auto& row : expected) {
        SCOPED_TRACE(row.fid);
        const double scale = std::max(
            std::abs(row.point_envelope.xmin),
            std::abs(row.point_envelope.ymin));
        const double tolerance = std::max(1e-8, scale * 1e-12);
        Query spatial_query;
        spatial_query.order = ResultOrder::FidAscending;
        spatial_query.spatial_filter = Envelope{
            row.point_envelope.xmin - tolerance,
            row.point_envelope.ymin - tolerance,
            row.point_envelope.xmax + tolerance,
            row.point_envelope.ymax + tolerance};
        const auto plan = layer.value().explain(spatial_query);
        ASSERT_TRUE(plan) << plan.error().message;
        EXPECT_EQ(plan.value().backend, Backend::FastGdb);
        EXPECT_EQ(plan.value().execution_path, "fast/spatial_bbox");

        const auto fast_window = layer.value().read_all(spatial_query);
        ASSERT_TRUE(fast_window) << fast_window.error().message;

        // Derive expected point hits from the full GDAL scan. GDAL 3.13.0's
        // direct filter disagrees with its scan for one tight window here.
        const auto& envelope = *spatial_query.spatial_filter;
        std::vector<ExpectedRow> expected_window;
        for (const auto& candidate : expected) {
            const auto& point = candidate.point_envelope;
            if (point.xmin <= envelope.xmax && point.xmax >= envelope.xmin &&
                point.ymin <= envelope.ymax && point.ymax >= envelope.ymin) {
                expected_window.push_back(candidate);
            }
        }
        ASSERT_FALSE(expected_window.empty());
        ASSERT_EQ(fast_window.value().features.size(), expected_window.size());
        for (std::size_t index = 0; index < expected_window.size(); ++index) {
            expect_feature(fast_window.value().features[index],
                           expected_window[index]);
        }
    }

    auto cursor_result = layer.value().open_cursor(ordered_query);
    ASSERT_TRUE(cursor_result) << cursor_result.error().message;
    auto cursor = std::move(cursor_result).value();
    for (const auto& row : expected) {
        const auto next = cursor.next();
        ASSERT_TRUE(next) << next.error().message;
        ASSERT_TRUE(next.value().has_value());
        expect_feature(*next.value(), row);
    }
    const auto end = cursor.next();
    ASSERT_TRUE(end) << end.error().message;
    EXPECT_FALSE(end.value().has_value());
}

TEST(UnifiedGdalFacadeTest, FrozenFastSchemaRejectsExternalFieldChanges) {
    namespace fs = std::filesystem;
    const fs::path source = explorgdb_test_paths::test_data_path(
        "test_data/gdb/acceptance_metadata.gdb");
    const fs::path copy = fs::temp_directory_path() /
        "fast_gdb_unified_schema_change.gdb";
    std::error_code error;
    fs::remove_all(copy, error);
    error.clear();
    fs::copy(source, copy, fs::copy_options::recursive, error);
    ASSERT_FALSE(error) << error.message();

    OpenOptions options;
    options.backend = BackendPreference::FastOnly;
    auto dataset = Dataset::open(copy.string(), options);
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto layer = dataset.value().open_layer("all_field_types");
    ASSERT_TRUE(layer) << layer.error().message;
    const auto original_field_count = layer.value().schema().fields.size();

    GDALAllRegister();
    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    auto* gdal_dataset = static_cast<GDALDataset*>(GDALOpenEx(
        copy.string().c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        allowed_drivers, nullptr, nullptr));
    ASSERT_NE(gdal_dataset, nullptr);
    OGRLayer* gdal_layer = gdal_dataset->GetLayerByName("all_field_types");
    ASSERT_NE(gdal_layer, nullptr);
    OGRFieldDefn added_field("added_after_snapshot", OFTInteger);
    ASSERT_EQ(gdal_layer->CreateField(&added_field), OGRERR_NONE);
    GDALClose(gdal_dataset);

    EXPECT_EQ(layer.value().schema().fields.size(), original_field_count);
    auto stale_cursor = layer.value().open_cursor();
    EXPECT_FALSE(stale_cursor);
    EXPECT_EQ(stale_cursor.error().code, ErrorCode::SchemaMismatch);

    auto refreshed = dataset.value().refresh();
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    auto refreshed_layer = refreshed.value().open_layer("all_field_types");
    ASSERT_TRUE(refreshed_layer) << refreshed_layer.error().message;
    EXPECT_EQ(refreshed_layer.value().schema().fields.size(),
              original_field_count + 1);
    auto expired_cursor = layer.value().open_cursor();
    EXPECT_FALSE(expired_cursor);
    EXPECT_EQ(expired_cursor.error().code, ErrorCode::ReaderExpired);
    auto expired_dataset_names = dataset.value().layer_names();
    EXPECT_FALSE(expired_dataset_names);
    EXPECT_EQ(expired_dataset_names.error().code, ErrorCode::ReaderExpired);
    fs::remove_all(copy, error);
}

TEST(UnifiedGdalFacadeTest,
     RefreshReadsExternalGdalAttributeAndGeometryChanges) {
    namespace fs = std::filesystem;
    const char* source = std::getenv("FAST_GDB_CURVE_DATASET");
    if (source == nullptr || !fs::is_directory(source)) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to testcurve.gdb";
    }

    const auto test_id = std::chrono::steady_clock::now()
                             .time_since_epoch().count();
    const fs::path copy = fs::temp_directory_path() /
        ("fast_gdb_unified_refresh_feature_change_" +
         std::to_string(test_id) + ".gdb");
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{copy};
    std::error_code error;
    fs::remove_all(copy, error);
    error.clear();
    fs::copy(source, copy, fs::copy_options::recursive, error);
    ASSERT_FALSE(error) << error.message();

    OpenOptions options;
    options.backend = BackendPreference::FastOnly;
    auto dataset = Dataset::open(copy.string(), options);
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto layer = dataset.value().open_layer("Point_FIDGap");
    ASSERT_TRUE(layer) << layer.error().message;
    EXPECT_EQ(layer.value().backend_report().selected, Backend::FastGdb);
    const auto name_field = std::find_if(
        layer.value().schema().fields.begin(),
        layer.value().schema().fields.end(),
        [](const FieldDefinition& field) { return field.name == "Name"; });
    ASSERT_NE(name_field, layer.value().schema().fields.end());
    const auto name_index = static_cast<std::size_t>(
        name_field - layer.value().schema().fields.begin());
    const auto original = layer.value().read_by_fid(6);
    ASSERT_TRUE(original) << original.error().message;
    ASSERT_LT(name_index, original.value().fields.size());
    ASSERT_TRUE(original.value().fields[name_index].value.has_value());
    ASSERT_EQ(std::get<std::string>(
                  *original.value().fields[name_index].value),
              "Feature_6");

    GDALAllRegister();
    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> gdal_dataset(
        static_cast<GDALDataset*>(GDALOpenEx(
            copy.string().c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
            allowed_drivers, nullptr, nullptr)),
        GDALClose);
    ASSERT_NE(gdal_dataset, nullptr);
    OGRLayer* gdal_layer = gdal_dataset->GetLayerByName("Point_FIDGap");
    ASSERT_NE(gdal_layer, nullptr);
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
        gdal_feature(gdal_layer->GetFeature(6), OGRFeature::DestroyFeature);
    ASSERT_NE(gdal_feature, nullptr);
    gdal_feature->SetField("Name", "ExternalUpdate_6");
    OGRGeometry* original_geometry = gdal_feature->GetGeometryRef();
    ASSERT_NE(original_geometry, nullptr);
    OGRPoint* updated_point = original_geometry->toPoint();
    ASSERT_NE(updated_point, nullptr);
    updated_point->setX(123456.75);
    updated_point->setY(654321.25);
    ASSERT_EQ(gdal_layer->SetFeature(gdal_feature.get()), OGRERR_NONE);

    OGRLayer* created_layer = gdal_dataset->CreateLayer(
        "ExternalRefreshLayer", nullptr, wkbNone, nullptr);
    ASSERT_NE(created_layer, nullptr);
    OGRFieldDefn value_field("value", OFTInteger);
    ASSERT_EQ(created_layer->CreateField(&value_field), OGRERR_NONE);
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
        created_feature(
            OGRFeature::CreateFeature(created_layer->GetLayerDefn()),
            OGRFeature::DestroyFeature);
    ASSERT_NE(created_feature, nullptr);
    created_feature->SetField("value", 42);
    ASSERT_EQ(created_layer->CreateFeature(created_feature.get()), OGRERR_NONE);
    gdal_dataset.reset();

    std::unique_ptr<GDALDataset, decltype(&GDALClose)>
        gdal_verification_dataset(
            static_cast<GDALDataset*>(GDALOpenEx(
                copy.string().c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                allowed_drivers, nullptr, nullptr)),
            GDALClose);
    ASSERT_NE(gdal_verification_dataset, nullptr);
    OGRLayer* gdal_verification_layer =
        gdal_verification_dataset->GetLayerByName("Point_FIDGap");
    ASSERT_NE(gdal_verification_layer, nullptr);
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
        gdal_verification_feature(
            gdal_verification_layer->GetFeature(6),
            OGRFeature::DestroyFeature);
    ASSERT_NE(gdal_verification_feature, nullptr);
    EXPECT_STREQ(gdal_verification_feature->GetFieldAsString("Name"),
                 "ExternalUpdate_6");
    const OGRGeometry* gdal_verification_geometry =
        gdal_verification_feature->GetGeometryRef();
    ASSERT_NE(gdal_verification_geometry, nullptr);
    const OGRPoint* gdal_verification_point =
        gdal_verification_geometry->toPoint();
    ASSERT_NE(gdal_verification_point, nullptr);
    EXPECT_DOUBLE_EQ(gdal_verification_point->getX(), 123456.75);
    EXPECT_DOUBLE_EQ(gdal_verification_point->getY(), 654321.25);
    gdal_verification_layer->SetSpatialFilterRect(
        123456.74, 654321.24, 123456.76, 654321.26);
    gdal_verification_layer->ResetReading();
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
        gdal_spatial_feature(gdal_verification_layer->GetNextFeature(),
                             OGRFeature::DestroyFeature);
    ASSERT_NE(gdal_spatial_feature, nullptr);
    EXPECT_EQ(gdal_spatial_feature->GetFID(), 6);
    EXPECT_EQ(gdal_verification_layer->GetNextFeature(), nullptr);
    gdal_verification_layer->SetSpatialFilter(nullptr);
    OGRLayer* gdal_created_layer =
        gdal_verification_dataset->GetLayerByName("ExternalRefreshLayer");
    ASSERT_NE(gdal_created_layer, nullptr);
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
        gdal_created_feature(
            gdal_created_layer->GetFeature(1),
            OGRFeature::DestroyFeature);
    ASSERT_NE(gdal_created_feature, nullptr);
    EXPECT_EQ(gdal_created_feature->GetFieldAsInteger("value"), 42);
    gdal_verification_dataset.reset();

    const auto stale_feature = layer.value().read_by_fid(6);
    ASSERT_FALSE(stale_feature);
    // OpenFileGDB may normalize the Shape field WKT during SetFeature; the
    // old snapshot must fail closed as SourceChanged or SchemaMismatch.
    EXPECT_TRUE(stale_feature.error().code == ErrorCode::SourceChanged ||
                stale_feature.error().code == ErrorCode::SchemaMismatch)
        << stale_feature.error().message;

    auto refreshed = dataset.value().refresh();
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    auto refreshed_layer = refreshed.value().open_layer("Point_FIDGap");
    ASSERT_TRUE(refreshed_layer) << refreshed_layer.error().message;
    EXPECT_EQ(refreshed_layer.value().backend_report().selected,
              Backend::FastGdb);
    const auto refreshed_name_field = std::find_if(
        refreshed_layer.value().schema().fields.begin(),
        refreshed_layer.value().schema().fields.end(),
        [](const FieldDefinition& field) { return field.name == "Name"; });
    ASSERT_NE(refreshed_name_field,
              refreshed_layer.value().schema().fields.end());
    const auto refreshed_name_index = static_cast<std::size_t>(
        refreshed_name_field - refreshed_layer.value().schema().fields.begin());
    const auto updated = refreshed_layer.value().read_by_fid(6);
    ASSERT_TRUE(updated) << updated.error().message;
    ASSERT_LT(refreshed_name_index, updated.value().fields.size());
    ASSERT_TRUE(
        updated.value().fields[refreshed_name_index].value.has_value());
    EXPECT_EQ(std::get<std::string>(
                  *updated.value().fields[refreshed_name_index].value),
              "ExternalUpdate_6");
    ASSERT_EQ(updated.value().geometry.state, GeometryState::Value);
    OGRGeometry* raw_geometry = nullptr;
    ASSERT_EQ(OGRGeometryFactory::createFromWkb(
                  updated.value().geometry.wkb.data(), nullptr,
                  &raw_geometry,
                  static_cast<int>(updated.value().geometry.wkb.size())),
              OGRERR_NONE);
    std::unique_ptr<OGRGeometry,
                    decltype(&OGRGeometryFactory::destroyGeometry)>
        geometry(raw_geometry, OGRGeometryFactory::destroyGeometry);
    ASSERT_NE(geometry, nullptr);
    const OGRPoint* refreshed_point = geometry->toPoint();
    ASSERT_NE(refreshed_point, nullptr);
    EXPECT_DOUBLE_EQ(refreshed_point->getX(), 123456.75);
    EXPECT_DOUBLE_EQ(refreshed_point->getY(), 654321.25);

    const auto refreshed_names = refreshed.value().layer_names();
    ASSERT_TRUE(refreshed_names) << refreshed_names.error().message;
    EXPECT_NE(std::find(refreshed_names.value().begin(),
                        refreshed_names.value().end(),
                        "ExternalRefreshLayer"),
              refreshed_names.value().end());
    auto created_layer_after_refresh =
        refreshed.value().open_layer("ExternalRefreshLayer");
    ASSERT_TRUE(created_layer_after_refresh)
        << created_layer_after_refresh.error().message;
    const auto created_layer_metadata =
        created_layer_after_refresh.value().metadata();
    ASSERT_TRUE(created_layer_metadata)
        << created_layer_metadata.error().message;
    EXPECT_EQ(created_layer_metadata.value().definition.source,
              MetadataSource::FastGdb);
    EXPECT_NE(created_layer_metadata.value().definition.status,
              MetadataStatus::Missing);
    EXPECT_NE(created_layer_metadata.value().definition.status,
              MetadataStatus::Unavailable);
    const auto created_feature_after_refresh =
        created_layer_after_refresh.value().read_by_fid(1);
    ASSERT_TRUE(created_feature_after_refresh)
        << created_feature_after_refresh.error().message;
    ASSERT_EQ(created_feature_after_refresh.value().fields.size(), 1U);
    ASSERT_TRUE(
        created_feature_after_refresh.value().fields.front().value.has_value());
    EXPECT_EQ(std::get<std::int32_t>(
                  *created_feature_after_refresh.value().fields.front().value),
              42);

    Query spatial_query;
    spatial_query.order = ResultOrder::FidAscending;
    spatial_query.spatial_filter = Envelope{
        123456.74, 654321.24, 123456.76, 654321.26};
    const auto spatial_plan = refreshed_layer.value().explain(spatial_query);
    ASSERT_TRUE(spatial_plan) << spatial_plan.error().message;
    EXPECT_EQ(spatial_plan.value().backend, Backend::FastGdb);
    EXPECT_EQ(spatial_plan.value().execution_path, "fast/spatial_bbox");
    auto spatial_cursor_result =
        refreshed_layer.value().open_cursor(spatial_query);
    ASSERT_TRUE(spatial_cursor_result)
        << spatial_cursor_result.error().message;
    auto spatial_cursor = std::move(spatial_cursor_result).value();
    EXPECT_EQ(spatial_cursor.query_report().execution_path,
              "bbox:model:sequential-fallback");
    const auto spatial_feature = spatial_cursor.next();
    ASSERT_TRUE(spatial_feature) << spatial_feature.error().message;
    ASSERT_TRUE(spatial_feature.value().has_value());
    EXPECT_EQ(spatial_feature.value()->fid, 6);
    const auto spatial_end = spatial_cursor.next();
    ASSERT_TRUE(spatial_end) << spatial_end.error().message;
    EXPECT_FALSE(spatial_end.value().has_value());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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
    EXPECT_EQ(cursor.value().consistency_report().consistency,
              Consistency::UnverifiedConcurrentRead);
    EXPECT_EQ(update.token.notify_update_closed(true),
              explorgdb::CoordinationStatus::Ok);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

    auto roads = dataset.value().open_layer_by_path("/TransportFD/roads");
    ASSERT_TRUE(roads) << roads.error().message;
    OpenOptions gdal_options;
    gdal_options.backend = BackendPreference::GdalOnly;
    auto gdal_dataset = Dataset::open(source, gdal_options);
    ASSERT_TRUE(gdal_dataset);
    auto gdal_roads =
        gdal_dataset.value().open_layer_by_path("/TransportFD/roads");
    ASSERT_TRUE(gdal_roads);
    ASSERT_EQ(roads.value().schema().fields.size(),
              gdal_roads.value().schema().fields.size());
    EXPECT_EQ(roads.value().schema().geometry_field,
              gdal_roads.value().schema().geometry_field);
    EXPECT_EQ(roads.value().schema().geometry_type,
              gdal_roads.value().schema().geometry_type);
    for (std::size_t i = 0; i < roads.value().schema().fields.size(); ++i) {
        const auto& fast_field = roads.value().schema().fields[i];
        const auto& gdal_field = gdal_roads.value().schema().fields[i];
        EXPECT_EQ(fast_field.name, gdal_field.name) << i;
        EXPECT_EQ(fast_field.alias, gdal_field.alias) << i;
        EXPECT_EQ(fast_field.type, gdal_field.type) << i;
        EXPECT_EQ(fast_field.nullable, gdal_field.nullable) << i;
        EXPECT_EQ(fast_field.default_value, gdal_field.default_value) << i;
        EXPECT_EQ(fast_field.domain_name, gdal_field.domain_name) << i;
    }
    Query geometry_query;
    geometry_query.attribute_filter = "status LIKE '%'";
    auto geometry_cursor = roads.value().open_cursor(geometry_query);
    ASSERT_TRUE(geometry_cursor) << geometry_cursor.error().message;
    EXPECT_EQ(geometry_cursor.value().backend_report().selected,
              Backend::GdalOpenFileGDB);

    auto parcels = dataset.value().open_layer_by_path("/AdminFD/parcels");
    ASSERT_TRUE(parcels);
    const auto area = std::find_if(
        parcels.value().schema().fields.begin(),
        parcels.value().schema().fields.end(),
        [](const FieldDefinition& field) {
            return field.name == "Shape_Area";
        });
    ASSERT_NE(area, parcels.value().schema().fields.end());
    EXPECT_EQ(area->default_value, "FILEGEODATABASE_SHAPE_AREA");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

    options.include_system_tables = true;
    const auto metadata_source = explorgdb_test_paths::test_data_path(
        "test_data/gdb/acceptance_metadata.gdb").string();
    auto system_dataset = Dataset::open(metadata_source, options);
    ASSERT_TRUE(system_dataset);
    auto system_names = system_dataset.value().layer_names();
    ASSERT_TRUE(system_names);
    EXPECT_NE(std::find(system_names.value().begin(),
                        system_names.value().end(),
                        "GDB_SystemCatalog"),
              system_names.value().end());
}

TEST(UnifiedGdalFacadeTest, ReportsAttachmentAndBoundedBinaryGaps) {
    OpenOptions options;
    options.backend = BackendPreference::GdalOnly;
    const auto source = explorgdb_test_paths::test_data_path(
        "test_data/gdb/acceptance_metadata.gdb").string();
    auto dataset = Dataset::open(source, options);
    ASSERT_TRUE(dataset) << dataset.error().message;

    const auto metadata = dataset.value().metadata();
    ASSERT_TRUE(metadata) << metadata.error().message;
    EXPECT_EQ(metadata.value().attachments.source,
              MetadataSource::GdalOpenFileGDB);
    EXPECT_EQ(metadata.value().attachments.status, MetadataStatus::Unsupported);

    auto layer = dataset.value().open_layer("all_field_types");
    ASSERT_TRUE(layer) << layer.error().message;
    const auto binary = layer.value().read_binary_field_by_fid(
        1, "blob_fld", 64);
    EXPECT_FALSE(binary);
    EXPECT_EQ(binary.error().code, ErrorCode::Unsupported);
}

TEST(UnifiedGdalFacadeTest, ProjectsFieldsWithoutBreakingAttributeFilters) {
    OpenOptions options;
    options.backend = BackendPreference::GdalOnly;
    const auto source = explorgdb_test_paths::test_data_path(
        "test_data/gdb/acceptance_metadata.gdb").string();
    auto dataset = Dataset::open(source, options);
    ASSERT_TRUE(dataset) << dataset.error().message;
    auto layer = dataset.value().open_layer_by_path("/TransportFD/roads");
    ASSERT_TRUE(layer) << layer.error().message;

    const auto name_index = std::find_if(
        layer.value().schema().fields.begin(),
        layer.value().schema().fields.end(),
        [](const FieldDefinition& field) { return field.name == "name"; });
    ASSERT_NE(name_index, layer.value().schema().fields.end());
    const auto projected_index = static_cast<std::size_t>(
        name_index - layer.value().schema().fields.begin());

    Query projected_query;
    projected_query.projected_fields = {"name"};
    projected_query.include_geometry = false;
    auto projected_cursor_result = layer.value().open_cursor(projected_query);
    ASSERT_TRUE(projected_cursor_result)
        << projected_cursor_result.error().message;
    auto projected_cursor = std::move(projected_cursor_result).value();
    auto projected_feature = projected_cursor.next();
    ASSERT_TRUE(projected_feature) << projected_feature.error().message;
    ASSERT_TRUE(projected_feature.value());
    EXPECT_EQ(projected_feature.value()->geometry.state,
              GeometryState::NotRequested);
    EXPECT_EQ(projected_feature.value()->fields[projected_index].state,
              ValueState::Value);
    for (std::size_t index = 0;
         index < projected_feature.value()->fields.size(); ++index) {
        if (index != projected_index) {
            EXPECT_EQ(projected_feature.value()->fields[index].state,
                      ValueState::Unset);
        }
    }
    projected_cursor.close();

    Query filtered_query;
    filtered_query.projected_fields = {"name"};
    filtered_query.attribute_filter = "road_id = 1";
    filtered_query.include_geometry = false;
    auto filtered_cursor_result = layer.value().open_cursor(filtered_query);
    ASSERT_TRUE(filtered_cursor_result) << filtered_cursor_result.error().message;
    auto filtered_cursor = std::move(filtered_cursor_result).value();
    auto filtered_feature = filtered_cursor.next();
    ASSERT_TRUE(filtered_feature) << filtered_feature.error().message;
    ASSERT_TRUE(filtered_feature.value());
    EXPECT_EQ(filtered_feature.value()->fields[projected_index].state,
              ValueState::Value);
    EXPECT_EQ(filtered_feature.value()->geometry.state,
              GeometryState::NotRequested);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(UnifiedGdalFacadeTest, BoundsFidAscendingCandidateMemory) {
    OpenOptions options;
    options.backend = BackendPreference::GdalOnly;
    auto dataset = Dataset::open(fixture_path(), options);
    ASSERT_TRUE(dataset);
    auto names = dataset.value().layer_names();
    ASSERT_TRUE(names);
    auto layer = dataset.value().open_layer(names.value().front());
    ASSERT_TRUE(layer);

    Query query;
    query.order = ResultOrder::FidAscending;
    query.max_ordered_fid_bytes = sizeof(Fid);
    auto cursor = layer.value().open_cursor(query);
    EXPECT_FALSE(cursor);
    EXPECT_EQ(cursor.error().code, ErrorCode::ResultLimitExceeded);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

    const auto field_index = [&](const std::string& name) {
        const auto found = std::find_if(
            fast.fields.begin(), fast.fields.end(),
            [&](const FieldDefinition& field) { return field.name == name; });
        return static_cast<std::size_t>(found - fast.fields.begin());
    };
    const std::size_t date_index = field_index("date_fld");
    const std::size_t offset_index = field_index("timestampoffset_fld");
    const std::size_t binary_index = field_index("blob_fld");
    ASSERT_LT(date_index, fast.fields.size());
    ASSERT_LT(offset_index, fast.fields.size());
    ASSERT_LT(binary_index, fast.fields.size());

    const auto fast_feature = fast_layer.value().read_by_fid(4);
    const auto gdal_feature = gdal_layer.value().read_by_fid(4);
    ASSERT_TRUE(fast_feature) << fast_feature.error().message;
    ASSERT_TRUE(gdal_feature) << gdal_feature.error().message;
    ASSERT_TRUE(fast_feature.value().fields[date_index].value.has_value());
    ASSERT_TRUE(gdal_feature.value().fields[date_index].value.has_value());
    ASSERT_TRUE(fast_feature.value().fields[offset_index].value.has_value());
    ASSERT_TRUE(gdal_feature.value().fields[offset_index].value.has_value());
    ASSERT_TRUE(fast_feature.value().fields[binary_index].value.has_value());
    ASSERT_TRUE(gdal_feature.value().fields[binary_index].value.has_value());
    const auto* fast_date = std::get_if<DateTimeValue>(
        &*fast_feature.value().fields[date_index].value);
    const auto* gdal_date = std::get_if<DateTimeValue>(
        &*gdal_feature.value().fields[date_index].value);
    ASSERT_NE(fast_date, nullptr);
    ASSERT_NE(gdal_date, nullptr);
    EXPECT_EQ(fast_date->iso8601, "2025-06-15 12:30:00");
    EXPECT_EQ(gdal_date->iso8601, fast_date->iso8601);

    const auto* fast_offset = std::get_if<DateTimeValue>(
        &*fast_feature.value().fields[offset_index].value);
    const auto* gdal_offset = std::get_if<DateTimeValue>(
        &*gdal_feature.value().fields[offset_index].value);
    ASSERT_NE(fast_offset, nullptr);
    ASSERT_NE(gdal_offset, nullptr);
    EXPECT_EQ(fast_offset->iso8601, "2025-01-01 12:00:00");
    ASSERT_TRUE(fast_offset->utc_offset_minutes.has_value());
    ASSERT_TRUE(gdal_offset->utc_offset_minutes.has_value());
    EXPECT_EQ(*fast_offset->utc_offset_minutes, 0);
    EXPECT_EQ(*gdal_offset->utc_offset_minutes, 0);

    const auto* fast_binary = std::get_if<std::vector<std::uint8_t>>(
        &*fast_feature.value().fields[binary_index].value);
    const auto* gdal_binary = std::get_if<std::vector<std::uint8_t>>(
        &*gdal_feature.value().fields[binary_index].value);
    ASSERT_NE(fast_binary, nullptr);
    ASSERT_NE(gdal_binary, nullptr);
    const std::vector<std::uint8_t> expected_binary = {
        'H', 'e', 'l', 'l', 'o', ' ', 'B', 'i', 'n', 'a', 'r', 'y', ' ',
        'W', 'o', 'r', 'l', 'd', '!', 0x00, 0xff};
    EXPECT_EQ(*fast_binary, expected_binary);
    EXPECT_EQ(*gdal_binary, expected_binary);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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
    auto workspace_metadata = dataset.value().metadata();
    ASSERT_TRUE(workspace_metadata);
    EXPECT_EQ(workspace_metadata.value().domains.source,
              MetadataSource::GdalOpenFileGDB);
    EXPECT_NE(workspace_metadata.value().domains.status,
              MetadataStatus::Unavailable);
    EXPECT_NE(workspace_metadata.value().relationships.status,
              MetadataStatus::Unavailable);
    EXPECT_EQ(workspace_metadata.value().catalog_objects.status,
              CatalogInventoryStatus::Unsupported);
    EXPECT_EQ(dataset.value().catalog_object_by_uuid(
                  "{E32B8E0E-76FF-4792-A874-31FD8B282224}").status,
              CatalogObjectLookupStatus::Unsupported);
    EXPECT_NE(std::find_if(
        workspace_metadata.value().feature_datasets.value.begin(),
        workspace_metadata.value().feature_datasets.value.end(),
        [](const FeatureDatasetMetadata& feature_dataset) {
            return feature_dataset.name == "TransportFD";
        }), workspace_metadata.value().feature_datasets.value.end());
    auto roads = dataset.value().open_layer_by_path("/TransportFD/roads");
    ASSERT_TRUE(roads) << roads.error().message;
    auto layer_metadata = roads.value().metadata();
    ASSERT_TRUE(layer_metadata) << layer_metadata.error().message;
    EXPECT_EQ(layer_metadata.value().subtypes.status,
              MetadataStatus::Unsupported);
    EXPECT_EQ(layer_metadata.value().subtypes.source,
              MetadataSource::GdalOpenFileGDB);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

TEST(UnifiedFastFacadeTest, RefreshRebuildsWarmProofWithSyntheticIndexDeclaration) {
    namespace fs = std::filesystem;
    const auto path = fs::temp_directory_path() / "fast_gdb_unified_source_proof_refresh.gdb";
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
    } cleanup{path};
    const auto declare_index = [&] {
        // GDAL writes HasSpatialIndex=false even when it persists a valid SPX.
        // This temporary fixture deliberately declares the index to exercise
        // the proof/cache path; it is not ArcGIS metadata acceptance evidence.
        const auto items = path / "a00000004.gdbtable";
        std::ifstream input(items, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(input)), {});
        input.close();
        const std::string marker = "HasSpatialIndex>false";
        const auto position = bytes.find(marker);
        if (position == std::string::npos)
            return bytes.find("HasSpatialIndex>true ") != std::string::npos;
        // Equal-width XML text preserves the table/row/string length prefixes.
        bytes.replace(position + marker.size() - 5, 5, "true ");
        std::ofstream output(items, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), bytes.size());
        return output.good();
    };
    fs::remove_all(path);
    GDALAllRegister();
    auto* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    ASSERT_NE(driver, nullptr);
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> native(
        driver->Create(path.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr), GDALClose);
    ASSERT_NE(native, nullptr);
    auto* native_layer = native->CreateLayer("points", nullptr, wkbPoint, nullptr);
    ASSERT_NE(native_layer, nullptr);
    OGRFieldDefn value("value", OFTInteger);
    ASSERT_EQ(native_layer->CreateField(&value), OGRERR_NONE);
    for (int i = 0; i < 500; ++i) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
            OGRFeature::CreateFeature(native_layer->GetLayerDefn()), OGRFeature::DestroyFeature);
        OGRPoint point(i + 1, i + 1);
        feature->SetField("value", i);
        ASSERT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
        ASSERT_EQ(native_layer->CreateFeature(feature.get()), OGRERR_NONE);
    }
    native.reset();
    native.reset(static_cast<GDALDataset*>(GDALOpenEx(path.string().c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr)));
    ASSERT_NE(native, nullptr);
    native_layer = native->GetLayerByName("points"); ASSERT_NE(native_layer, nullptr);
    while (auto* raw = native_layer->GetNextFeature()) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(raw, OGRFeature::DestroyFeature);
        ASSERT_EQ(native_layer->SetFeature(feature.get()), OGRERR_NONE);
    }
    CPLErrorReset();
    native->ExecuteSQL("RECOMPUTE EXTENT ON points", nullptr, nullptr);
    ASSERT_LT(CPLGetLastErrorType(), CE_Failure);
    native->ExecuteSQL("CREATE INDEX source_value_idx ON points(value)", nullptr, nullptr);
    ASSERT_LT(CPLGetLastErrorType(), CE_Failure);
    native.reset();

    ASSERT_TRUE(declare_index());
    {
        explorgdb::GdbCatalog catalog; ASSERT_TRUE(catalog.scan(path.string()));
        explorgdb::CatalogResolver resolver(catalog); ASSERT_TRUE(resolver.load());
        const auto table = resolver.resolve("points"); ASSERT_TRUE(table.has_value());
        explorgdb::QueryEngine probe(catalog, *table); ASSERT_TRUE(probe.open());
        explorgdb::QueryRequest request; request.kind = explorgdb::QueryKind::SpatialBbox;
        request.xmin = 1.9; request.ymin = 1.9; request.xmax = 2.1; request.ymax = 2.1;
        const auto result = probe.query(request);
        ASSERT_GT(result.spatial_metrics.source_directory_bytes, 0U) << result.fallback_reason;
    }
    OpenOptions options; options.backend = BackendPreference::FastOnly;
    auto dataset = Dataset::open(path.string(), options); ASSERT_TRUE(dataset) << dataset.error().message;
    auto layer = dataset.value().open_layer("points"); ASSERT_TRUE(layer) << layer.error().message;
    Query old_window; old_window.spatial_filter = Envelope{1.9, 1.9, 2.1, 2.1};
    for (int round = 0; round < 2; ++round) {
        auto result = layer.value().open_cursor(old_window); ASSERT_TRUE(result) << result.error().message;
        auto cursor = std::move(result).value();
        EXPECT_NE(cursor.query_report().execution_path.find("bbox:model:spx-candidates"), std::string::npos)
            << cursor.query_report().execution_path;
        const auto item = cursor.next(); ASSERT_TRUE(item); ASSERT_TRUE(item.value().has_value());
        EXPECT_EQ(item.value()->fid, 2);
        const auto end = cursor.next(); ASSERT_TRUE(end); EXPECT_FALSE(end.value().has_value());
        cursor.close();
    }
    native.reset(static_cast<GDALDataset*>(GDALOpenEx(path.string().c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
        nullptr, nullptr, nullptr)));
    ASSERT_NE(native, nullptr);
    native_layer = native->GetLayerByName("points"); ASSERT_NE(native_layer, nullptr);
    std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> changed(
        native_layer->GetFeature(2), OGRFeature::DestroyFeature);
    ASSERT_NE(changed, nullptr);
    OGRPoint moved(900, 900); ASSERT_EQ(changed->SetGeometry(&moved), OGRERR_NONE);
    ASSERT_EQ(native_layer->SetFeature(changed.get()), OGRERR_NONE);
    native.reset();
    ASSERT_TRUE(declare_index());
    const auto stale = layer.value().open_cursor(old_window);
    ASSERT_FALSE(stale);
    EXPECT_TRUE(stale.error().code == ErrorCode::SourceChanged || stale.error().code == ErrorCode::SchemaMismatch);
    auto refreshed = dataset.value().refresh(); ASSERT_TRUE(refreshed) << refreshed.error().message;
    // Uncoordinated external writes need not advance the coordinator counter;
    // refresh must replace the source snapshot and expire old handles anyway.
    const auto expired = dataset.value().layer_names();
    ASSERT_FALSE(expired); EXPECT_EQ(expired.error().code, ErrorCode::ReaderExpired);
    auto current = refreshed.value().open_layer("points"); ASSERT_TRUE(current) << current.error().message;
    auto old_result = current.value().open_cursor(old_window); ASSERT_TRUE(old_result) << old_result.error().message;
    auto old_cursor = std::move(old_result).value();
    const auto old_end = old_cursor.next(); ASSERT_TRUE(old_end); EXPECT_FALSE(old_end.value().has_value());
    old_cursor.close();
    Query new_window; new_window.spatial_filter = Envelope{899.9, 899.9, 900.1, 900.1};
    auto result = current.value().open_cursor(new_window); ASSERT_TRUE(result) << result.error().message;
    auto cursor = std::move(result).value();
    EXPECT_NE(cursor.query_report().execution_path.find("bbox:model:spx-candidates"), std::string::npos)
        << cursor.query_report().execution_path;
    const auto item = cursor.next(); ASSERT_TRUE(item); ASSERT_TRUE(item.value().has_value());
    EXPECT_EQ(item.value()->fid, 2);
    const auto end = cursor.next(); ASSERT_TRUE(end); EXPECT_FALSE(end.value().has_value());
}
#endif

}  // namespace
