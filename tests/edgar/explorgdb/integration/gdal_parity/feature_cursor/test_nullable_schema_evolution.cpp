// GDAL/OpenFileGDB field growth rewrites existing rows with the new bitmap.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "gdb_table.h"
#include "spatial_where_test_utils.h"
#include "test_fixture.h"

using namespace explorgdb;

namespace {

constexpr const char* kLayerName = "nullable_schema_evolution";
constexpr int kInitialNullableFields = 7;
constexpr int kExpandedNullableFields = 33;
constexpr int kFeatureCount = 3;

int32_t stored_value(int row, int field) {
    const uint32_t byte1 = static_cast<uint32_t>(row + 1);
    const uint32_t byte2 = static_cast<uint32_t>(field + 1);
    const uint32_t byte3 = static_cast<uint32_t>(row + field + 1);
    return static_cast<int32_t>(0x11000000U | (byte1 << 16U) |
                                (byte2 << 8U) | byte3);
}

int field_index(const GdbTableParser& table, const std::string& name) {
    const auto& fields = table.fields();
    for (size_t index = 0; index < fields.size(); ++index) {
        if (fields[index].name == name)
            return static_cast<int>(index);
    }
    return -1;
}

} // namespace

class NullableSchemaEvolutionTest : public GdbTutorialFixture {
protected:
    std::string create_fixture() {
        const std::string path = spatial_where_test_utils::fixture_path(
            "fast_gdb_nullable_schema_evolution").string();
        GDALDataset* dataset = createGdb(path.c_str());
        if (dataset == nullptr) return {};

        OGRLayer* layer = dataset->CreateLayer(
            kLayerName, nullptr, wkbNone, nullptr);
        if (layer == nullptr) {
            GDALClose(dataset);
            return {};
        }

        for (int field_index = 0;
             field_index < kInitialNullableFields;
             ++field_index) {
            const std::string name =
                "value_" + std::to_string(field_index);
            OGRFieldDefn field(name.c_str(), OFTInteger);
            field.SetNullable(true);
            if (layer->CreateField(&field) != OGRERR_NONE) {
                GDALClose(dataset);
                return {};
            }
        }

        for (int row = 0; row < kFeatureCount; ++row) {
            OGRFeature* feature = OGRFeature::CreateFeature(
                layer->GetLayerDefn());
            if (feature == nullptr) {
                GDALClose(dataset);
                return {};
            }
            for (int field = 0; field < kInitialNullableFields; ++field) {
                feature->SetField(field, stored_value(row, field));
            }
            const OGRErr error = layer->CreateFeature(feature);
            OGRFeature::DestroyFeature(feature);
            if (error != OGRERR_NONE) {
                GDALClose(dataset);
                return {};
            }
        }
        GDALClose(dataset);

        const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
        dataset = static_cast<GDALDataset*>(GDALOpenEx(
            path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
            allowed_drivers, nullptr, nullptr));
        if (dataset == nullptr) return {};
        layer = dataset->GetLayerByName(kLayerName);
        if (layer == nullptr) {
            GDALClose(dataset);
            return {};
        }
        for (int field_index = kInitialNullableFields;
             field_index < kExpandedNullableFields;
             ++field_index) {
            const std::string name =
                "value_" + std::to_string(field_index);
            OGRFieldDefn field(name.c_str(), OFTInteger);
            field.SetNullable(true);
            if (layer->CreateField(&field) != OGRERR_NONE) {
                GDALClose(dataset);
                return {};
            }
        }
        dataset->FlushCache();
        GDALClose(dataset);
        return path;
    }
};

TEST_F(NullableSchemaEvolutionTest,
       GdalFieldGrowthKeepsFastGdbRowsAligned) {
    const std::string path = create_fixture();
    ASSERT_FALSE(path.empty());

    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> gdal_dataset(
        static_cast<GDALDataset*>(GDALOpenEx(
            path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
            allowed_drivers, nullptr, nullptr)),
        &GDALClose);
    ASSERT_NE(gdal_dataset.get(), nullptr);
    OGRLayer* gdal_layer = gdal_dataset->GetLayerByName(kLayerName);
    ASSERT_NE(gdal_layer, nullptr);
    EXPECT_EQ(gdal_layer->GetLayerDefn()->GetFieldCount(),
              kExpandedNullableFields);
    EXPECT_EQ(gdal_layer->GetFeatureCount(), kFeatureCount);
    for (int row = 0; row < kFeatureCount; ++row) {
        OGRFeature* feature = gdal_layer->GetFeature(row + 1);
        ASSERT_NE(feature, nullptr);
        for (int field = 0; field < kInitialNullableFields; ++field) {
            EXPECT_EQ(feature->GetFieldAsInteger(field),
                      stored_value(row, field));
        }
        EXPECT_FALSE(feature->IsFieldSetAndNotNull(
            kExpandedNullableFields - 1));
        OGRFeature::DestroyFeature(feature);
    }
    gdal_dataset.reset();

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve(kLayerName);
    ASSERT_TRUE(resolved.has_value());

    GdbTableParser table(resolved->table_path);
    ASSERT_TRUE(table.open());
    ASSERT_TRUE(table.load_tablx(resolved->tablx_path));
    const int first_field = field_index(table, "value_0");
    const int last_added_field =
        field_index(table, "value_" + std::to_string(
            kExpandedNullableFields - 1));
    ASSERT_GE(first_field, 0);
    ASSERT_GE(last_added_field, 0);

    for (RowSlot row = 0; row < kFeatureCount; ++row) {
        FeatureRecord record;
        ASSERT_TRUE(table.read_record_by_fid(row, record));
        EXPECT_EQ(record.nullable_flags.size(), 5U);
        for (int bit = 0; bit < kExpandedNullableFields; ++bit) {
            const bool is_null =
                ((record.nullable_flags[static_cast<size_t>(bit / 8)] >>
                  static_cast<unsigned>(bit % 8)) & 1U) != 0;
            EXPECT_EQ(is_null, bit >= kInitialNullableFields)
                << "row=" << row << ", nullable bit=" << bit;
        }
        for (int field = 0; field < kInitialNullableFields; ++field) {
            const int index = field_index(
                table, "value_" + std::to_string(field));
            ASSERT_GE(index, 0);
            const auto* value = std::get_if<int32_t>(
                &record.field_values[static_cast<size_t>(index)]);
            ASSERT_NE(value, nullptr);
            EXPECT_EQ(*value, stored_value(static_cast<int>(row), field));
        }
        EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
            record.field_values[static_cast<size_t>(last_added_field)]));

        FeatureRecord one_pass_record;
        GeometryValue geometry;
        ASSERT_TRUE(table.read_feature_by_fid(
            row, one_pass_record, geometry, nullptr, nullptr,
            GeometryReadOptions::preserve_source(), false));
        ASSERT_EQ(one_pass_record.field_values.size(),
                  record.field_values.size());
        for (size_t index = 0; index < record.field_values.size(); ++index) {
            const auto* expected = std::get_if<int32_t>(
                &record.field_values[index]);
            if (expected != nullptr) {
                const auto* actual = std::get_if<int32_t>(
                    &one_pass_record.field_values[index]);
                ASSERT_NE(actual, nullptr);
                EXPECT_EQ(*actual, *expected);
            } else {
                EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
                    one_pass_record.field_values[index]));
            }
        }
    }

    const auto validate_refs = [&](uint32_t fid,
                                   const FieldRef* refs,
                                   int field_count) {
        if (fid >= kFeatureCount ||
            field_count != kExpandedNullableFields + 1) {
            return false;
        }
        int32_t first_value = 0;
        if (refs[first_field].is_null || refs[first_field].data == nullptr ||
            refs[first_field].byte_len != sizeof(first_value) ||
            !refs[last_added_field].is_null ||
            refs[last_added_field].data != nullptr) {
            return false;
        }
        std::memcpy(&first_value, refs[first_field].data, sizeof(first_value));
        return first_value == stored_value(static_cast<int>(fid), 0);
    };
    EXPECT_EQ(table.sequential_scan(validate_refs),
              static_cast<uint64_t>(kFeatureCount));
    EXPECT_EQ(table.scan_field_candidates(
                  table.active_feature_fids(), validate_refs),
              static_cast<uint64_t>(kFeatureCount));
}
