// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include "metadata_reader.h"
#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "test_fixture.h"

using namespace explorgdb;

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(MetadataReaderTest, DecodesRequestedSpatialReference) {
    std::unordered_map<std::string, size_t> columns{
        {"wkid", 0}, {"latestwkid", 1}, {"srsname", 2}, {"wkt", 3}
    };
    FeatureRecord row;
    row.field_values = {
        int32_t{4326}, int32_t{4326}, std::string{"WGS 84"},
        std::string{"GEOGCS[\"WGS 84\"]"}
    };

    const auto info = MetadataReader::decode_spatial_reference_row(columns, row, 4326);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->wkid, 4326);
    EXPECT_EQ(info->latest_wkid, 4326);
    EXPECT_EQ(info->name, "WGS 84");
    EXPECT_EQ(info->wkt, "GEOGCS[\"WGS 84\"]");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(MetadataReaderTest, FallsBackToDefinitionWhenWktIsEmpty) {
    std::unordered_map<std::string, size_t> columns{
        {"wkid", 0}, {"definition", 1}
    };
    FeatureRecord row;
    row.field_values = {int32_t{3857}, std::string{"PROJCS[\"Web Mercator\"]"}};

    const auto info = MetadataReader::decode_spatial_reference_row(columns, row, 3857);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->wkt, "PROJCS[\"Web Mercator\"]");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(MetadataReaderTest, RejectsDifferentWkid) {
    std::unordered_map<std::string, size_t> columns{{"wkid", 0}};
    FeatureRecord row;
    row.field_values = {int32_t{4326}};
    EXPECT_FALSE(MetadataReader::decode_spatial_reference_row(columns, row, 3857));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(MetadataReaderTest, DecodesLayerMetadataByNameOrPhysicalName) {
    std::unordered_map<std::string, size_t> columns{
        {"name", 0}, {"physicalname", 1}, {"path", 2}, {"definition", 3}, {"type", 4}
    };
    FeatureRecord row;
    row.field_values = {
        std::string{"roads"},
        std::string{"a00000009"},
        std::string{"\\Transportation\\roads"},
        std::string{"<DEFeatureClassInfo />"},
        int32_t{7}
    };

    const auto by_name = MetadataReader::decode_layer_metadata_row(columns, row, "roads");
    ASSERT_TRUE(by_name.has_value());
    EXPECT_EQ(by_name->name, "roads");
    EXPECT_EQ(by_name->physical_name, "a00000009");
    EXPECT_EQ(by_name->path, "\\Transportation\\roads");
    EXPECT_EQ(by_name->definition, "<DEFeatureClassInfo />");
    EXPECT_EQ(by_name->type, "7");
    EXPECT_EQ(by_name->catalog_path, "");
    EXPECT_EQ(by_name->dataset_type, "");

    const auto by_physical = MetadataReader::decode_layer_metadata_row(columns, row, "A00000009");
    ASSERT_TRUE(by_physical.has_value());
    EXPECT_EQ(by_physical->name, "roads");

    EXPECT_FALSE(MetadataReader::decode_layer_metadata_row(columns, row, "buildings"));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(MetadataReaderTest, ExtractsCatalogPathAndDatasetTypeFromDefinitionXml) {
    std::unordered_map<std::string, size_t> columns{
        {"name", 0}, {"physicalname", 1}, {"definition", 2}
    };
    FeatureRecord row;
    row.field_values = {
        std::string{"roads"},
        std::string{"ROADS"},
        std::string{
            "<typens:DEFeatureClassInfo>"
            "<CatalogPath>\\Transportation\\roads</CatalogPath>"
            "<DatasetType>esriDTFeatureClass</DatasetType>"
            "</typens:DEFeatureClassInfo>"
        }
    };

    const auto metadata = MetadataReader::decode_layer_metadata_row(columns, row, "roads");
    ASSERT_TRUE(metadata.has_value());
    EXPECT_EQ(metadata->catalog_path, "\\Transportation\\roads");
    EXPECT_EQ(metadata->dataset_type, "esriDTFeatureClass");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(MetadataReaderTest, DecodesWorkspaceDomainsXml) {
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<DEWorkspace>"
        "<Domains xsi:type=\"typens:ArrayOfDomain\">"
        "<Domain xsi:type=\"typens:CodedValueDomain\">"
        "<DomainName>status_domain</DomainName>"
        "<FieldType>esriFieldTypeString</FieldType>"
        "<Description>Status values</Description>"
        "<MergePolicy>esriMPTDefaultValue</MergePolicy>"
        "<SplitPolicy>esriSPTDuplicate</SplitPolicy>"
        "<CodedValues>"
        "<CodedValue><Name>Open</Name><Code>OPEN</Code></CodedValue>"
        "<CodedValue><Name>Closed</Name><Code>CLOSED</Code></CodedValue>"
        "</CodedValues>"
        "</Domain>"
        "<Domain xsi:type=\"typens:RangeDomain\">"
        "<DomainName>height_domain</DomainName>"
        "<FieldType>esriFieldTypeInteger</FieldType>"
        "<MinValue>1</MinValue>"
        "<MaxValue>99</MaxValue>"
        "</Domain>"
        "</Domains>"
        "</DEWorkspace>";

    const auto domains = MetadataReader::decode_workspace_domains_xml(xml);
    ASSERT_EQ(domains.size(), 2u);

    EXPECT_EQ(domains[0].name, "status_domain");
    EXPECT_EQ(domains[0].type, "typens:CodedValueDomain");
    EXPECT_EQ(domains[0].field_type, "esriFieldTypeString");
    EXPECT_EQ(domains[0].coded_values.size(), 2u);
    EXPECT_EQ(domains[0].coded_values[0].name, "Open");
    EXPECT_EQ(domains[0].coded_values[0].code, "OPEN");

    EXPECT_EQ(domains[1].name, "height_domain");
    EXPECT_EQ(domains[1].type, "typens:RangeDomain");
    EXPECT_EQ(domains[1].min_value, "1");
    EXPECT_EQ(domains[1].max_value, "99");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(MetadataReaderTest, DecodesFieldDomainBindingsAndResolvesWorkspaceDomains) {
    const std::string xml =
        "<typens:DEFeatureClassInfo>"
        "<GPFieldInfoExs>"
        "<GPFieldInfoEx>"
        "<Name>status</Name>"
        "<DomainName>status_domain</DomainName>"
        "</GPFieldInfoEx>"
        "<GPFieldInfoEx>"
        "<Name>height</Name>"
        "<DomainName>height_domain</DomainName>"
        "</GPFieldInfoEx>"
        "<GPFieldInfoEx>"
        "<Name>name</Name>"
        "</GPFieldInfoEx>"
        "</GPFieldInfoExs>"
        "</typens:DEFeatureClassInfo>";

    std::vector<DomainInfo> domains;
    DomainInfo coded;
    coded.name = "status_domain";
    coded.type = "typens:CodedValueDomain";
    coded.field_type = "esriFieldTypeString";
    domains.push_back(coded);

    DomainInfo range;
    range.name = "height_domain";
    range.type = "typens:RangeDomain";
    range.min_value = "1";
    range.max_value = "99";
    domains.push_back(range);

    const auto bindings = MetadataReader::decode_field_domain_bindings_xml(xml, domains);
    ASSERT_EQ(bindings.size(), 2u);

    EXPECT_EQ(bindings[0].field_name, "status");
    EXPECT_EQ(bindings[0].domain_name, "status_domain");
    ASSERT_TRUE(bindings[0].domain.has_value());
    EXPECT_EQ(bindings[0].domain->type, "typens:CodedValueDomain");

    EXPECT_EQ(bindings[1].field_name, "height");
    EXPECT_EQ(bindings[1].domain_name, "height_domain");
    ASSERT_TRUE(bindings[1].domain.has_value());
    EXPECT_EQ(bindings[1].domain->max_value, "99");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(MetadataReaderTest, SummarizesDatasetGroupsFromCatalogPaths) {
    std::vector<LayerMetadata> layers;

    LayerMetadata roads;
    roads.name = "roads";
    roads.catalog_path = "\\transport\\roads";
    roads.dataset_type = "esriDTFeatureClass";
    layers.push_back(roads);

    LayerMetadata buildings;
    buildings.name = "buildings";
    buildings.catalog_path = "\\transport\\buildings";
    buildings.dataset_type = "esriDTFeatureClass";
    layers.push_back(buildings);

    LayerMetadata root_table;
    root_table.name = "standalone";
    root_table.catalog_path = "\\standalone";
    root_table.dataset_type = "esriDTTable";
    layers.push_back(root_table);

    const auto groups = MetadataReader::summarize_dataset_groups(layers);
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].group_path, "\\transport");
    ASSERT_EQ(groups[0].member_names.size(), 2u);
    auto names = groups[0].member_names;
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "buildings");
    EXPECT_EQ(names[1], "roads");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(MetadataReaderTest, DecodesRelationshipClassAttributesXml) {
    const std::string xml =
        "<DERelationshipClassInfo>"
        "<Cardinality>esriRelCardinalityOneToMany</Cardinality>"
        "<Notification>esriRelNotificationBoth</Notification>"
        "<OriginPrimaryKey>OBJECTID</OriginPrimaryKey>"
        "<OriginForeignKey>PARENT_ID</OriginForeignKey>"
        "<DestinationPrimaryKey>GLOBALID</DestinationPrimaryKey>"
        "<DestinationForeignKey>CHILD_GUID</DestinationForeignKey>"
        "</DERelationshipClassInfo>";

    const auto attrs = MetadataReader::decode_relationship_class_attributes_xml(xml);
    EXPECT_EQ(attrs.cardinality, "esriRelCardinalityOneToMany");
    EXPECT_EQ(attrs.notification, "esriRelNotificationBoth");
    EXPECT_EQ(attrs.origin_primary_key, "OBJECTID");
    EXPECT_EQ(attrs.origin_foreign_key, "PARENT_ID");
    EXPECT_EQ(attrs.destination_primary_key, "GLOBALID");
    EXPECT_EQ(attrs.destination_foreign_key, "CHILD_GUID");
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(MetadataReaderTest, DecodesSubtypeDefinitionsWithoutLosingCodes) {
    const auto subtypes = MetadataReader::decode_subtypes_xml(
        "<Subtypes><Subtype><SubtypeCode>1</SubtypeCode>"
        "<SubtypeName>Highway</SubtypeName><DefaultValue>60</DefaultValue>"
        "</Subtype><Subtype><SubtypeCode>2</SubtypeCode>"
        "<Name>Local</Name></Subtype></Subtypes>");
    ASSERT_EQ(subtypes.size(), 2u);
    EXPECT_EQ(subtypes[0].code, 1);
    EXPECT_EQ(subtypes[0].name, "Highway");
    EXPECT_EQ(subtypes[0].default_value, "60");
    EXPECT_EQ(subtypes[1].code, 2);
    EXPECT_EQ(subtypes[1].name, "Local");
}

class MetadataReaderIntegrationTest : public GdbTutorialFixture {};

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST_F(MetadataReaderIntegrationTest, ReadsDefinitionFromGdbItemsForGeneratedLayer) {
    const auto path = (std::filesystem::temp_directory_path() /
                       "fast_gdb_metadata_reader_items.gdb").string();

    GDALDataset* dataset = createGdb(path.c_str());
    ASSERT_NE(dataset, nullptr);

    OGRSpatialReference srs;
    ASSERT_EQ(srs.importFromEPSG(4326), OGRERR_NONE);
    OGRLayer* layer = dataset->CreateLayer("metadata_points", &srs, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn name_field("name", OFTString);
    ASSERT_EQ(layer->CreateField(&name_field), OGRERR_NONE);
    GDALClose(dataset);

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));

    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    MetadataReader reader(resolver);
    const auto metadata = reader.read_layer_metadata("metadata_points");
    ASSERT_TRUE(metadata.has_value());
    EXPECT_EQ(metadata->name, "metadata_points");
    EXPECT_FALSE(metadata->definition.empty());
    EXPECT_EQ(metadata->catalog_path, "\\metadata_points");
    EXPECT_EQ(metadata->dataset_type, "esriDTFeatureClass");

    const auto definition = reader.read_metadata_item("metadata_points", "Definition");
    ASSERT_TRUE(definition.has_value());
    EXPECT_EQ(*definition, metadata->definition);

    const auto physical_name = reader.read_metadata_item("metadata_points", "physicalname");
    ASSERT_TRUE(physical_name.has_value());
    EXPECT_FALSE(physical_name->empty());

    const auto domains = reader.read_workspace_domains();
    EXPECT_TRUE(domains.empty());

    const auto field_domains = reader.read_field_domain_bindings("metadata_points");
    EXPECT_TRUE(field_domains.empty());

    const auto relationships = reader.read_relationship_summaries();
    ASSERT_EQ(relationships.size(), 1u);
    EXPECT_EQ(relationships[0].type_name, "DatasetInFolder");
    EXPECT_EQ(relationships[0].origin_path, "\\");
    EXPECT_EQ(relationships[0].destination_name, "metadata_points");
    EXPECT_EQ(relationships[0].destination_path, "\\metadata_points");

    const auto definitions = reader.read_relationship_class_definitions();
    ASSERT_EQ(definitions.size(), 1u);
    EXPECT_EQ(definitions[0].relationship_type_name, "DatasetInFolder");
    EXPECT_EQ(definitions[0].origin_item_type_name, "Folder");
    EXPECT_EQ(definitions[0].destination_item_type_name, "Dataset");
    EXPECT_EQ(definitions[0].origin_item_path, "\\");
    EXPECT_EQ(definitions[0].destination_item_name, "metadata_points");
    EXPECT_EQ(definitions[0].destination_item_path, "\\metadata_points");
    EXPECT_EQ(definitions[0].properties, 1);
    EXPECT_TRUE(definitions[0].is_containment);
    EXPECT_TRUE(definitions[0].cardinality.empty());

    const auto groups = reader.read_dataset_group_summaries();
    EXPECT_TRUE(groups.empty());
}
