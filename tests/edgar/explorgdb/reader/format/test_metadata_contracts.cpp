// 文件说明：验证元数据解码和分类合同，不依赖 GDAL 运行时。

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "metadata_reader.h"

using namespace explorgdb;

TEST(MetadataContractTest, DecodesAttachmentMarkerFromRelationshipDefinitionXml) {
    LayerMetadata item;
    item.uuid = "{relationship-uuid}";
    item.name = "roads__ATTACHREL";
    item.path = "\\TransportFD\\roads__ATTACHREL";
    item.dataset_type = "esriDTRelationshipClass";
    item.definition =
        "<DERelationshipClassInfo>"
        "<Cardinality>esriRelCardinalityOneToMany</Cardinality>"
        "<IsAttachmentRelationship>true</IsAttachmentRelationship>"
        "<OriginClassNames><Name>roads</Name></OriginClassNames>"
        "<DestinationClassNames><Name>roads__ATTACH</Name>"
        "</DestinationClassNames>"
        "<OriginClassKeys><RelationshipClassKey>"
        "<ObjectKeyName>GlobalID</ObjectKeyName>"
        "<KeyRole>esriRelKeyRoleOriginPrimary</KeyRole>"
        "</RelationshipClassKey></OriginClassKeys>"
        "<DestinationClassKeys><RelationshipClassKey>"
        "<ObjectKeyName>REL_GLOBALID</ObjectKeyName>"
        "<KeyRole>esriRelKeyRoleDestinationForeign</KeyRole>"
        "</RelationshipClassKey></DestinationClassKeys>"
        "</DERelationshipClassInfo>";

    const RelationshipClassInfo relationship =
        MetadataReader::decode_relationship_class_definition_xml(item);

    EXPECT_TRUE(relationship.is_attachment_relationship);
    EXPECT_EQ(relationship.uuid, item.uuid);
    EXPECT_EQ(relationship.name, item.name);
    EXPECT_EQ(relationship.path, item.path);
    EXPECT_EQ(relationship.cardinality, "esriRelCardinalityOneToMany");
    EXPECT_EQ(relationship.origin_class_names,
              (std::vector<std::string>{"roads"}));
    EXPECT_EQ(relationship.destination_class_names,
              (std::vector<std::string>{"roads__ATTACH"}));
    ASSERT_EQ(relationship.keys.size(), 2U);
    EXPECT_EQ(relationship.keys[0].object_key_name, "GlobalID");
    EXPECT_EQ(relationship.keys[0].key_role,
              "esriRelKeyRoleOriginPrimary");
    EXPECT_EQ(relationship.keys[1].object_key_name, "REL_GLOBALID");
    EXPECT_EQ(relationship.keys[1].key_role,
              "esriRelKeyRoleDestinationForeign");
    EXPECT_EQ(relationship.raw_definition, item.definition);
}

TEST(MetadataContractTest, ClassifiesEveryKnownAdvancedObjectKind) {
    struct Case {
        const char* type_name;
        const char* dataset_type;
        const char* geometry_type;
        AdvancedObjectKind expected;
    };
    const Case cases[] = {
        {"Feature Class", "esriDTFeatureClass", "esriGeometryAnnotation",
         AdvancedObjectKind::Annotation},
        {"Feature Class", "esriDTFeatureClass", "esriGeometryDimension",
         AdvancedObjectKind::Dimension},
        {"Topology", "", "", AdvancedObjectKind::Topology},
        {"Geometric Network", "", "",
         AdvancedObjectKind::GeometricNetwork},
        {"Network Dataset", "", "", AdvancedObjectKind::NetworkDataset},
        {"Network Diagram", "", "", AdvancedObjectKind::NetworkDiagram},
        {"Trace Network", "", "", AdvancedObjectKind::TraceNetwork},
        {"Utility Network", "", "", AdvancedObjectKind::UtilityNetwork},
        {"Parcel Fabric", "", "", AdvancedObjectKind::ParcelFabric},
        {"Terrain", "", "", AdvancedObjectKind::Terrain},
        {"Mosaic Dataset", "", "", AdvancedObjectKind::MosaicDataset},
        {"TIN", "", "", AdvancedObjectKind::Tin},
        {"mOsAiC dAtAsEt", "", "", AdvancedObjectKind::MosaicDataset},
        {"Feature Class", "esriDTFeatureClass", "esriGeometryPolyline",
         AdvancedObjectKind::None},
        {"unknown custom object", "esriDTCustomObject", "",
         AdvancedObjectKind::None},
    };

    for (const auto& test_case : cases) {
        EXPECT_EQ(MetadataReader::classify_advanced_object(
                      test_case.type_name, test_case.dataset_type,
                      test_case.geometry_type),
                  test_case.expected)
            << test_case.type_name << "/" << test_case.dataset_type << "/"
            << test_case.geometry_type;
    }
}
