#include <gtest/gtest.h>
#include "metadata_reader.h"

using namespace explorgdb;

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

TEST(MetadataReaderTest, RejectsDifferentWkid) {
    std::unordered_map<std::string, size_t> columns{{"wkid", 0}};
    FeatureRecord row;
    row.field_values = {int32_t{4326}};
    EXPECT_FALSE(MetadataReader::decode_spatial_reference_row(columns, row, 3857));
}
