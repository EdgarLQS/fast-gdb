// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <limits>
#include <sstream>
#include <string>
#include <cctype>
#include <unordered_map>
#include <variant>
#include <vector>

#include "cpl_conv.h"
#include "cpl_time.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "explorgdb_constants.h"
#include "gdb_geometry.h"
#include "gdb_catalog.h"
#include "gdb_indexes.h"
#include "metadata_reader.h"
#include "ole_date.h"
#include "query_engine.h"
#include "reader.h"
#include "fixtures/test_fixture_explorgdb.h"

using namespace explorgdb;

namespace {

const char* required_dataset_from_env(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return nullptr;
    return value;
}

class ScopedCplConfigOption {
public:
    ScopedCplConfigOption(const char* name, const char* value)
        : name_(name) {
        const char* current = CPLGetConfigOption(name, nullptr);
        if (current != nullptr) {
            had_previous_value_ = true;
            previous_value_ = current;
        }
        CPLSetConfigOption(name_.c_str(), value);
    }

    ~ScopedCplConfigOption() {
        CPLSetConfigOption(name_.c_str(),
                           had_previous_value_ ? previous_value_.c_str()
                                               : nullptr);
    }

    ScopedCplConfigOption(const ScopedCplConfigOption&) = delete;
    ScopedCplConfigOption& operator=(const ScopedCplConfigOption&) = delete;

private:
    std::string name_;
    std::string previous_value_;
    bool had_previous_value_ = false;
};

std::vector<size_t> geometry_field_indexes(const GdbTableParser& table) {
    std::vector<size_t> indexes;
    const auto& fields = table.fields();
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].type == FieldType::Geometry) indexes.push_back(i);
    }
    return indexes;
}

[[maybe_unused]] bool record_has_explicit_curve_unsupported(
    const FeatureRecord& record,
    const std::vector<size_t>& geometry_indexes) {
    for (const size_t index : geometry_indexes) {
        if (index >= record.field_values.size()) continue;
        const auto* text = std::get_if<std::string>(&record.field_values[index]);
        if (text != nullptr &&
            text->find("UNSUPPORTED_CURVE_GEOMETRY") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool geometry_is_curve_like(const OGRGeometry* geometry) {
    if (geometry == nullptr) return false;
    if (geometry->hasCurveGeometry(TRUE)) return true;

    const char* geometry_name = geometry->getGeometryName();
    if (geometry_name != nullptr) {
        const std::string name(geometry_name);
        if (name.find("MULTICURVE") != std::string::npos ||
            name.find("COMPOUNDCURVE") != std::string::npos ||
            name.find("CIRCULARSTRING") != std::string::npos) {
            return true;
        }
    }

    char* wkt = nullptr;
    if (geometry->exportToWkt(&wkt) == OGRERR_NONE && wkt != nullptr) {
        const std::string text(wkt);
        CPLFree(wkt);
        return text.find("MULTICURVE") != std::string::npos ||
               text.find("COMPOUNDCURVE") != std::string::npos ||
               text.find("CIRCULARSTRING") != std::string::npos;
    }
    CPLFree(wkt);
    return false;
}

std::vector<std::string> csv_prefix(const std::string& line, size_t count) {
    std::vector<std::string> values;
    std::string value;
    bool quoted = false;
    for (const char ch : line) {
        if (ch == '"') {
            quoted = !quoted;
        } else if (ch == ',' && !quoted && values.size() + 1 < count) {
            values.push_back(value);
            value.clear();
        } else {
            value.push_back(ch);
        }
    }
    values.push_back(value);
    if (values.size() == count) {
        const auto extra = values.back().find(',');
        if (extra != std::string::npos) values.back().resize(extra);
    }
    return values.size() == count ? values : std::vector<std::string>();
}

bool expected_binary_summary_matches(
        const std::string& summary,
        const std::vector<std::uint8_t>& actual) {
    if (summary.rfind("len=", 0) != 0) return false;
    const auto separator = summary.find(";hex=");
    if (separator == std::string::npos) return false;
    size_t expected_size = 0;
    try {
        expected_size = static_cast<size_t>(
            std::stoull(summary.substr(4, separator - 4)));
    } catch (...) {
        return false;
    }
    if (actual.size() != expected_size) return false;
    std::string hex = summary.substr(separator + 5);
    const bool truncated = hex.size() >= 3 &&
        hex.compare(hex.size() - 3, 3, "...") == 0;
    if (truncated) hex.resize(hex.size() - 3);
    if (hex.size() % 2 != 0 ||
        (!truncated && hex.size() != actual.size() * 2)) return false;
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned int value = 0;
        std::istringstream stream(hex.substr(i, 2));
        stream >> std::hex >> value;
        if (!stream || value > 0xffU || actual[i / 2] != value) return false;
    }
    return true;
}

bool expected_value_matches(const std::string& expected,
                            const std::string& binary_summary,
                            const FieldDescriptor& field,
                            const FieldValue& actual) {
    if (const auto* value = std::get_if<std::string>(&actual)) {
        if (field.type == FieldType::UUID_1 || field.type == FieldType::UUID_2) {
            std::string normalized = expected;
            if (!normalized.empty() && normalized.front() == '{') normalized.erase(0, 1);
            if (!normalized.empty() && normalized.back() == '}') normalized.pop_back();
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return *value == normalized || *value == expected;
        }
        return *value == expected;
    }
    if (const auto* value = std::get_if<int32_t>(&actual)) {
        return std::stoll(expected) == *value;
    }
    if (const auto* value = std::get_if<int64_t>(&actual)) {
        return std::stoll(expected) == *value;
    }
    if (const auto* value = std::get_if<double>(&actual)) {
        if (field.type == FieldType::DateTime) {
            return ole_to_string(*value, false, false) == expected;
        }
        if (field.type == FieldType::Date) return ole_to_string(*value, true, false) == expected;
        if (field.type == FieldType::Time) return ole_to_string(*value, false, true) == expected;
        return std::abs(*value - std::stod(expected)) <=
               std::max(1e-9, std::abs(std::stod(expected)) * 1e-12);
    }
    if (const auto* value = std::get_if<DateTimeOffsetValue>(&actual)) {
        const int magnitude = std::abs(value->offset_minutes);
        std::ostringstream suffix;
        suffix << (value->offset_minutes >= 0 ? "+" : "-")
               << std::setw(2) << std::setfill('0') << magnitude / 60
               << ":" << std::setw(2) << magnitude % 60;
        return ole_to_string(value->date, false, false) + suffix.str() == expected;
    }
    if (std::holds_alternative<std::vector<uint8_t>>(actual)) {
        const auto& bytes = std::get<std::vector<uint8_t>>(actual);
        return expected_binary_summary_matches(binary_summary, bytes);
    }
    return false;
}

[[maybe_unused]] void expect_no_silent_geometry_decode(
    const FeatureRecord& record,
    const std::vector<size_t>& geometry_indexes,
    const std::string& layer_name,
    uint32_t fid) {
    for (const size_t index : geometry_indexes) {
        if (index >= record.field_values.size()) continue;
        if (std::holds_alternative<std::nullptr_t>(record.field_values[index])) continue;

        const auto* text = std::get_if<std::string>(&record.field_values[index]);
        ASSERT_NE(text, nullptr) << "layer=" << layer_name << ", fid=" << fid;
        EXPECT_EQ(text->find("<geom decode error>"), std::string::npos)
            << "layer=" << layer_name << ", fid=" << fid;
        EXPECT_EQ(text->find("UNKNOWN("), std::string::npos)
            << "layer=" << layer_name << ", fid=" << fid;

        // Curves are deliberately outside the release scope. They must be explicit,
        // never emitted as an ordinary line or polygon after a failed curve decode.
        if (text->find("UNSUPPORTED_CURVE_GEOMETRY") != std::string::npos) {
            EXPECT_EQ(text->find("MULTILINESTRING"), std::string::npos)
                << "layer=" << layer_name << ", fid=" << fid;
            EXPECT_EQ(text->find("MULTIPOLYGON"), std::string::npos)
                << "layer=" << layer_name << ", fid=" << fid;
        }
    }
}

bool ogr_type_for_field(FieldType type, OGRFieldType& ogr_type) {
    switch (type) {
        case FieldType::Int16:
        case FieldType::Int32:
            ogr_type = OFTInteger;
            return true;
        case FieldType::Int64:
            ogr_type = OFTInteger64;
            return true;
        case FieldType::Float32:
        case FieldType::Float64:
            ogr_type = OFTReal;
            return true;
        case FieldType::String:
        case FieldType::UUID_1:
        case FieldType::UUID_2:
        case FieldType::XML:
            ogr_type = OFTString;
            return true;
        case FieldType::Binary:
            ogr_type = OFTBinary;
            return true;
        case FieldType::Date:
            ogr_type = OFTDate;
            return true;
        case FieldType::Time:
            ogr_type = OFTTime;
            return true;
        case FieldType::DateTime:
        case FieldType::DateTimeWithOffset:
            ogr_type = OFTDateTime;
            return true;
        default:
            return false;
    }
}

std::string normalize_uuid_for_comparison(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '{'), value.end());
    value.erase(std::remove(value.begin(), value.end(), '}'), value.end());
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

bool temporal_value_matches_gdal(FieldType type,
                                 double ole_value,
                                 const OGRFeature& feature,
                                 int ogr_field_index,
                                 int16_t expected_offset_minutes = 0) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    float second = 0.0F;
    int timezone_flag = 0;
    if (!feature.GetFieldAsDateTime(ogr_field_index, &year, &month, &day,
                                    &hour, &minute, &second,
                                    &timezone_flag)) {
        return false;
    }

    const std::string expected = ole_to_string(
        ole_value, type == FieldType::Date, type == FieldType::Time);
    int expected_year = 0;
    int expected_month = 0;
    int expected_day = 0;
    int expected_hour = 0;
    int expected_minute = 0;
    float expected_second = 0.0F;
    if (type == FieldType::Date) {
        if (std::sscanf(expected.c_str(), "%d-%d-%d",
                        &expected_year, &expected_month,
                        &expected_day) != 3) {
            return false;
        }
        return year == expected_year && month == expected_month &&
               day == expected_day;
    }
    if (type == FieldType::Time) {
        if (std::sscanf(expected.c_str(), "%d:%d:%f",
                        &expected_hour, &expected_minute,
                        &expected_second) != 3) {
            return false;
        }
        return hour == expected_hour && minute == expected_minute &&
               std::abs(second - expected_second) <= 0.001F;
    }
    if (std::sscanf(expected.c_str(), "%d-%d-%d %d:%d:%f",
                    &expected_year, &expected_month, &expected_day,
                    &expected_hour, &expected_minute,
                    &expected_second) != 6 ||
        year != expected_year || month != expected_month ||
        day != expected_day || hour != expected_hour ||
        minute != expected_minute ||
        std::abs(second - expected_second) > 0.001F) {
        return false;
    }

    if (type == FieldType::DateTimeWithOffset) {
        const int expected_timezone_flag =
            100 + static_cast<int>(expected_offset_minutes) / 15;
        return timezone_flag == expected_timezone_flag;
    }
    return true;
}

std::string field_value_mismatch(const FieldDescriptor& field,
                                 const FieldValue& value,
                                 const OGRFeature& feature,
                                 int ogr_field_index) {
    if (field.type == FieldType::ObjectId) {
        const GIntBig gdal_fid = feature.GetFID();
        if (const auto* int_value = std::get_if<int32_t>(&value)) {
            return *int_value == gdal_fid ? std::string() : "ObjectID differs";
        }
        if (const auto* int_value = std::get_if<int64_t>(&value)) {
            return *int_value == gdal_fid ? std::string() : "ObjectID differs";
        }
        return "ObjectID value has an unexpected type";
    }

    if (field.type == FieldType::Geometry || field.type == FieldType::Raster)
        return {};
    if (ogr_field_index < 0) return "GDAL field is missing";

    const bool fast_is_null = std::holds_alternative<std::nullptr_t>(value);
    const bool gdal_is_null = !feature.IsFieldSetAndNotNull(ogr_field_index);
    if (fast_is_null || gdal_is_null) {
        return fast_is_null == gdal_is_null ? std::string() : "NULL differs";
    }

    if (const auto* int_value = std::get_if<int32_t>(&value)) {
        return feature.GetFieldAsInteger(ogr_field_index) == *int_value
            ? std::string() : "integer value differs";
    }
    if (const auto* int_value = std::get_if<int64_t>(&value)) {
        return feature.GetFieldAsInteger64(ogr_field_index) == *int_value
            ? std::string() : "Int64 value differs";
    }
    if (const auto* double_value = std::get_if<double>(&value)) {
        if (field.type == FieldType::DateTime || field.type == FieldType::Date ||
            field.type == FieldType::Time) {
            return temporal_value_matches_gdal(
                       field.type, *double_value, feature, ogr_field_index)
                ? std::string() : "temporal value differs";
        }
        const double gdal_value = feature.GetFieldAsDouble(ogr_field_index);
        const double tolerance = field.type == FieldType::Float32
            ? std::max(1e-7, std::abs(*double_value) * 1e-7)
            : std::max(1e-12, std::abs(*double_value) * 1e-12);
        return std::abs(gdal_value - *double_value) <= tolerance
            ? std::string() : "real value differs";
    }
    if (const auto* offset_value = std::get_if<DateTimeOffsetValue>(&value)) {
        return temporal_value_matches_gdal(
                   field.type, offset_value->date, feature, ogr_field_index,
                   offset_value->offset_minutes)
            ? std::string() : "DateTimeOffset value differs";
    }
    if (const auto* string_value = std::get_if<std::string>(&value)) {
        const std::string gdal_value = feature.GetFieldAsString(ogr_field_index);
        if (field.type == FieldType::UUID_1 || field.type == FieldType::UUID_2) {
            return normalize_uuid_for_comparison(gdal_value) ==
                           normalize_uuid_for_comparison(*string_value)
                ? std::string() : "UUID value differs";
        }
        return gdal_value == *string_value ? std::string() : "string value differs";
    }
    if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&value)) {
        int gdal_size = 0;
        const GByte* gdal_bytes = feature.GetFieldAsBinary(
            ogr_field_index, &gdal_size);
        if (gdal_size < 0 || static_cast<size_t>(gdal_size) != bytes->size())
            return "binary length differs";
        if (bytes->empty()) return {};
        if (gdal_size != 0 && gdal_bytes == nullptr)
            return "GDAL returned no binary data";
        return std::equal(bytes->begin(), bytes->end(), gdal_bytes)
            ? std::string() : "binary content differs";
    }
    return "value has an unexpected variant type";
}

bool ordinate_matches(double left, double right) {
    if (std::isnan(left) || std::isnan(right))
        return std::isnan(left) && std::isnan(right);
    const double tolerance = std::max(
        1e-9, std::max(std::abs(left), std::abs(right)) * 1e-12);
    return std::abs(left - right) <= tolerance;
}

bool geometry_coordinates_match(const OGRGeometry& left,
                                const OGRGeometry& right) {
    if (wkbFlatten(left.getGeometryType()) !=
            wkbFlatten(right.getGeometryType()) ||
        left.Is3D() != right.Is3D() ||
        left.IsMeasured() != right.IsMeasured()) {
        return false;
    }
    if (left.IsEmpty() || right.IsEmpty())
        return left.IsEmpty() && right.IsEmpty();

    switch (wkbFlatten(left.getGeometryType())) {
        case wkbPoint: {
            const OGRPoint* left_point = left.toPoint();
            const OGRPoint* right_point = right.toPoint();
            return left_point != nullptr && right_point != nullptr &&
                   ordinate_matches(left_point->getX(), right_point->getX()) &&
                   ordinate_matches(left_point->getY(), right_point->getY()) &&
                   (!left.Is3D() || ordinate_matches(
                        left_point->getZ(), right_point->getZ())) &&
                   (!left.IsMeasured() || ordinate_matches(
                        left_point->getM(), right_point->getM()));
        }
        case wkbLineString:
        case wkbLinearRing: {
            const OGRLineString* left_line = left.toLineString();
            const OGRLineString* right_line = right.toLineString();
            if (left_line == nullptr || right_line == nullptr ||
                left_line->getNumPoints() != right_line->getNumPoints()) {
                return false;
            }
            for (int index = 0; index < left_line->getNumPoints(); ++index) {
                if (!ordinate_matches(left_line->getX(index),
                                     right_line->getX(index)) ||
                    !ordinate_matches(left_line->getY(index),
                                     right_line->getY(index)) ||
                    (left.Is3D() && !ordinate_matches(
                         left_line->getZ(index), right_line->getZ(index))) ||
                    (left.IsMeasured() && !ordinate_matches(
                         left_line->getM(index), right_line->getM(index)))) {
                    return false;
                }
            }
            return true;
        }
        case wkbPolygon: {
            const OGRPolygon* left_polygon = left.toPolygon();
            const OGRPolygon* right_polygon = right.toPolygon();
            if (left_polygon == nullptr || right_polygon == nullptr ||
                left_polygon->getNumInteriorRings() !=
                    right_polygon->getNumInteriorRings()) {
                return false;
            }
            const OGRLinearRing* left_exterior =
                left_polygon->getExteriorRing();
            const OGRLinearRing* right_exterior =
                right_polygon->getExteriorRing();
            if ((left_exterior == nullptr) != (right_exterior == nullptr) ||
                (left_exterior != nullptr &&
                 !geometry_coordinates_match(*left_exterior, *right_exterior))) {
                return false;
            }
            for (int index = 0; index < left_polygon->getNumInteriorRings();
                 ++index) {
                if (!geometry_coordinates_match(
                        *left_polygon->getInteriorRing(index),
                        *right_polygon->getInteriorRing(index))) {
                    return false;
                }
            }
            return true;
        }
        case wkbMultiPoint:
        case wkbMultiLineString:
        case wkbMultiPolygon:
        case wkbGeometryCollection: {
            const OGRGeometryCollection* left_collection =
                left.toGeometryCollection();
            const OGRGeometryCollection* right_collection =
                right.toGeometryCollection();
            if (left_collection == nullptr || right_collection == nullptr ||
                left_collection->getNumGeometries() !=
                    right_collection->getNumGeometries()) {
                return false;
            }
            for (int index = 0;
                 index < left_collection->getNumGeometries(); ++index) {
                const OGRGeometry* left_part =
                    left_collection->getGeometryRef(index);
                const OGRGeometry* right_part =
                    right_collection->getGeometryRef(index);
                if (left_part == nullptr || right_part == nullptr ||
                    !geometry_coordinates_match(*left_part, *right_part)) {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
    }
}

} // namespace

TEST(RealDataReleaseContractTest,
     SyntheticMultiPatchIsoWkbIsAcceptedByGdal) {
    GdbGeomDecoder decoder(
        0.0, 0.0, 1000.0, 0.0, 1000.0, 0.0, 1000.0,
        false, false);

    const auto strip_blob = explorgdb_test::build_geom_multipatch();
    const GeometryValue strip = decoder.decode_value(
        strip_blob.data(), strip_blob.size());
    ASSERT_EQ(strip.status, GeometryStatus::Valid) << strip.diagnostic;
    ASSERT_FALSE(strip.wkb.empty());
    OGRGeometry* gdal_strip = nullptr;
    ASSERT_EQ(OGRGeometryFactory::createFromWkb(
        strip.wkb.data(), nullptr, &gdal_strip,
        static_cast<int>(strip.wkb.size()), wkbVariantIso), OGRERR_NONE);
    ASSERT_NE(gdal_strip, nullptr);
    EXPECT_EQ(wkbFlatten(gdal_strip->getGeometryType()), wkbTIN);
    EXPECT_TRUE(wkbHasZ(gdal_strip->getGeometryType()));
    OGRGeometryFactory::destroyGeometry(gdal_strip);

    const auto outer_ring_blob =
        explorgdb_test::build_geom_multipatch_outer_ring_tin();
    const GeometryValue outer_ring_tin = decoder.decode_value(
        outer_ring_blob.data(), outer_ring_blob.size());
    ASSERT_EQ(outer_ring_tin.status, GeometryStatus::Valid)
        << outer_ring_tin.diagnostic;
    EXPECT_EQ(outer_ring_tin.geometry_type, 1016U);
    OGRGeometry* gdal_outer_ring_tin = nullptr;
    ASSERT_EQ(OGRGeometryFactory::createFromWkb(
        outer_ring_tin.wkb.data(), nullptr, &gdal_outer_ring_tin,
        static_cast<int>(outer_ring_tin.wkb.size()), wkbVariantIso),
        OGRERR_NONE);
    ASSERT_NE(gdal_outer_ring_tin, nullptr);
    EXPECT_EQ(wkbFlatten(gdal_outer_ring_tin->getGeometryType()), wkbTIN);
    OGRGeometryFactory::destroyGeometry(gdal_outer_ring_tin);

    const auto ring_blob = explorgdb_test::build_geom_multipatch_parts(
        {2, 3, 4, 5}, {4, 4, 4, 4}, true);
    const GeometryValue rings = decoder.decode_value(
        ring_blob.data(), ring_blob.size());
    ASSERT_EQ(rings.status, GeometryStatus::Valid) << rings.diagnostic;
    OGRGeometry* gdal_rings = nullptr;
    ASSERT_EQ(OGRGeometryFactory::createFromWkb(
        rings.wkb.data(), nullptr, &gdal_rings,
        static_cast<int>(rings.wkb.size()), wkbVariantIso), OGRERR_NONE);
    ASSERT_NE(gdal_rings, nullptr);
    EXPECT_EQ(wkbFlatten(gdal_rings->getGeometryType()), wkbMultiPolygon);
    EXPECT_TRUE(wkbHasZ(gdal_rings->getGeometryType()));
    EXPECT_TRUE(wkbHasM(gdal_rings->getGeometryType()));
    OGRGeometryFactory::destroyGeometry(gdal_rings);

    const auto mixed_blob = explorgdb_test::build_geom_multipatch_parts(
        {0, 2}, {4, 4});
    const GeometryValue mixed = decoder.decode_value(
        mixed_blob.data(), mixed_blob.size());
    ASSERT_EQ(mixed.status, GeometryStatus::Valid) << mixed.diagnostic;
    OGRGeometry* gdal_mixed = nullptr;
    ASSERT_EQ(OGRGeometryFactory::createFromWkb(
        mixed.wkb.data(), nullptr, &gdal_mixed,
        static_cast<int>(mixed.wkb.size()), wkbVariantIso), OGRERR_NONE);
    ASSERT_NE(gdal_mixed, nullptr);
    ASSERT_EQ(wkbFlatten(gdal_mixed->getGeometryType()),
              wkbGeometryCollection);
    auto* collection = gdal_mixed->toGeometryCollection();
    ASSERT_NE(collection, nullptr);
    ASSERT_EQ(collection->getNumGeometries(), 2);
    EXPECT_EQ(wkbFlatten(collection->getGeometryRef(0)->getGeometryType()),
              wkbTIN);
    EXPECT_EQ(wkbFlatten(collection->getGeometryRef(1)->getGeometryType()),
              wkbMultiPolygon);
    OGRGeometryFactory::destroyGeometry(gdal_mixed);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(RealDataReleaseContractTest, RegularFileGdbMatchesCoreReadContract) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_REAL_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_REAL_DATASET to a real non-curve .gdb directory";
    }
    ASSERT_TRUE(std::filesystem::is_directory(dataset_path)) << dataset_path;

    GDALAllRegister();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(
        static_cast<GDALDataset*>(GDALOpenEx(
            dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr)),
        GDALClose);
    ASSERT_NE(dataset.get(), nullptr) << dataset_path;
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    size_t opened_layers = 0;
    uint64_t compared_rows = 0;
    uint64_t compared_values = 0;
    uint64_t compared_geometries = 0;
    uint64_t empty_geometries = 0;
    uint64_t zero_length_rows = 0;
    uint64_t mismatches = 0;
    std::string first_mismatch;
    const auto report_mismatch = [&](const std::string& message) {
        ++mismatches;
        if (first_mismatch.empty()) first_mismatch = message;
    };

    for (int layer_index = 0;
         layer_index < dataset->GetLayerCount(); ++layer_index) {
        OGRLayer* layer = dataset->GetLayer(layer_index);
        ASSERT_NE(layer, nullptr);
        const std::string layer_name = layer->GetName();
        SCOPED_TRACE(layer_name);

        const auto resolved = resolver.resolve(layer_name);
        ASSERT_TRUE(resolved.has_value()) << "unresolved layer: " << layer_name;

        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open()) << "failed to open layer: " << layer_name;
        ASSERT_NE(engine.table(), nullptr);
        ++opened_layers;
        GdbTableParser& table = *engine.table();
        OGRFeatureDefn* gdal_definition = layer->GetLayerDefn();
        ASSERT_NE(gdal_definition, nullptr);

        std::vector<int> ogr_field_indexes(table.fields().size(), -1);
        size_t fast_geometry_fields = 0;
        size_t fast_attribute_fields = 0;
        for (size_t field_index = 0; field_index < table.fields().size();
             ++field_index) {
            const FieldDescriptor& field = table.fields()[field_index];
            if (field.type == FieldType::Geometry) {
                ++fast_geometry_fields;
                continue;
            }
            if (field.type == FieldType::ObjectId) continue;
            if (field.type == FieldType::Raster) continue;

            ++fast_attribute_fields;
            const int ogr_field_index =
                gdal_definition->GetFieldIndex(field.name.c_str());
            if (ogr_field_index < 0) {
                report_mismatch(layer_name + ": missing GDAL field " +
                                field.name);
                continue;
            }
            ogr_field_indexes[field_index] = ogr_field_index;

            OGRFieldType expected_type = OFTMaxType;
            if (!ogr_type_for_field(field.type, expected_type)) {
                report_mismatch(layer_name + "." + field.name +
                                ": unsupported FastGDB field type");
                continue;
            }
            if (gdal_definition->GetFieldDefn(ogr_field_index)->GetType() !=
                expected_type) {
                report_mismatch(layer_name + "." + field.name +
                                ": field type differs from GDAL");
            }
        }
        if (fast_geometry_fields !=
            static_cast<size_t>(gdal_definition->GetGeomFieldCount())) {
            report_mismatch(layer_name + ": geometry field count differs");
        }
        if (fast_attribute_fields !=
            static_cast<size_t>(gdal_definition->GetFieldCount())) {
            report_mismatch(layer_name + ": attribute field count differs");
        }

        const GIntBig gdal_count = layer->GetFeatureCount(TRUE);
        ASSERT_GE(gdal_count, 0) << "GDAL could not count layer " << layer_name;
        if (table.active_feature_count() != static_cast<uint64_t>(gdal_count)) {
            report_mismatch(layer_name + ": active feature count differs");
        }

        const auto geometry_indexes = geometry_field_indexes(table);
        size_t feature_index = 0;
        layer->ResetReading();
        while (OGRFeature* raw_feature = layer->GetNextFeature()) {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                feature(raw_feature, OGRFeature::DestroyFeature);
            const GIntBig gdal_fid = feature->GetFID();
            if (gdal_fid <= 0) {
                report_mismatch(layer_name +
                                ": GDAL returned a non-positive FID");
                continue;
            }

            const RowSlot row_slot = static_cast<RowSlot>(gdal_fid - 1);
            if (feature_index >= table.active_feature_count() ||
                table.active_feature_fid_at(feature_index) != row_slot) {
                report_mismatch(layer_name +
                                ": active FID order differs at GDAL FID " +
                                std::to_string(gdal_fid));
            }
            ++feature_index;
            ++compared_rows;

            if (!table.has_feature(row_slot)) {
                report_mismatch(layer_name +
                                ": FastGDB has no row for GDAL FID " +
                                std::to_string(gdal_fid));
                continue;
            }

            std::vector<uint8_t> raw_record;
            if (!table.read_raw_record_by_fid(row_slot, raw_record)) {
                report_mismatch(layer_name +
                                ": FastGDB failed to read raw row at FID " +
                                std::to_string(gdal_fid));
            } else if (raw_record.empty()) {
                ++zero_length_rows;
            }

            FeatureRecord record;
            if (!table.read_record_by_fid(row_slot, record)) {
                report_mismatch(layer_name +
                                ": FastGDB failed to read GDAL FID " +
                                std::to_string(gdal_fid));
                continue;
            }
            if (record.field_values.size() != table.fields().size()) {
                report_mismatch(layer_name +
                                ": FastGDB field value count differs at FID " +
                                std::to_string(gdal_fid));
                continue;
            }

            for (size_t field_index = 0;
                 field_index < table.fields().size(); ++field_index) {
                const FieldDescriptor& field = table.fields()[field_index];
                if (field.type == FieldType::Geometry ||
                    field.type == FieldType::Raster) {
                    continue;
                }
                const std::string mismatch = field_value_mismatch(
                    field, record.field_values[field_index], *feature,
                    ogr_field_indexes[field_index]);
                ++compared_values;
                if (!mismatch.empty()) {
                    report_mismatch(layer_name + "." + field.name +
                                    " at FID " + std::to_string(gdal_fid) +
                                    ": " + mismatch);
                }
            }

            if (!geometry_indexes.empty()) {
                GeometryValue fast_geometry;
                const bool fast_read =
                    table.read_geometry_value(row_slot, fast_geometry);
                const OGRGeometry* gdal_geometry = feature->GetGeometryRef();
                const bool gdal_empty =
                    gdal_geometry == nullptr || gdal_geometry->IsEmpty();
                if (gdal_empty) {
                    const bool fast_empty =
                        fast_geometry.status == GeometryStatus::Null ||
                        fast_geometry.status == GeometryStatus::Empty;
                    if (!fast_read || !fast_empty ||
                        !fast_geometry.wkb.empty()) {
                        report_mismatch(layer_name +
                                        ": NULL/EMPTY geometry differs at FID " +
                                        std::to_string(gdal_fid));
                    } else {
                        ++empty_geometries;
                    }
                } else if (!fast_read || !fast_geometry.valid() ||
                           fast_geometry.wkb.empty()) {
                    report_mismatch(layer_name +
                                    ": non-empty geometry is unreadable at FID " +
                                    std::to_string(gdal_fid));
                } else {
                    OGRGeometry* raw_fast_geometry = nullptr;
                    const OGRErr fast_wkb_status =
                        OGRGeometryFactory::createFromWkb(
                            fast_geometry.wkb.data(), nullptr,
                            &raw_fast_geometry,
                            static_cast<int>(fast_geometry.wkb.size()));
                    std::unique_ptr<
                        OGRGeometry,
                        decltype(&OGRGeometryFactory::destroyGeometry)>
                        fast_ogr_geometry(
                            raw_fast_geometry,
                            OGRGeometryFactory::destroyGeometry);
                    if (fast_wkb_status != OGRERR_NONE ||
                        fast_ogr_geometry == nullptr) {
                        report_mismatch(layer_name +
                                        ": FastGDB WKB is rejected by GDAL at FID " +
                                        std::to_string(gdal_fid));
                    } else {
                        std::unique_ptr<
                            OGRGeometry,
                            decltype(&OGRGeometryFactory::destroyGeometry)>
                            normalized_fast(
                                fast_ogr_geometry->Normalize(),
                                OGRGeometryFactory::destroyGeometry);
                        std::unique_ptr<
                            OGRGeometry,
                            decltype(&OGRGeometryFactory::destroyGeometry)>
                            normalized_gdal(
                                gdal_geometry->Normalize(),
                                OGRGeometryFactory::destroyGeometry);
                        if (normalized_fast == nullptr ||
                            normalized_gdal == nullptr ||
                            !geometry_coordinates_match(
                                *normalized_fast, *normalized_gdal)) {
                            report_mismatch(layer_name +
                                            ": geometry type/dimension/coordinates differ at FID " +
                                            std::to_string(gdal_fid));
                        }
                        ++compared_geometries;
                    }
                }
            }
        }
        if (feature_index != static_cast<size_t>(gdal_count)) {
            report_mismatch(layer_name + ": GDAL iteration count differs");
        }

        OGREnvelope extent;
        if (!geometry_indexes.empty() &&
            layer->GetExtent(&extent, TRUE) == OGRERR_NONE &&
            table.active_feature_count() > 0) {
            QueryRequest request;
            request.kind = QueryKind::SpatialBbox;
            request.xmin = extent.MinX;
            request.ymin = extent.MinY;
            request.xmax = extent.MaxX;
            request.ymax = extent.MaxY;
            const QueryResult result = engine.query(request);
            if (result.execution_path.empty() ||
                result.matched_fids.size() > table.active_feature_count()) {
                report_mismatch(layer_name +
                                ": full extent spatial query contract failed");
            }
        }
    }

    EXPECT_GT(opened_layers, 0u);
    EXPECT_GT(compared_rows, 0u);
    EXPECT_GT(compared_values, 0u);
    EXPECT_GT(compared_geometries, 0u);
    EXPECT_EQ(mismatches, 0u) << first_mismatch;
    RecordProperty("ComparedLayers", std::to_string(opened_layers));
    RecordProperty("ComparedRows", std::to_string(compared_rows));
    RecordProperty("ComparedAttributeValues", std::to_string(compared_values));
    RecordProperty("ComparedGeometries", std::to_string(compared_geometries));
    RecordProperty("EmptyGeometries", std::to_string(empty_geometries));
    RecordProperty("ZeroLengthRows", std::to_string(zero_length_rows));
}

TEST(RealDataReleaseContractTest,
     SuppliedGdbEveryFeatureRowAndAttributeMatchesGdal) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_CURVE_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to the supplied testcurve.gdb";
    }

    GDALAllRegister();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(
        static_cast<GDALDataset*>(GDALOpenEx(
            dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr)), GDALClose);
    ASSERT_NE(dataset, nullptr) << dataset_path;

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    uint64_t total_rows = 0;
    uint64_t compared_values = 0;
    uint64_t mismatches = 0;
    uint64_t raster_fields_outside_scope = 0;
    std::string first_mismatch;

    for (int layer_index = 0; layer_index < dataset->GetLayerCount();
         ++layer_index) {
        OGRLayer* layer = dataset->GetLayer(layer_index);
        ASSERT_NE(layer, nullptr);
        const std::string layer_name = layer->GetName();
        SCOPED_TRACE(layer_name);

        const auto resolved = resolver.resolve(layer_name);
        ASSERT_TRUE(resolved.has_value()) << "unresolved layer";
        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open()) << "failed to open layer";
        ASSERT_NE(engine.table(), nullptr);
        const GdbTableParser& table = *engine.table();
        OGRFeatureDefn* gdal_definition = layer->GetLayerDefn();
        ASSERT_NE(gdal_definition, nullptr);

        std::vector<int> ogr_field_indexes(table.fields().size(), -1);
        size_t fast_geometry_fields = 0;
        size_t fast_attribute_fields = 0;
        for (size_t field_index = 0; field_index < table.fields().size();
             ++field_index) {
            const FieldDescriptor& field = table.fields()[field_index];
            if (field.type == FieldType::Geometry) {
                ++fast_geometry_fields;
                continue;
            }
            if (field.type == FieldType::ObjectId) continue;
            if (field.type == FieldType::Raster) {
                ++raster_fields_outside_scope;
                continue;
            }

            ++fast_attribute_fields;
            const int ogr_field_index =
                gdal_definition->GetFieldIndex(field.name.c_str());
            if (ogr_field_index < 0) {
                ++mismatches;
                if (first_mismatch.empty()) {
                    first_mismatch = layer_name + ": missing GDAL field " +
                                     field.name;
                }
                continue;
            }
            ogr_field_indexes[field_index] = ogr_field_index;

            OGRFieldType expected_type = OFTMaxType;
            if (!ogr_type_for_field(field.type, expected_type)) {
                ++mismatches;
                if (first_mismatch.empty()) {
                    first_mismatch = layer_name + "." + field.name +
                                     ": unsupported FastGDB field type";
                }
                continue;
            }
            if (gdal_definition->GetFieldDefn(ogr_field_index)->GetType() !=
                expected_type) {
                ++mismatches;
                if (first_mismatch.empty()) {
                    first_mismatch = layer_name + "." + field.name +
                                     ": field type differs from GDAL";
                }
            }
        }

        EXPECT_EQ(fast_geometry_fields, gdal_definition->GetGeomFieldCount())
            << "geometry field count differs";
        EXPECT_EQ(fast_attribute_fields, gdal_definition->GetFieldCount())
            << "attribute field count differs";

        const GIntBig gdal_count = layer->GetFeatureCount(TRUE);
        ASSERT_GE(gdal_count, 0) << "GDAL could not count layer features";
        EXPECT_EQ(table.active_feature_count(),
                  static_cast<uint64_t>(gdal_count))
            << "active feature count differs";

        size_t feature_index = 0;
        layer->ResetReading();
        while (OGRFeature* raw_feature = layer->GetNextFeature()) {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                feature(raw_feature, OGRFeature::DestroyFeature);
            const GIntBig gdal_fid = feature->GetFID();
            if (gdal_fid <= 0) {
                ++mismatches;
                if (first_mismatch.empty())
                    first_mismatch = layer_name + ": GDAL returned a non-positive FID";
                continue;
            }

            const RowSlot row_slot = static_cast<RowSlot>(gdal_fid - 1);
            if (feature_index >= table.active_feature_count() ||
                table.active_feature_fid_at(feature_index) != row_slot) {
                ++mismatches;
                if (first_mismatch.empty()) {
                    first_mismatch = layer_name + ": active FID differs at GDAL FID " +
                                     std::to_string(gdal_fid);
                }
            }
            ++feature_index;
            ++total_rows;

            if (!table.has_feature(row_slot)) {
                ++mismatches;
                if (first_mismatch.empty()) {
                    first_mismatch = layer_name + ": FastGDB has no row for GDAL FID " +
                                     std::to_string(gdal_fid);
                }
                continue;
            }

            FeatureRecord record;
            if (!engine.table()->read_record_by_fid(row_slot, record)) {
                ++mismatches;
                if (first_mismatch.empty()) {
                    first_mismatch = layer_name + ": FastGDB failed to read GDAL FID " +
                                     std::to_string(gdal_fid);
                }
                continue;
            }
            if (record.field_values.size() != table.fields().size()) {
                ++mismatches;
                if (first_mismatch.empty()) {
                    first_mismatch = layer_name + ": FastGDB field value count differs at FID " +
                                     std::to_string(gdal_fid);
                }
                continue;
            }

            for (size_t field_index = 0; field_index < table.fields().size();
                 ++field_index) {
                const FieldDescriptor& field = table.fields()[field_index];
                if (field.type == FieldType::Geometry ||
                    field.type == FieldType::Raster) {
                    continue;
                }
                const std::string mismatch = field_value_mismatch(
                    field, record.field_values[field_index], *feature,
                    ogr_field_indexes[field_index]);
                ++compared_values;
                if (!mismatch.empty()) {
                    ++mismatches;
                    if (first_mismatch.empty()) {
                        first_mismatch = layer_name + "." + field.name +
                            " at FID " + std::to_string(gdal_fid) + ": " + mismatch;
                    }
                }
            }
        }

        EXPECT_EQ(feature_index, static_cast<size_t>(gdal_count))
            << "iterated feature count differs";
    }

    EXPECT_GT(dataset->GetLayerCount(), 0);
    EXPECT_GT(total_rows, 0u);
    EXPECT_GT(compared_values, 0u);
    EXPECT_EQ(mismatches, 0u) << first_mismatch;
    RecordProperty("ComparedLayers", dataset->GetLayerCount());
    RecordProperty("ComparedRows", std::to_string(total_rows));
    RecordProperty("ComparedAttributeValues", std::to_string(compared_values));
    RecordProperty("RasterFieldsOutsideVectorScope",
                   std::to_string(raster_fields_outside_scope));
}

static void verify_non_curve_geometry_dataset(
        const char* dataset_path, bool testcurve_contract) {
    GDALAllRegister();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(
        static_cast<GDALDataset*>(GDALOpenEx(
            dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr)), GDALClose);
    ASSERT_NE(dataset, nullptr) << dataset_path;

    const std::vector<std::string> curve_layers = {
        "Curve_Polyline_FC", "Curve_Polygon_FC", "Curve_Polyline_Z_FC",
        "Curve_Polyline_M_FC", "Curve_Polygon_MultiHole_FC",
        "Curve_Polygon_Island_FC", "Circle_FC", "Ellipse_FC",
        "RotatedEllipse_FC", "EllipseArc_FC"};
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    uint64_t total_rows = 0;
    uint64_t compared_geometries = 0;
    uint64_t empty_geometries = 0;
    uint64_t nonspatial_rows = 0;
    uint64_t curve_rows_delegated_to_curve_contract = 0;
    uint64_t explicit_topology_failures = 0;
    uint64_t mismatches = 0;
    std::string first_mismatch;

    const auto report_mismatch = [&](const std::string& message) {
        ++mismatches;
        if (first_mismatch.empty()) first_mismatch = message;
        ADD_FAILURE() << message;
    };

    for (int layer_index = 0; layer_index < dataset->GetLayerCount();
         ++layer_index) {
        OGRLayer* layer = dataset->GetLayer(layer_index);
        ASSERT_NE(layer, nullptr);
        const std::string layer_name = layer->GetName();
        if (testcurve_contract &&
            std::find(curve_layers.begin(), curve_layers.end(), layer_name) !=
                curve_layers.end()) {
            curve_rows_delegated_to_curve_contract +=
                static_cast<uint64_t>(std::max<GIntBig>(
                    0, layer->GetFeatureCount(TRUE)));
            continue;
        }

        SCOPED_TRACE(layer_name);
        const auto resolved = resolver.resolve(layer_name);
        ASSERT_TRUE(resolved.has_value()) << "unresolved layer";
        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open()) << "failed to open layer";
        ASSERT_NE(engine.table(), nullptr);
        const GdbTableParser& table = *engine.table();

        layer->ResetReading();
        while (OGRFeature* raw_feature = layer->GetNextFeature()) {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                feature(raw_feature, OGRFeature::DestroyFeature);
            const GIntBig gdal_fid = feature->GetFID();
            if (gdal_fid <= 0) {
                report_mismatch(layer_name + ": GDAL returned a non-positive FID");
                continue;
            }
            ++total_rows;

            const RowSlot row_slot = static_cast<RowSlot>(gdal_fid - 1);
            if (!table.has_feature(row_slot)) {
                report_mismatch(layer_name + ": no FastGDB row for FID " +
                                std::to_string(gdal_fid));
                continue;
            }

            if (layer->GetLayerDefn()->GetGeomFieldCount() == 0) {
                EXPECT_EQ(std::count_if(
                    table.fields().begin(), table.fields().end(),
                    [](const FieldDescriptor& field) {
                        return field.type == FieldType::Geometry;
                    }), 0);
                EXPECT_EQ(feature->GetGeometryRef(), nullptr);
                ++nonspatial_rows;
                continue;
            }

            GeometryValue fast_value;
            const bool fast_read =
                engine.table()->read_geometry_value(row_slot, fast_value);
            const OGRGeometry* gdal_geometry = feature->GetGeometryRef();
            const bool gdal_empty =
                gdal_geometry == nullptr || gdal_geometry->IsEmpty();

            if (testcurve_contract && layer_name == "SelfIntersect_Polygon" &&
                fast_value.status == GeometryStatus::TouchingRings) {
                if (fast_read || !fast_value.wkb.empty() ||
                    fast_value.diagnostic.empty()) {
                    report_mismatch(layer_name + ": expected fail-closed topology status");
                } else {
                    ++explicit_topology_failures;
                }
                continue;
            }

            if (gdal_empty) {
                const bool fast_empty =
                    fast_value.status == GeometryStatus::Null ||
                    fast_value.status == GeometryStatus::Empty;
                const bool known_degenerate_ring =
                    testcurve_contract && layer_name == "DegenerateRing_Polygon" &&
                    fast_value.status == GeometryStatus::Null;
                if (!fast_empty || !fast_read || !fast_value.wkb.empty() ||
                    (!known_degenerate_ring && gdal_geometry != nullptr &&
                     fast_value.status != GeometryStatus::Empty) ||
                    (gdal_geometry == nullptr &&
                     fast_value.status != GeometryStatus::Null)) {
                    report_mismatch(layer_name + ": NULL/EMPTY geometry state differs at FID " +
                                    std::to_string(gdal_fid) + ", status=" +
                                    std::to_string(static_cast<int>(fast_value.status)));
                } else {
                    ++empty_geometries;
                }
                continue;
            }

            if (!fast_read || !fast_value.valid() || fast_value.wkb.empty()) {
                report_mismatch(layer_name + ": non-empty GDAL geometry is not readable at FID " +
                                std::to_string(gdal_fid) + ", status=" +
                                std::to_string(static_cast<int>(fast_value.status)) +
                                ", diagnostic=" + fast_value.diagnostic);
                continue;
            }

            OGRGeometry* raw_fast_geometry = nullptr;
            const OGRErr fast_wkb_status = OGRGeometryFactory::createFromWkb(
                fast_value.wkb.data(), nullptr, &raw_fast_geometry,
                static_cast<int>(fast_value.wkb.size()));
            std::unique_ptr<OGRGeometry,
                            decltype(&OGRGeometryFactory::destroyGeometry)>
                fast_geometry(raw_fast_geometry,
                              OGRGeometryFactory::destroyGeometry);
            if (fast_wkb_status != OGRERR_NONE || fast_geometry == nullptr) {
                report_mismatch(layer_name + ": FastGDB WKB is rejected by GDAL at FID " +
                                std::to_string(gdal_fid));
                continue;
            }

            std::unique_ptr<OGRGeometry,
                            decltype(&OGRGeometryFactory::destroyGeometry)>
                normalized_fast(fast_geometry->Normalize(),
                                OGRGeometryFactory::destroyGeometry);
            std::unique_ptr<OGRGeometry,
                            decltype(&OGRGeometryFactory::destroyGeometry)>
                normalized_gdal(gdal_geometry->Normalize(),
                                OGRGeometryFactory::destroyGeometry);
            if (normalized_fast == nullptr || normalized_gdal == nullptr ||
                !geometry_coordinates_match(*normalized_fast, *normalized_gdal)) {
                report_mismatch(layer_name + ": normalized geometry coordinates/dimensions "
                                "differ at FID " + std::to_string(gdal_fid));
            }
            ++compared_geometries;
        }
    }

    EXPECT_GT(total_rows, 0u);
    EXPECT_GT(compared_geometries, 0u);
    if (testcurve_contract) {
        EXPECT_EQ(curve_rows_delegated_to_curve_contract, 27u)
            << "curve geometries are verified by the existing curve preservation sweep";
        EXPECT_EQ(explicit_topology_failures, 1u)
            << "the self-intersecting sample must remain explicitly fail-closed";
    }
    testing::Test::RecordProperty("NonspatialRows", std::to_string(nonspatial_rows));
    EXPECT_EQ(mismatches, 0u) << first_mismatch;
    testing::Test::RecordProperty("TotalNonCurveRows", std::to_string(total_rows));
    testing::Test::RecordProperty("ComparedGeometries", std::to_string(compared_geometries));
    testing::Test::RecordProperty("EmptyGeometries", std::to_string(empty_geometries));
    testing::Test::RecordProperty("CurveRowsCoveredByCurveContract",
                   std::to_string(curve_rows_delegated_to_curve_contract));
    testing::Test::RecordProperty("ExplicitTopologyFailures",
                   std::to_string(explicit_topology_failures));
}

TEST(RealDataReleaseContractTest,
     SuppliedGdbEveryNonCurveGeometryMatchesGdal) {
    const char* path = required_dataset_from_env("FAST_GDB_CURVE_DATASET");
    if (path == nullptr) GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to testcurve.gdb";
    verify_non_curve_geometry_dataset(path, true);
}

TEST(RealDataReleaseContractTest,
     HistoricalGdbEveryNonCurveGeometryMatchesGdal) {
    const char* path = required_dataset_from_env("FAST_GDB_HISTORICAL_DATASET");
    if (path == nullptr) GTEST_SKIP() << "Set FAST_GDB_HISTORICAL_DATASET";
    verify_non_curve_geometry_dataset(path, false);
}

TEST(RealDataReleaseContractTest,
     HistoricalAttributeIndexesMatchIndependentRecordScan) {
    const char* path = required_dataset_from_env("FAST_GDB_HISTORICAL_DATASET");
    if (path == nullptr) GTEST_SKIP() << "Set FAST_GDB_HISTORICAL_DATASET";
    GDALAllRegister();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> reference(
        static_cast<GDALDataset*>(GDALOpenEx(path,
            GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
    ASSERT_NE(reference, nullptr);
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    uint64_t tested_indexes = 0, queries = 0, skipped_types = 0, combined_queries = 0;
    const auto matches = [](int order, AttrOp op) {
        switch (op) {
            case AttrOp::Eq: return order == 0;
            case AttrOp::Ne: return order != 0;
            case AttrOp::Lt: return order < 0;
            case AttrOp::Le: return order <= 0;
            case AttrOp::Gt: return order > 0;
            case AttrOp::Ge: return order >= 0;
        }
        return false;
    };
    for (int i = 0; i < reference->GetLayerCount(); ++i) {
        OGRLayer* source = reference->GetLayer(i);
        ASSERT_NE(source, nullptr);
        SCOPED_TRACE(source->GetName());
        const auto resolved = resolver.resolve(source->GetName());
        ASSERT_TRUE(resolved.has_value());
        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open());
        const auto* metadata_file = catalog.find_indexes(resolved->id);
        if (metadata_file == nullptr) continue;
        std::ifstream input(std::filesystem::u8path(catalog.path()) / metadata_file->filename,
                            std::ios::binary);
        unsigned char signature[4]{};
        input.read(reinterpret_cast<char*>(signature), 4);
        ASSERT_TRUE(input.good());
        if (signature[0] == 0x13 && signature[1] == 0x98 &&
            signature[2] == 0x85 && signature[3] == 0x03) {
            GTEST_SKIP() << "v9 index metadata layout unsupported; native ATX field discovery not verified";
        }
        std::vector<IndexEntry> indexes;
        ASSERT_TRUE(catalog.read_index_metadata(resolved->id, indexes));
        for (const auto& index : indexes) {
            if (catalog.find_atx(resolved->id, index.name) == nullptr ||
                !GdbIndexesParser::is_direct_field_expression(index.column_name))
                continue;
            const std::string column =
                GdbIndexesParser::field_name_from_expression(index.column_name);
            const int field = source->GetLayerDefn()->GetFieldIndex(column.c_str());
            if (field < 0) continue; // ObjectID is exposed separately by GDAL.
            SCOPED_TRACE(index.name + "/" + column);
            const OGRFieldType type = source->GetLayerDefn()->GetFieldDefn(field)->GetType();
            const auto native = std::find_if(engine.table()->fields().begin(),
                engine.table()->fields().end(), [&](const FieldDescriptor& descriptor) {
                    return descriptor.name == column;
                });
            ASSERT_NE(native, engine.table()->fields().end());
            const bool string_field = native->type == FieldType::String;
            if ((!string_field && type != OFTInteger && type != OFTReal) ||
                (type == OFTString && !string_field)) {
                ++skipped_types;
                continue;
            }
            struct Row { RowSlot fid; double numeric; std::string text; bool spatial; };
            std::vector<Row> rows;
            std::vector<double> numeric_probes = {-1.0, 0.0, 1.0};
            std::vector<std::string> string_probes = {"", "unmatched-index-value"};
            source->SetSpatialFilter(nullptr);
            ASSERT_EQ(source->SetAttributeFilter(nullptr), OGRERR_NONE);
            source->ResetReading();
            while (OGRFeature* raw = source->GetNextFeature()) {
                std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                    feature(raw, OGRFeature::DestroyFeature);
                if (!feature->IsFieldSetAndNotNull(field)) continue;
                ASSERT_GT(feature->GetFID(), 0);
                const auto* geometry = feature->GetGeometryRef();
                Row row{static_cast<RowSlot>(feature->GetFID() - 1), 0, {},
                        geometry != nullptr && !geometry->IsEmpty()};
                if (string_field) {
                    row.text = feature->GetFieldAsString(field);
                    string_probes.push_back(row.text);
                } else {
                    row.numeric = feature->GetFieldAsDouble(field);
                    ASSERT_TRUE(std::isfinite(row.numeric));
                    numeric_probes.push_back(row.numeric);
                    numeric_probes.push_back(row.numeric - 1);
                    numeric_probes.push_back(row.numeric + 1);
                }
                rows.push_back(std::move(row));
            }
            std::sort(numeric_probes.begin(), numeric_probes.end());
            numeric_probes.erase(std::unique(numeric_probes.begin(), numeric_probes.end()),
                                 numeric_probes.end());
            std::sort(string_probes.begin(), string_probes.end());
            string_probes.erase(std::unique(string_probes.begin(), string_probes.end()),
                                string_probes.end());
            const size_t probe_count = string_field ? string_probes.size() : numeric_probes.size();
            ++tested_indexes;
            for (size_t probe = 0; probe < probe_count; ++probe) {
                for (AttrOp op : {AttrOp::Eq, AttrOp::Ne, AttrOp::Lt,
                                  AttrOp::Le, AttrOp::Gt, AttrOp::Ge}) {
                    QueryRequest request;
                    request.kind = string_field ? QueryKind::AttributeString
                                                : QueryKind::AttributeDouble;
                    request.index_name = index.name;
                    request.attr_op = op;
                    if (string_field) request.string_value = string_probes[probe];
                    else request.double_value = numeric_probes[probe];
                    std::vector<RowSlot> expected;
                    for (const auto& row : rows) {
                        const int order = string_field
                            ? row.text.compare(request.string_value)
                            : (row.numeric < request.double_value ? -1
                               : (row.numeric > request.double_value ? 1 : 0));
                        if (matches(order, op)) expected.push_back(row.fid);
                    }
                    const auto result = engine.query(request);
                    ASSERT_EQ(result.status, QueryStatus::Ok) << result.error;
                    EXPECT_EQ(result.execution_path, kPathAttributeAtx)
                        << result.fallback_reason;
                    auto observed = result.matched_fids;
                    std::sort(expected.begin(), expected.end());
                    std::sort(observed.begin(), observed.end());
                    EXPECT_EQ(observed, expected)
                        << "op=" << static_cast<int>(op)
                        << " probe=" << request.double_value << '/' << request.string_value;
                    if ((native->type == FieldType::Int16 ||
                         native->type == FieldType::Float32) && op != AttrOp::Ne &&
                        source->GetLayerDefn()->GetGeomFieldCount() > 0) {
                        OGREnvelope extent;
                        ASSERT_EQ(source->GetExtent(&extent, TRUE), OGRERR_NONE);
                        const char* symbol = "=";
                        switch (op) {
                            case AttrOp::Eq: break;
                            case AttrOp::Lt: symbol = "<"; break;
                            case AttrOp::Le: symbol = "<="; break;
                            case AttrOp::Gt: symbol = ">"; break;
                            case AttrOp::Ge: symbol = ">="; break;
                            case AttrOp::Ne: break;
                        }
                        std::ostringstream predicate;
                        predicate << column << ' ' << symbol << ' '
                                  << std::setprecision(std::numeric_limits<double>::max_digits10)
                                  << request.double_value;
                        QueryRequest combined;
                        combined.kind = QueryKind::SpatialWhere;
                        combined.where_clause = predicate.str();
                        combined.xmin = extent.MinX - 1;
                        combined.ymin = extent.MinY - 1;
                        combined.xmax = extent.MaxX + 1;
                        combined.ymax = extent.MaxY + 1;
                        std::vector<RowSlot> spatial_expected;
                        for (const auto& row : rows) {
                            if (row.spatial && std::binary_search(expected.begin(), expected.end(), row.fid))
                                spatial_expected.push_back(row.fid);
                        }
                        std::sort(spatial_expected.begin(), spatial_expected.end());
                        const auto fused = engine.query(combined);
                        ASSERT_EQ(fused.status, QueryStatus::Ok) << fused.error;
                        EXPECT_TRUE(fused.combined_metrics.used_attribute_index)
                            << fused.fallback_reason;
                        auto fused_fids = fused.matched_fids;
                        std::sort(fused_fids.begin(), fused_fids.end());
                        EXPECT_EQ(fused_fids, spatial_expected) << predicate.str();
                        ++combined_queries;
                    }
                    ++queries;
                }
            }
        }
    }
    EXPECT_GT(tested_indexes, 0U);
    EXPECT_GT(queries, 0U);
    RecordProperty("CombinedIndexedQueries", std::to_string(combined_queries));
    RecordProperty("TestedIndexes", std::to_string(tested_indexes));
    RecordProperty("ComparedQueries", std::to_string(queries));
    RecordProperty("IndexesWithOtherFieldTypes", std::to_string(skipped_types));
}

TEST(RealDataReleaseContractTest, HistoricalDatePredicatesMatchIndependentRecordScan) {
    const char* path = required_dataset_from_env("FAST_GDB_HISTORICAL_DATASET");
    if (path == nullptr) GTEST_SKIP() << "Set FAST_GDB_HISTORICAL_DATASET";
    GDALAllRegister();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> reference(
        static_cast<GDALDataset*>(GDALOpenEx(path,
            GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
    ASSERT_NE(reference, nullptr);
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto timestamp_text = [](int64_t milliseconds) {
        int64_t seconds = milliseconds / 1000;
        int64_t remainder = milliseconds % 1000;
        if (remainder < 0) { --seconds; remainder += 1000; }
        std::tm time{};
        if (CPLUnixTimeToYMDHMS(seconds, &time) == nullptr) return std::string{};
        std::ostringstream text;
        text.imbue(std::locale::classic());
        text << std::setfill('0') << std::setw(4) << time.tm_year + 1900 << '-'
             << std::setw(2) << time.tm_mon + 1 << '-' << std::setw(2) << time.tm_mday
             << ' ' << std::setw(2) << time.tm_hour << ':' << std::setw(2) << time.tm_min
             << ':' << std::setw(2) << time.tm_sec << '.' << std::setw(3) << remainder;
        return text.str();
    };
    uint64_t where_queries = 0, combined_queries = 0, indexed_queries = 0;
    for (int i = 0; i < reference->GetLayerCount(); ++i) {
        OGRLayer* source = reference->GetLayer(i);
        ASSERT_NE(source, nullptr);
        SCOPED_TRACE(source->GetName());
        const auto resolved = resolver.resolve(source->GetName());
        ASSERT_TRUE(resolved.has_value());
        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open());
        std::vector<IndexEntry> indexes;
        catalog.read_index_metadata(resolved->id, indexes);
        for (const auto& field : engine.table()->fields()) {
            if (field.type != FieldType::DateTime) continue;
            SCOPED_TRACE(field.name);
            const int source_field = source->GetLayerDefn()->GetFieldIndex(field.name.c_str());
            ASSERT_GE(source_field, 0);
            struct Row { RowSlot fid; int64_t milliseconds; bool spatial; };
            std::vector<Row> rows;
            std::vector<int64_t> probes = {0};
            source->SetSpatialFilter(nullptr);
            ASSERT_EQ(source->SetAttributeFilter(nullptr), OGRERR_NONE);
            source->ResetReading();
            while (OGRFeature* raw = source->GetNextFeature()) {
                std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                    feature(raw, OGRFeature::DestroyFeature);
                if (!feature->IsFieldSetAndNotNull(source_field)) continue;
                ASSERT_GT(feature->GetFID(), 0);
                int year = 0, month = 0, day = 0, hour = 0, minute = 0, timezone = 0;
                float second = 0;
                ASSERT_TRUE(feature->GetFieldAsDateTime(source_field, &year, &month, &day,
                    &hour, &minute, &second, &timezone));
                std::tm time{};
                time.tm_year = year - 1900;
                time.tm_mon = month - 1;
                time.tm_mday = day;
                time.tm_hour = hour;
                time.tm_min = minute;
                const int64_t milliseconds = CPLYMDHMSToUnixTime(&time) * 1000 +
                    static_cast<int64_t>(std::llround(static_cast<double>(second) * 1000));
                const auto* geometry = feature->GetGeometryRef();
                rows.push_back({static_cast<RowSlot>(feature->GetFID() - 1), milliseconds,
                                geometry != nullptr && !geometry->IsEmpty()});
                probes.push_back(milliseconds - 1);
                probes.push_back(milliseconds);
                probes.push_back(milliseconds + 1);
            }
            std::sort(probes.begin(), probes.end());
            probes.erase(std::unique(probes.begin(), probes.end()), probes.end());
            const bool indexed = std::any_of(indexes.begin(), indexes.end(), [&](const IndexEntry& index) {
                return GdbIndexesParser::is_direct_field_expression(index.column_name) &&
                    GdbIndexesParser::field_name_from_expression(index.column_name) == field.name &&
                    catalog.find_atx(resolved->id, index.name) != nullptr;
            });
            const bool spatial = std::any_of(rows.begin(), rows.end(),
                [](const Row& row) { return row.spatial; });
            OGREnvelope extent;
            if (spatial) {
                ASSERT_EQ(source->GetExtent(&extent, TRUE), OGRERR_NONE);
            }
            const char* symbols[] = {"=", "!=", "<", "<=", ">", ">="};
            for (int64_t probe : probes) {
                for (size_t op = 0; op < 6; ++op) {
                    std::vector<RowSlot> expected, spatial_expected;
                    for (const auto& row : rows) {
                        const int order = (row.milliseconds > probe) - (row.milliseconds < probe);
                        const bool matches[] = {order == 0, order != 0, order < 0,
                                                order <= 0, order > 0, order >= 0};
                        if (matches[op]) {
                            expected.push_back(row.fid);
                            if (row.spatial) spatial_expected.push_back(row.fid);
                        }
                    }
                    std::sort(expected.begin(), expected.end());
                    std::sort(spatial_expected.begin(), spatial_expected.end());
                    QueryRequest request;
                    request.kind = QueryKind::WhereClause;
                    const std::string text = timestamp_text(probe);
                    ASSERT_FALSE(text.empty());
                    request.where_clause = field.name + " " + symbols[op] + " TIMESTAMP '" + text + "'";
                    auto result = engine.query(request);
                    ASSERT_EQ(result.status, QueryStatus::Ok) << result.error;
                    std::sort(result.matched_fids.begin(), result.matched_fids.end());
                    EXPECT_EQ(result.matched_fids, expected) << request.where_clause;
                    ++where_queries;
                    if (!spatial) continue;
                    request.kind = QueryKind::SpatialWhere;
                    request.xmin = extent.MinX - 1;
                    request.ymin = extent.MinY - 1;
                    request.xmax = extent.MaxX + 1;
                    request.ymax = extent.MaxY + 1;
                    auto combined = engine.query(request);
                    ASSERT_EQ(combined.status, QueryStatus::Ok) << combined.error;
                    std::sort(combined.matched_fids.begin(), combined.matched_fids.end());
                    EXPECT_EQ(combined.matched_fids, spatial_expected) << request.where_clause;
                    if (indexed && op != 1) {
                        EXPECT_TRUE(combined.combined_metrics.used_attribute_index)
                            << request.where_clause << ": " << combined.fallback_reason;
                        ++indexed_queries;
                    }
                    ++combined_queries;
                }
            }
        }
    }
    EXPECT_GT(where_queries, 0U);
    RecordProperty("DateWhereQueries", std::to_string(where_queries));
    RecordProperty("DateCombinedQueries", std::to_string(combined_queries));
    RecordProperty("DateNativeCombinedQueries", std::to_string(indexed_queries));
}

TEST(RealDataReleaseContractTest, HistoricalUuidPredicatesMatchIndependentRecordScan) {
    const char* path = required_dataset_from_env("FAST_GDB_HISTORICAL_DATASET");
    if (path == nullptr) GTEST_SKIP() << "Set FAST_GDB_HISTORICAL_DATASET";
    GDALAllRegister();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> reference(
        static_cast<GDALDataset*>(GDALOpenEx(path,
            GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
    ASSERT_NE(reference, nullptr);
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto canonical = [](std::string value) {
        if (value.size() == 38) value = value.substr(1, 36);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    };
    uint64_t queries = 0, indexed_queries = 0, combined_queries = 0, indexed_combined = 0;
    for (int i = 0; i < reference->GetLayerCount(); ++i) {
        OGRLayer* source = reference->GetLayer(i);
        ASSERT_NE(source, nullptr);
        SCOPED_TRACE(source->GetName());
        const auto resolved = resolver.resolve(source->GetName());
        ASSERT_TRUE(resolved.has_value());
        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open());
        std::vector<IndexEntry> indexes;
        catalog.read_index_metadata(resolved->id, indexes);
        for (const auto& field : engine.table()->fields()) {
            if (field.type != FieldType::UUID_1 && field.type != FieldType::UUID_2) continue;
            SCOPED_TRACE(field.name);
            const int source_field = source->GetLayerDefn()->GetFieldIndex(field.name.c_str());
            ASSERT_GE(source_field, 0);
            std::vector<std::pair<RowSlot, std::string>> rows;
            std::vector<RowSlot> spatial_fids;
            std::vector<std::string> probes = {
                "00000000-0000-0000-0000-000000000000",
                "ffffffff-ffff-ffff-ffff-ffffffffffff"};
            source->SetSpatialFilter(nullptr);
            ASSERT_EQ(source->SetAttributeFilter(nullptr), OGRERR_NONE);
            source->ResetReading();
            while (OGRFeature* raw = source->GetNextFeature()) {
                std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                    feature(raw, OGRFeature::DestroyFeature);
                if (!feature->IsFieldSetAndNotNull(source_field)) continue;
                ASSERT_GT(feature->GetFID(), 0);
                const auto text = canonical(feature->GetFieldAsString(source_field));
                ASSERT_EQ(text.size(), 36U);
                rows.emplace_back(static_cast<RowSlot>(feature->GetFID() - 1), text);
                probes.push_back(text);
                const auto* geometry = feature->GetGeometryRef();
                if (geometry != nullptr && !geometry->IsEmpty())
                    spatial_fids.push_back(static_cast<RowSlot>(feature->GetFID() - 1));
            }
            std::sort(probes.begin(), probes.end());
            probes.erase(std::unique(probes.begin(), probes.end()), probes.end());
            std::sort(spatial_fids.begin(), spatial_fids.end());
            OGREnvelope extent;
            if (!spatial_fids.empty()) {
                ASSERT_EQ(source->GetExtent(&extent, TRUE), OGRERR_NONE);
            }
            const auto index = std::find_if(indexes.begin(), indexes.end(), [&](const IndexEntry& entry) {
                return GdbIndexesParser::is_direct_field_expression(entry.column_name) &&
                    GdbIndexesParser::field_name_from_expression(entry.column_name) == field.name &&
                    catalog.find_atx(resolved->id, entry.name) != nullptr;
            });
            if (index != indexes.end()) {
                QueryRequest invalid;
                invalid.kind = QueryKind::AttributeString;
                invalid.index_name = index->name;
                invalid.string_value = "invalid-uuid";
                const auto result = engine.query(invalid);
                EXPECT_EQ(result.status, QueryStatus::InvalidRequest);
                EXPECT_FALSE(result.error.empty());
                invalid.kind = QueryKind::AttributeDouble;
                EXPECT_EQ(engine.query(invalid).status, QueryStatus::InvalidRequest);
            }
            const char* symbols[] = {"=", "!=", "<", "<=", ">", ">="};
            const AttrOp operations[] = {AttrOp::Eq, AttrOp::Ne, AttrOp::Lt,
                                         AttrOp::Le, AttrOp::Gt, AttrOp::Ge};
            for (const auto& probe : probes) {
                for (size_t op = 0; op < 6; ++op) {
                    std::vector<RowSlot> expected;
                    for (const auto& row : rows) {
                        const int order = row.second.compare(probe);
                        const bool matches[] = {order == 0, order != 0, order < 0,
                                                order <= 0, order > 0, order >= 0};
                        if (matches[op]) expected.push_back(row.first);
                    }
                    std::sort(expected.begin(), expected.end());
                    for (bool braced : {false, true}) {
                        std::string literal = probe;
                        if (braced) {
                            std::transform(literal.begin(), literal.end(), literal.begin(),
                                [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                            literal = "{" + literal + "}";
                        }
                        QueryRequest request;
                        request.kind = QueryKind::WhereClause;
                        request.where_clause = field.name + " " + symbols[op] + " '" + literal + "'";
                        auto result = engine.query(request);
                        ASSERT_EQ(result.status, QueryStatus::Ok) << result.error;
                        std::sort(result.matched_fids.begin(), result.matched_fids.end());
                        EXPECT_EQ(result.matched_fids, expected) << request.where_clause;
                        ++queries;
                        if (index != indexes.end()) {
                            request.kind = QueryKind::AttributeString;
                            request.index_name = index->name;
                            request.string_value = literal;
                            request.attr_op = operations[op];
                            auto indexed = engine.query(request);
                            ASSERT_EQ(indexed.status, QueryStatus::Ok) << indexed.error;
                            EXPECT_EQ(indexed.execution_path, kPathAttributeAtx);
                            EXPECT_TRUE(indexed.fallback_reason.empty());
                            std::sort(indexed.matched_fids.begin(), indexed.matched_fids.end());
                            EXPECT_EQ(indexed.matched_fids, expected) << request.where_clause;
                            ++indexed_queries;
                        }
                        if (!spatial_fids.empty()) {
                            std::vector<RowSlot> spatial_expected;
                            std::set_intersection(expected.begin(), expected.end(),
                                spatial_fids.begin(), spatial_fids.end(),
                                std::back_inserter(spatial_expected));
                            request.kind = QueryKind::SpatialWhere;
                            request.xmin = extent.MinX - 1;
                            request.ymin = extent.MinY - 1;
                            request.xmax = extent.MaxX + 1;
                            request.ymax = extent.MaxY + 1;
                            auto combined = engine.query(request);
                            ASSERT_EQ(combined.status, QueryStatus::Ok) << combined.error;
                            std::sort(combined.matched_fids.begin(), combined.matched_fids.end());
                            EXPECT_EQ(combined.matched_fids, spatial_expected) << request.where_clause;
                            if (index != indexes.end()) {
                                EXPECT_TRUE(combined.combined_metrics.used_attribute_index)
                                    << combined.fallback_reason;
                                ++indexed_combined;
                            }
                            ++combined_queries;
                        }
                    }
                }
            }
        }
    }
    EXPECT_GT(queries, 0U);
    RecordProperty("UuidWhereQueries", std::to_string(queries));
    RecordProperty("UuidNativeIndexQueries", std::to_string(indexed_queries));
    RecordProperty("UuidCombinedQueries", std::to_string(combined_queries));
    RecordProperty("UuidNativeCombinedQueries", std::to_string(indexed_combined));
}

TEST(RealDataReleaseContractTest,
     HistoricalUnknownIndexMetadataReturnsUnsupported) {
    const char* path = required_dataset_from_env("FAST_GDB_HISTORICAL_DATASET");
    if (path == nullptr) GTEST_SKIP() << "Set FAST_GDB_HISTORICAL_DATASET";
    GDALAllRegister();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> reference(
        static_cast<GDALDataset*>(GDALOpenEx(path,
            GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
    ASSERT_NE(reference, nullptr);
    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    size_t rejected = 0;
    // System catalog names may include objects without a physical table.
    // Exercise each readable layer exposed by the independent GDAL reference.
    for (int i = 0; i < reference->GetLayerCount(); ++i) {
        OGRLayer* source = reference->GetLayer(i);
        ASSERT_NE(source, nullptr);
        SCOPED_TRACE(source->GetName());
        const auto resolved = resolver.resolve(source->GetName());
        ASSERT_TRUE(resolved.has_value());
        const auto files = catalog.find_all_atx(resolved->id);
        if (files.empty()) continue;
        std::vector<IndexEntry> metadata;
        if (catalog.read_index_metadata(resolved->id, metadata)) continue;
        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open());
        for (const auto* file : files) {
            ASSERT_GT(file->extension.size(), 5U);
            QueryRequest request;
            request.kind = QueryKind::AttributeDouble;
            request.index_name = file->extension.substr(1, file->extension.size() - 5);
            const auto result = engine.query(request);
            EXPECT_EQ(result.status, QueryStatus::Unsupported);
            EXPECT_FALSE(result.error.empty());
            EXPECT_TRUE(result.matched_fids.empty());
            ++rejected;
        }
    }
    if (rejected == 0) GTEST_SKIP() << "dataset has no ATX with unparsed metadata";
    RecordProperty("RejectedAmbiguousIndexQueries", std::to_string(rejected));
}

TEST(RealDataReleaseContractTest,
     HistoricalSpatialQueriesMatchIndependentGeometryScan) {
    const char* path = required_dataset_from_env("FAST_GDB_HISTORICAL_DATASET");
    if (path == nullptr) GTEST_SKIP() << "Set FAST_GDB_HISTORICAL_DATASET";
    GDALAllRegister();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> reference(
        static_cast<GDALDataset*>(GDALOpenEx(
            path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr)), GDALClose);
    ASSERT_NE(reference, nullptr);
    ReaderError error;
    auto reader = Reader::open(path, {}, &error);
    ASSERT_TRUE(reader.has_value()) << error.message;
    uint64_t layers = 0, windows = 0, indexed_windows = 0, hits = 0;
    for (int index = 0; index < reference->GetLayerCount(); ++index) {
        OGRLayer* source = reference->GetLayer(index);
        ASSERT_NE(source, nullptr);
        if (source->GetLayerDefn()->GetGeomFieldCount() == 0) continue;
        SCOPED_TRACE(source->GetName());
        auto layer = reader->open_layer(source->GetName(), &error);
        ASSERT_TRUE(layer.has_value()) << error.message;
        const auto object_id = std::find_if(
            layer->fields().begin(), layer->fields().end(),
            [](const FieldDescriptor& field) {
                return field.type == FieldType::ObjectId;
            });
        ASSERT_NE(object_id, layer->fields().end());
        struct Row {
            RowSlot fid;
            std::unique_ptr<OGRGeometry> geometry;
        };
        std::vector<Row> rows;
        OGREnvelope extent;
        source->SetSpatialFilter(nullptr);
        source->ResetReading();
        while (OGRFeature* raw = source->GetNextFeature()) {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                feature(raw, OGRFeature::DestroyFeature);
            const OGRGeometry* geometry = feature->GetGeometryRef();
            if (geometry == nullptr || geometry->IsEmpty()) continue;
            ASSERT_GT(feature->GetFID(), 0);
            OGREnvelope envelope;
            geometry->getEnvelope(&envelope);
            if (rows.empty()) extent = envelope;
            else extent.Merge(envelope);
            rows.push_back({static_cast<RowSlot>(feature->GetFID() - 1),
                            std::unique_ptr<OGRGeometry>(geometry->clone())});
        }
        if (rows.empty()) continue;
        ++layers;
        const double mx = extent.MinX + (extent.MaxX - extent.MinX) / 2;
        const double my = extent.MinY + (extent.MaxY - extent.MinY) / 2;
        const double gap = std::max({1.0, extent.MaxX - extent.MinX,
                                     extent.MaxY - extent.MinY});
        struct Window { double xmin, ymin, xmax, ymax; };
        const std::vector<Window> boxes = {
            {extent.MinX, extent.MinY, extent.MaxX, extent.MaxY},
            {extent.MinX, extent.MinY, mx, my},
            {mx, extent.MinY, extent.MaxX, my},
            {extent.MinX, my, mx, extent.MaxY},
            {mx, my, extent.MaxX, extent.MaxY},
            {extent.MaxX + gap, extent.MaxY + gap,
             extent.MaxX + 2 * gap, extent.MaxY + 2 * gap}};
        for (const auto& box : boxes) {
            SCOPED_TRACE(testing::Message() << box.xmin << ',' << box.ymin
                << " to " << box.xmax << ',' << box.ymax);
            std::unique_ptr<OGRGeometry> filter;
            if (box.xmin == box.xmax && box.ymin == box.ymax) {
                filter = std::make_unique<OGRPoint>(box.xmin, box.ymin);
            } else if (box.xmin == box.xmax || box.ymin == box.ymax) {
                auto line = std::make_unique<OGRLineString>();
                line->addPoint(box.xmin, box.ymin);
                line->addPoint(box.xmax, box.ymax);
                filter = std::move(line);
            } else {
                auto polygon = std::make_unique<OGRPolygon>();
                OGRLinearRing ring;
                ring.addPoint(box.xmin, box.ymin);
                ring.addPoint(box.xmax, box.ymin);
                ring.addPoint(box.xmax, box.ymax);
                ring.addPoint(box.xmin, box.ymax);
                ring.addPoint(box.xmin, box.ymin);
                polygon->addRing(&ring);
                filter = std::move(polygon);
            }
            std::vector<RowSlot> expected;
            for (const auto& row : rows) {
                CPLErrorReset();
                const bool intersects = row.geometry->Intersects(filter.get());
                ASSERT_LT(CPLGetLastErrorType(), CE_Failure)
                    << CPLGetLastErrorMsg();
                if (intersects) expected.push_back(row.fid);
            }
            QueryRequest request;
            request.kind = QueryKind::SpatialBbox;
            request.xmin = box.xmin; request.ymin = box.ymin;
            request.xmax = box.xmax; request.ymax = box.ymax;
            const auto result = layer->query(request);
            ASSERT_EQ(result.status, QueryStatus::Ok) << result.error;
            auto observed = result.matched_fids;
            std::sort(expected.begin(), expected.end());
            std::sort(observed.begin(), observed.end());
            EXPECT_EQ(observed, expected) << result.execution_path
                << " / " << result.fallback_reason
                << " candidates=" << result.spatial_metrics.candidate_count
                << " bbox_rejected=" << result.spatial_metrics.bbox_rejected
                << " exact=" << result.spatial_metrics.exact_tested;
            request.kind = QueryKind::SpatialWhere;
            request.where_clause = object_id->name + " >= 1";
            const auto combined = layer->query(request);
            ASSERT_EQ(combined.status, QueryStatus::Ok) << combined.error;
            auto combined_fids = combined.matched_fids;
            std::sort(combined_fids.begin(), combined_fids.end());
            EXPECT_EQ(combined_fids, expected) << combined.execution_path;
            ++windows;
            hits += expected.size();
            if (result.execution_path == kPathBboxModelSpxCandidates ||
                result.execution_path == kPathBboxModelSpxCandidatesBatched)
                ++indexed_windows;
        }
    }
    EXPECT_GT(layers, 0U);
    EXPECT_GT(windows, 0U);
    RecordProperty("QueriedSpatialLayers", std::to_string(layers));
    RecordProperty("ComparedWindows", std::to_string(windows));
    RecordProperty("SpxCandidateWindows", std::to_string(indexed_windows));
    RecordProperty("MatchedFids", std::to_string(hits));
}

TEST(RealDataReleaseContractTest,
     SuppliedGdbSpatialWindowsReturnSameFidsAsGdal) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_CURVE_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to the supplied testcurve.gdb";
    }

    // Compare spatial semantics against GDAL's geometry filtering, independent
    // of any stale or unrepresentable .spx candidate set in the sample.
    ScopedCplConfigOption disable_gdal_spatial_index(
        "OPENFILEGDB_USE_SPATIAL_INDEX", "NO");
    GDALAllRegister();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(
        static_cast<GDALDataset*>(GDALOpenEx(
            dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr)), GDALClose);
    ASSERT_NE(dataset, nullptr) << dataset_path;

    const std::vector<std::string> curve_layers = {
        "Curve_Polyline_FC", "Curve_Polygon_FC", "Curve_Polyline_Z_FC",
        "Curve_Polyline_M_FC", "Curve_Polygon_MultiHole_FC",
        "Curve_Polygon_Island_FC", "Circle_FC", "Ellipse_FC",
        "RotatedEllipse_FC", "EllipseArc_FC"};
    const std::vector<std::string> topology_layers = {
        "SelfIntersect_Polygon", "DegenerateRing_Polygon"};

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    uint64_t queried_layers = 0;
    uint64_t compared_windows = 0;
    uint64_t matched_fids = 0;
    uint64_t skipped_curve_rows = 0;
    uint64_t skipped_topology_layers = 0;
    uint64_t skipped_no_extent_layers = 0;
    uint64_t source_proof_fallback_layers = 0;
    uint64_t mismatches = 0;
    std::string first_mismatch;
    std::ostringstream mismatch_details;

    for (int layer_index = 0; layer_index < dataset->GetLayerCount();
         ++layer_index) {
        OGRLayer* layer = dataset->GetLayer(layer_index);
        ASSERT_NE(layer, nullptr);
        const std::string layer_name = layer->GetName();
        if (std::find(curve_layers.begin(), curve_layers.end(), layer_name) !=
            curve_layers.end()) {
            skipped_curve_rows += static_cast<uint64_t>(std::max<GIntBig>(
                0, layer->GetFeatureCount(TRUE)));
            continue;
        }
        if (std::find(topology_layers.begin(), topology_layers.end(), layer_name) !=
            topology_layers.end()) {
            ++skipped_topology_layers;
            continue;
        }

        SCOPED_TRACE(layer_name);
        const auto resolved = resolver.resolve(layer_name);
        ASSERT_TRUE(resolved.has_value()) << "unresolved layer";
        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open()) << "failed to open layer";
        ASSERT_NE(engine.table(), nullptr);

        const GIntBig feature_count = layer->GetFeatureCount(TRUE);
        ASSERT_GE(feature_count, 0) << "GDAL could not count layer features";
        if (feature_count == 0) continue;

        OGREnvelope extent;
        if (layer->GetExtent(&extent, TRUE) != OGRERR_NONE) {
            ++skipped_no_extent_layers;
            continue;
        }
        const double mid_x = extent.MinX + (extent.MaxX - extent.MinX) / 2.0;
        const double mid_y = extent.MinY + (extent.MaxY - extent.MinY) / 2.0;
        struct SpatialWindow {
            const char* label;
            double xmin;
            double ymin;
            double xmax;
            double ymax;
        };
        const std::vector<SpatialWindow> windows = {
            {"whole", extent.MinX, extent.MinY, extent.MaxX, extent.MaxY},
            {"lower-left", extent.MinX, extent.MinY, mid_x, mid_y},
            {"lower-right", mid_x, extent.MinY, extent.MaxX, mid_y},
            {"upper-left", extent.MinX, mid_y, mid_x, extent.MaxY},
            {"upper-right", mid_x, mid_y, extent.MaxX, extent.MaxY}};

        ++queried_layers;
        for (const auto& window : windows) {
            SCOPED_TRACE(window.label);
            QueryRequest request;
            request.kind = QueryKind::SpatialBbox;
            request.xmin = window.xmin;
            request.ymin = window.ymin;
            request.xmax = window.xmax;
            request.ymax = window.ymax;
            const QueryResult fast_result = engine.query(request);
            if (fast_result.status != QueryStatus::Ok) {
                ++mismatches;
                if (first_mismatch.empty()) {
                    first_mismatch = layer_name + "/" + window.label +
                        ": FastGDB query failed: " + fast_result.error;
                }
                continue;
            }
            if (layer_name.rfind("CRS_", 0) == 0) {
                std::printf(
                    "[crs-proof] %s/%s path=%s reason=%s hit=%s bytes=%zu "
                    "leaves=%zu entries=%zu pages=%zu\n",
                    layer_name.c_str(), window.label,
                    fast_result.execution_path.c_str(),
                    fast_result.fallback_reason.c_str(),
                    fast_result.spatial_metrics.source_directory_hit
                        ? "true" : "false",
                    fast_result.spatial_metrics.source_directory_bytes,
                    fast_result.spatial_metrics.source_directory_leaves,
                    fast_result.spatial_metrics.source_directory_entries,
                    fast_result.spatial_metrics.source_index_pages_read);
                EXPECT_EQ(fast_result.execution_path,
                          kPathBboxModelSequentialFallback);
                EXPECT_NE(fast_result.fallback_reason.find("spatial source proof failed"), std::string::npos);
                EXPECT_EQ(fast_result.spatial_metrics.source_directory_bytes, 0U);
                if (std::string(window.label) == "whole")
                    ++source_proof_fallback_layers;
            }

            std::vector<RowSlot> gdal_fids;
            layer->SetSpatialFilterRect(
                window.xmin, window.ymin, window.xmax, window.ymax);
            layer->ResetReading();
            while (OGRFeature* raw_feature = layer->GetNextFeature()) {
                std::unique_ptr<OGRFeature,
                                decltype(&OGRFeature::DestroyFeature)>
                    feature(raw_feature, OGRFeature::DestroyFeature);
                const GIntBig gdal_fid = feature->GetFID();
                if (gdal_fid <= 0) {
                    ++mismatches;
                    if (first_mismatch.empty()) {
                        first_mismatch = layer_name + "/" + window.label +
                                         ": GDAL returned a non-positive FID";
                    }
                    continue;
                }
                gdal_fids.push_back(static_cast<RowSlot>(gdal_fid - 1));
            }
            layer->SetSpatialFilter(nullptr);

            std::sort(gdal_fids.begin(), gdal_fids.end());
            std::vector<RowSlot> fast_fids = fast_result.matched_fids;
            std::sort(fast_fids.begin(), fast_fids.end());
            matched_fids += static_cast<uint64_t>(gdal_fids.size());
            ++compared_windows;
            if (fast_fids != gdal_fids) {
                ++mismatches;
                if (first_mismatch.empty()) {
                    first_mismatch = layer_name + "/" + window.label +
                        ": FastGDB and GDAL selected different FIDs (fast=" +
                        std::to_string(fast_fids.size()) + ", GDAL=" +
                        std::to_string(gdal_fids.size()) + ")";
                }
                const auto summarize = [](const std::vector<RowSlot>& fids) {
                    std::ostringstream output;
                    output << '[';
                    const size_t shown = std::min<size_t>(fids.size(), 8U);
                    for (size_t index = 0; index < shown; ++index) {
                        if (index != 0) output << ',';
                        output << fids[index];
                    }
                    if (shown < fids.size()) output << ",...";
                    output << ']';
                    return output.str();
                };
                mismatch_details << "\n" << layer_name << '/' << window.label
                    << " fast=" << summarize(fast_fids)
                    << " GDAL=" << summarize(gdal_fids)
                    << " path=" << fast_result.execution_path
                    << " candidates="
                    << fast_result.spatial_metrics.candidate_count
                    << " coverage="
                    << fast_result.spatial_metrics.estimated_coverage
                    << " exact=" << fast_result.spatial_metrics.exact_tested
                    << " rejected=" << fast_result.spatial_metrics.bbox_rejected;
            }

            if (std::string(window.label) == "whole" &&
                layer_name.rfind("CRS_", 0) == 0) {
                QueryRequest combined_request = request;
                combined_request.kind = QueryKind::SpatialWhere;
                combined_request.where_clause = "Name = 'x'";
                const QueryResult combined_result = engine.query(combined_request);
                ASSERT_EQ(combined_result.status, QueryStatus::Ok)
                    << combined_result.error;

                ASSERT_EQ(layer->SetAttributeFilter("Name = 'x'"),
                          OGRERR_NONE);
                layer->SetSpatialFilterRect(
                    window.xmin, window.ymin, window.xmax, window.ymax);
                std::vector<RowSlot> gdal_combined_fids;
                layer->ResetReading();
                while (OGRFeature* raw_feature = layer->GetNextFeature()) {
                    std::unique_ptr<OGRFeature,
                                    decltype(&OGRFeature::DestroyFeature)>
                        feature(raw_feature, OGRFeature::DestroyFeature);
                    const GIntBig gdal_fid = feature->GetFID();
                    if (gdal_fid > 0)
                        gdal_combined_fids.push_back(
                            static_cast<RowSlot>(gdal_fid - 1));
                }
                layer->SetSpatialFilter(nullptr);
                ASSERT_EQ(layer->SetAttributeFilter(nullptr), OGRERR_NONE);
                std::sort(gdal_combined_fids.begin(),
                          gdal_combined_fids.end());
                EXPECT_EQ(combined_result.matched_fids,
                          gdal_combined_fids)
                    << "combined spatial/attribute query must keep the CRS "
                       "grid-range fallback";
                EXPECT_FALSE(combined_result.combined_metrics.used_spatial_index);
            }
        }
    }

    EXPECT_GT(queried_layers, 0u);
    EXPECT_GT(compared_windows, 0u);
    EXPECT_GT(matched_fids, 0u);
    EXPECT_EQ(skipped_curve_rows, 27u)
        << "curve spatial queries have a separate geometry policy contract";
    EXPECT_EQ(skipped_topology_layers, 2u)
        << "the two intentionally malformed topology layers have explicit status tests";
    EXPECT_EQ(source_proof_fallback_layers, 3u)
        << "these CRS layers cannot establish complete source coverage and must use sequential filtering";
    EXPECT_EQ(mismatches, 0u) << first_mismatch << mismatch_details.str();
    RecordProperty("QueriedLayers", std::to_string(queried_layers));
    RecordProperty("ComparedSpatialWindows", std::to_string(compared_windows));
    RecordProperty("MatchedGdalFids", std::to_string(matched_fids));
    RecordProperty("SkippedCurveRows", std::to_string(skipped_curve_rows));
    RecordProperty("SkippedTopologyLayers",
                   std::to_string(skipped_topology_layers));
    RecordProperty("SourceProofFallbackLayers",
                   std::to_string(source_proof_fallback_layers));
    RecordProperty("SkippedNoExtentLayers",
                   std::to_string(skipped_no_extent_layers));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(RealDataReleaseContractTest, ArcGisMetadataSidecarShapesAreReadable) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_REAL_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_REAL_DATASET to acceptance_metadata.gdb";
    }

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    MetadataReader reader(resolver);
    const auto domains = reader.read_workspace_domains();
    const auto relationships = reader.read_relationship_class_definitions();
    const auto groups = reader.read_dataset_group_summaries();
    const auto audit = reader.audit_system_tables();

    EXPECT_GE(domains.size(), 4u);
    EXPECT_GE(relationships.size(), 3u);
    EXPECT_GE(groups.size(), 2u);
    ASSERT_EQ(audit.size(), 6u);
    EXPECT_TRUE(std::all_of(audit.begin(), audit.end(), [](const auto& entry) {
        return entry.status == "ok" || entry.status == "empty" || entry.status == "missing";
    }));

    const auto roads = reader.read_layer_metadata("roads");
    ASSERT_TRUE(roads.has_value());
    EXPECT_FALSE(roads->definition.empty());
    EXPECT_FALSE(reader.read_field_domain_bindings("roads").empty());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(RealDataReleaseContractTest, ArcGisFieldTypeInventoryAndSparseFidsAreExplicit) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_REAL_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_REAL_DATASET to acceptance_metadata.gdb";
    }

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    const auto resolved = resolver.resolve("all_field_types");
    ASSERT_TRUE(resolved.has_value());
    GdbTableParser types(resolved->table_path);
    ASSERT_TRUE(types.open());
    ASSERT_TRUE(types.load_tablx(resolved->tablx_path));

    const std::vector<std::pair<std::string, FieldType>> expected = {
        {"short_fld", FieldType::Int16}, {"long_fld", FieldType::Int32},
        {"float_fld", FieldType::Float32}, {"double_fld", FieldType::Float64},
        {"text_fld", FieldType::String}, {"date_fld", FieldType::DateTime},
        {"guid_fld", FieldType::UUID_1}, {"globalid_fld", FieldType::UUID_2},
        {"blob_fld", FieldType::Binary}, {"bigint_fld", FieldType::Int64},
        {"dateonly_fld", FieldType::Date}, {"timeonly_fld", FieldType::Time},
        {"timestampoffset_fld", FieldType::DateTimeWithOffset},
    };
    for (const auto& item : expected) {
        const auto field = std::find_if(
            types.fields().begin(), types.fields().end(),
            [&](const FieldDescriptor& candidate) {
                return candidate.name == item.first;
            });
        ASSERT_NE(field, types.fields().end()) << item.first;
        EXPECT_EQ(field->type, item.second) << item.first;
    }

    const auto parcels = resolver.resolve("parcels");
    ASSERT_TRUE(parcels.has_value());
    GdbTableParser sparse(parcels->table_path);
    ASSERT_TRUE(sparse.open());
    ASSERT_TRUE(sparse.load_tablx(parcels->tablx_path));
    EXPECT_GT(sparse.feature_count(), sparse.active_feature_count());
    EXPECT_EQ(sparse.active_feature_count(), 18u);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(RealDataReleaseContractTest, ArcGisExpectedValuesResolveAndPreserveNulls) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_REAL_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_REAL_DATASET to acceptance_metadata.gdb";
    }

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    const auto expected_path = std::filesystem::path(dataset_path).parent_path() /
                               "acceptance_metadata" / "field-values-expected.csv";
    std::ifstream expected(expected_path);
    ASSERT_TRUE(expected.is_open()) << expected_path;
    std::string line;
    ASSERT_TRUE(std::getline(expected, line));

    std::unordered_map<std::string, std::unique_ptr<GdbTableParser>> tables;
    size_t checked = 0;
    while (std::getline(expected, line)) {
        if (line.empty()) continue;
        const auto columns = csv_prefix(line, 6);
        ASSERT_EQ(columns.size(), 6u) << line;
        const auto resolved = resolver.resolve(columns[0]);
        ASSERT_TRUE(resolved.has_value()) << columns[0];
        auto& table = tables[columns[0]];
        if (!table) {
            table = std::make_unique<GdbTableParser>(resolved->table_path);
            ASSERT_TRUE(table->open()) << columns[0];
            ASSERT_TRUE(table->load_tablx(resolved->tablx_path)) << columns[0];
        }

        const uint32_t object_id = static_cast<uint32_t>(std::stoul(columns[1]));
        ASSERT_GT(object_id, 0u);
        ASSERT_TRUE(table->has_feature(object_id - 1))
            << columns[0] << " OBJECTID=" << object_id;
        const auto field = std::find_if(
            table->fields().begin(), table->fields().end(),
            [&](const FieldDescriptor& candidate) { return candidate.name == columns[2]; });
        ASSERT_NE(field, table->fields().end()) << columns[0] << "." << columns[2];

        FeatureRecord record;
        ASSERT_TRUE(table->read_record_by_fid(object_id - 1, record));
        ASSERT_LT(static_cast<size_t>(field - table->fields().begin()),
                  record.field_values.size());
        const bool actual_null = std::holds_alternative<std::nullptr_t>(
            record.field_values[static_cast<size_t>(field - table->fields().begin())]);
        EXPECT_EQ(actual_null, columns[4] == "Y")
            << columns[0] << " OBJECTID=" << object_id << "." << columns[2];
        if (!actual_null) {
            EXPECT_TRUE(expected_value_matches(
                columns[3], columns[5], *field,
                record.field_values[static_cast<size_t>(field - table->fields().begin())]))
                << columns[0] << " OBJECTID=" << object_id << "." << columns[2];
        }
        ++checked;
    }
    EXPECT_EQ(checked, 404u);
}

TEST(RealDataReleaseContractTest, TypedTemporalWhereMatchesNativeValues) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_REAL_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_REAL_DATASET to acceptance_metadata.gdb";
    }
    ReaderError status;
    auto reader = Reader::open(dataset_path, {}, &status);
    ASSERT_TRUE(reader.has_value()) << status.message;
    auto layer = reader->open_layer("all_field_types", &status);
    ASSERT_TRUE(layer.has_value()) << status.message;

    QueryRequest date_query;
    date_query.kind = QueryKind::WhereClause;
    date_query.where_clause =
        "date_fld >= timestamp '2025-06-15 12:30:00'";
    const auto dates = layer->query(date_query);
    ASSERT_EQ(dates.status, QueryStatus::Ok) << dates.error;
    EXPECT_EQ(dates.matched_fids, (std::vector<explorgdb::RowSlot>{3, 4}));

    QueryRequest offset_query;
    offset_query.kind = QueryKind::WhereClause;
    offset_query.where_clause =
        "timestampoffset_fld = timestamp '2025-01-01 14:00:00 +02:00'";
    const auto offsets = layer->query(offset_query);
    ASSERT_EQ(offsets.status, QueryStatus::Ok) << offsets.error;
    EXPECT_EQ(offsets.matched_fids,
              (std::vector<explorgdb::RowSlot>{1, 2, 3, 4, 5}));
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(RealDataReleaseContractTest, CurveFileGdbIsExplicitlyUnsupported) {
#if defined(FAST_GDB_CURVE_BACKEND_BUILTIN) || defined(FAST_GDB_CURVE_BACKEND_GDAL)
    GTEST_SKIP() << "curve backend linearizes curves; UNSUPPORTED_CURVE_GEOMETRY only applies to REJECT backend";
#else
    const char* dataset_path = required_dataset_from_env("FAST_GDB_CURVE_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to a real .gdb containing curve geometry";
    }
    ASSERT_TRUE(std::filesystem::is_directory(dataset_path)) << dataset_path;

    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(
        GDALOpenEx(dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr) << dataset_path;

    bool gdal_found_curve = false;
    for (int layer_index = 0;
         layer_index < dataset->GetLayerCount() && !gdal_found_curve;
         ++layer_index) {
        OGRLayer* layer = dataset->GetLayer(layer_index);
        ASSERT_NE(layer, nullptr);
        layer->ResetReading();
        while (OGRFeature* feature = layer->GetNextFeature()) {
            OGRGeometry* geometry = feature->GetGeometryRef();
            if (geometry_is_curve_like(geometry)) {
                gdal_found_curve = true;
            }
            OGRFeature::DestroyFeature(feature);
            if (gdal_found_curve) break;
        }
    }
    ASSERT_TRUE(gdal_found_curve)
        << "FAST_GDB_CURVE_DATASET does not contain a curve according to GDAL";

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    bool fast_found_explicit_unsupported = false;
    for (int layer_index = 0;
         layer_index < dataset->GetLayerCount() && !fast_found_explicit_unsupported;
         ++layer_index) {
        OGRLayer* layer = dataset->GetLayer(layer_index);
        ASSERT_NE(layer, nullptr);
        const std::string layer_name = layer->GetName();
        const auto resolved = resolver.resolve(layer_name);
        if (!resolved.has_value()) continue;

        QueryEngine engine(catalog, *resolved);
        if (!engine.open() || engine.table() == nullptr) continue;
        const auto geometry_indexes = geometry_field_indexes(*engine.table());
        if (geometry_indexes.empty()) continue;

        for (uint32_t fid = 0; fid < engine.table()->feature_count(); ++fid) {
            FeatureRecord record;
            if (!engine.read_by_fid(fid, record)) continue;
            expect_no_silent_geometry_decode(record, geometry_indexes, layer_name, fid);
            if (record_has_explicit_curve_unsupported(record, geometry_indexes)) {
                fast_found_explicit_unsupported = true;
                break;
            }
        }
    }

    GDALClose(dataset);
    EXPECT_TRUE(fast_found_explicit_unsupported)
        << "curve geometry must return UNSUPPORTED_CURVE_GEOMETRY instead of a linear WKT";
#endif
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(RealDataReleaseContractTest, CurveFileGdbUsesBuiltinWkbFirstPath) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_CURVE_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to a real .gdb containing curve geometry";
    }

    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(
        GDALOpenEx(dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr) << dataset_path;

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    bool verified_curve = false;
    for (int layer_index = 0;
         layer_index < dataset->GetLayerCount() && !verified_curve;
         ++layer_index) {
        OGRLayer* layer = dataset->GetLayer(layer_index);
        ASSERT_NE(layer, nullptr);
        const auto resolved = resolver.resolve(layer->GetName());
        ASSERT_TRUE(resolved.has_value()) << layer->GetName();

        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open()) << layer->GetName();
        ASSERT_NE(engine.table(), nullptr);
        layer->ResetReading();
        while (OGRFeature* feature = layer->GetNextFeature()) {
            const OGRGeometry* geometry = feature->GetGeometryRef();
            const GIntBig gdal_fid = feature->GetFID();
            if (!geometry_is_curve_like(geometry) || gdal_fid <= 0) {
                OGRFeature::DestroyFeature(feature);
                continue;
            }

            GeometryModel model;
            ASSERT_TRUE(engine.table()->read_geometry_model(
                static_cast<uint32_t>(gdal_fid - 1), model))
                << "layer=" << layer->GetName()
                << ", gdal_fid=" << gdal_fid
                << ", diagnostic=" << model.diagnostic;
            EXPECT_TRUE(model.source_was_curve);
            EXPECT_TRUE(model.linearized);
            EXPECT_EQ(model.backend, GeometryBackend::BuiltinCurve);

            GeometryValue value;
            ASSERT_TRUE(engine.table()->read_geometry_value(
                static_cast<uint32_t>(gdal_fid - 1), value))
                << value.diagnostic;
            EXPECT_FALSE(value.wkb.empty());
            EXPECT_TRUE(value.source_was_curve);
            EXPECT_TRUE(value.linearized);

            const uint32_t native_fid =
                static_cast<uint32_t>(gdal_fid - 1);
            const uint8_t* source_blob = nullptr;
            size_t source_blob_size = 0;
            ASSERT_TRUE(engine.peek_bbox_source(
                native_fid, source_blob, source_blob_size));

            QueryRequest preserve_request;
            preserve_request.kind = QueryKind::ReadByFid;
            preserve_request.fid = native_fid;
            {
                auto preserve_cursor = engine.open_cursor(preserve_request);
                ASSERT_EQ(preserve_cursor.query_result().status,
                          QueryStatus::Ok)
                    << preserve_cursor.error();
                QueryFeature preserved_feature;
                ASSERT_TRUE(preserve_cursor.next(preserved_feature))
                    << preserve_cursor.error();
                EXPECT_EQ(preserved_feature.geometry.status,
                          GeometryStatus::UnsupportedCurve);
                EXPECT_EQ(preserved_feature.geometry.source_payload_state,
                          GeometrySourcePayloadState::Preserved);
                EXPECT_TRUE(preserved_feature.geometry.source_was_curve);
                ASSERT_EQ(preserved_feature.geometry.source_payload.size(),
                          source_blob_size);
                EXPECT_TRUE(std::equal(
                    preserved_feature.geometry.source_payload.begin(),
                    preserved_feature.geometry.source_payload.end(),
                    source_blob));
            }

            QueryRequest linearize_request = preserve_request;
            linearize_request.geometry_read_options.curve_policy =
                CurveReadPolicy::Linearize;
            linearize_request.geometry_read_options.curve_linearization
                .max_angle_step_degrees = 2.5;
            auto linearize_cursor = engine.open_cursor(linearize_request);
            ASSERT_EQ(linearize_cursor.query_result().status,
                      QueryStatus::Ok)
                << linearize_cursor.error();
            QueryFeature linearized_feature;
            ASSERT_TRUE(linearize_cursor.next(linearized_feature))
                << linearize_cursor.error();
            EXPECT_TRUE(linearized_feature.geometry.valid());
            EXPECT_TRUE(linearized_feature.geometry.linearized);
            EXPECT_EQ(linearized_feature.geometry.source_payload_state,
                      GeometrySourcePayloadState::Preserved);
            EXPECT_EQ(linearized_feature.geometry.source_payload.size(),
                      source_blob_size);

            OGRGeometry* gdal_linear = geometry->getLinearGeometry();
            ASSERT_NE(gdal_linear, nullptr);
            OGRGeometry* fast_linear = nullptr;
            ASSERT_EQ(OGRGeometryFactory::createFromWkb(
                value.wkb.data(), nullptr, &fast_linear,
                static_cast<int>(value.wkb.size())), OGRERR_NONE);
            ASSERT_NE(fast_linear, nullptr);
            EXPECT_EQ(wkbFlatten(fast_linear->getGeometryType()),
                      wkbFlatten(gdal_linear->getGeometryType()));

            OGREnvelope gdal_envelope;
            OGREnvelope fast_envelope;
            geometry->getEnvelope(&gdal_envelope);
            fast_linear->getEnvelope(&fast_envelope);
            const double envelope_tolerance = 1e-3;
            EXPECT_NEAR(fast_envelope.MinX, gdal_envelope.MinX,
                        envelope_tolerance);
            EXPECT_NEAR(fast_envelope.MinY, gdal_envelope.MinY,
                        envelope_tolerance);
            EXPECT_NEAR(fast_envelope.MaxX, gdal_envelope.MaxX,
                        envelope_tolerance);
            EXPECT_NEAR(fast_envelope.MaxY, gdal_envelope.MaxY,
                        envelope_tolerance);
            const double fast_length = OGR_G_Length(
                reinterpret_cast<OGRGeometryH>(fast_linear));
            const double gdal_length = OGR_G_Length(
                reinterpret_cast<OGRGeometryH>(
                    const_cast<OGRGeometry*>(geometry)));
            EXPECT_NEAR(fast_length, gdal_length,
                        std::max(1.0, gdal_length * 1e-3));
            OGRGeometryFactory::destroyGeometry(fast_linear);
            OGRGeometryFactory::destroyGeometry(gdal_linear);
            verified_curve = true;
            OGRFeature::DestroyFeature(feature);
            break;
        }
    }

    GDALClose(dataset);
    EXPECT_TRUE(verified_curve);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(RealDataReleaseContractTest,
     CurvePolylineMMatchesGdalWithoutHybridFallback) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_CURVE_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to testcurve.gdb";
    }
    ASSERT_TRUE(std::filesystem::is_directory(dataset_path)) << dataset_path;

    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(
        GDALOpenEx(dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr) << dataset_path;

    OGRLayer* layer = dataset->GetLayerByName("Curve_Polyline_M_FC");
    if (layer == nullptr) {
        GDALClose(dataset);
        GTEST_SKIP() << "FAST_GDB_CURVE_DATASET has no Curve_Polyline_M_FC layer";
    }

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve("Curve_Polyline_M_FC");
    ASSERT_TRUE(resolved.has_value());

    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());
    ASSERT_NE(engine.table(), nullptr);

    const GIntBig gdal_count = layer->GetFeatureCount(TRUE);
    ASSERT_GT(gdal_count, 0);
    size_t verified_features = 0;

    layer->ResetReading();
    while (OGRFeature* feature = layer->GetNextFeature()) {
        const OGRGeometry* geometry = feature->GetGeometryRef();
        const GIntBig gdal_fid = feature->GetFID();
        ASSERT_NE(geometry, nullptr);
        ASSERT_GT(gdal_fid, 0);

        GeometryModel model;
        ASSERT_TRUE(engine.table()->read_geometry_model(
            static_cast<uint32_t>(gdal_fid - 1), model))
            << "gdal_fid=" << gdal_fid
            << ", diagnostic=" << model.diagnostic;
        EXPECT_TRUE(model.valid()) << model.diagnostic;
        EXPECT_TRUE(model.has_m);
        EXPECT_TRUE(model.source_was_curve);
        EXPECT_TRUE(model.linearized);
        EXPECT_EQ(model.backend, GeometryBackend::BuiltinCurve);
        ASSERT_FALSE(model.lines.empty());
        for (const auto& part : model.lines) {
            for (const auto& point : part)
                EXPECT_TRUE(std::isnan(point.m)) << "gdal_fid=" << gdal_fid;
        }

        GeometryValue value;
        ASSERT_TRUE(engine.table()->read_geometry_value(
            static_cast<uint32_t>(gdal_fid - 1), value))
            << "gdal_fid=" << gdal_fid
            << ", diagnostic=" << value.diagnostic;
        EXPECT_TRUE(value.valid());
        EXPECT_TRUE(value.has_m);
        EXPECT_TRUE(value.source_was_curve);
        EXPECT_TRUE(value.linearized);

        OGRGeometry* gdal_linear = geometry->getLinearGeometry();
        ASSERT_NE(gdal_linear, nullptr);
        OGRGeometry* fast_linear = nullptr;
        ASSERT_EQ(OGRGeometryFactory::createFromWkb(
            value.wkb.data(), nullptr, &fast_linear,
            static_cast<int>(value.wkb.size())), OGRERR_NONE);
        ASSERT_NE(fast_linear, nullptr);
        EXPECT_EQ(wkbFlatten(fast_linear->getGeometryType()),
                  wkbFlatten(gdal_linear->getGeometryType()));

        OGREnvelope gdal_envelope;
        OGREnvelope fast_envelope;
        geometry->getEnvelope(&gdal_envelope);
        fast_linear->getEnvelope(&fast_envelope);
        const double envelope_tolerance = 1e-3;
        EXPECT_NEAR(fast_envelope.MinX, gdal_envelope.MinX,
                    envelope_tolerance);
        EXPECT_NEAR(fast_envelope.MinY, gdal_envelope.MinY,
                    envelope_tolerance);
        EXPECT_NEAR(fast_envelope.MaxX, gdal_envelope.MaxX,
                    envelope_tolerance);
        EXPECT_NEAR(fast_envelope.MaxY, gdal_envelope.MaxY,
                    envelope_tolerance);

        const double fast_length = OGR_G_Length(
            reinterpret_cast<OGRGeometryH>(fast_linear));
        const double gdal_length = OGR_G_Length(
            reinterpret_cast<OGRGeometryH>(gdal_linear));
        EXPECT_NEAR(fast_length, gdal_length,
                    std::max(1.0, gdal_length * 1e-3));

        OGRGeometryFactory::destroyGeometry(fast_linear);
        OGRGeometryFactory::destroyGeometry(gdal_linear);
        OGRFeature::DestroyFeature(feature);
        ++verified_features;
    }

    OGREnvelope extent;
    ASSERT_EQ(layer->GetExtent(&extent, TRUE), OGRERR_NONE);
    QueryRequest request;
    request.kind = QueryKind::SpatialBbox;
    request.xmin = extent.MinX;
    request.ymin = extent.MinY;
    request.xmax = extent.MaxX;
    request.ymax = extent.MaxY;
    const QueryResult result = engine.query(request);
    EXPECT_FALSE(result.execution_path.empty());
    EXPECT_EQ(result.matched_fids.size(),
              static_cast<size_t>(gdal_count));

    GDALClose(dataset);
    EXPECT_EQ(verified_features, static_cast<size_t>(gdal_count));
}

TEST(RealDataReleaseContractTest,
     SuppliedCurvesPreserveSourceAndCompareLinearizationsToGdal) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_CURVE_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to testcurve.gdb";
    }

    GDALAllRegister();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(
        static_cast<GDALDataset*>(GDALOpenEx(
            dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr)), GDALClose);
    ASSERT_NE(dataset, nullptr) << dataset_path;

    struct CurveLayerExpectation {
        const char* name;
        GIntBig feature_count;
        bool has_z = false;
        bool has_m = false;
    };
    const std::vector<CurveLayerExpectation> expected_layers = {
        {"Curve_Polyline_FC", 8},
        {"Curve_Polyline_Z_FC", 2, true},
        {"Curve_Polyline_M_FC", 2, false, true},
        {"Curve_Polygon_FC", 2},
        {"Curve_Polygon_MultiHole_FC", 2},
        {"Curve_Polygon_Island_FC", 2},
        {"Circle_FC", 2},
        {"Ellipse_FC", 2},
        {"RotatedEllipse_FC", 3},
        {"EllipseArc_FC", 2},
    };
    for (const auto& expected : expected_layers) {
        if (dataset->GetLayerByName(expected.name) == nullptr) {
            GTEST_SKIP() << "FAST_GDB_CURVE_DATASET lacks curve layer "
                         << expected.name;
        }
    }

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    size_t valid_geometry_rows = 0;
    size_t touching_rings_rows = 0;
    size_t invalid_encoding_rows = 0;
    size_t self_intersection_rows = 0;
    for (const auto& expected : expected_layers) {
        SCOPED_TRACE(expected.name);
        OGRLayer* layer = dataset->GetLayerByName(expected.name);
        ASSERT_NE(layer, nullptr);
        ASSERT_EQ(layer->GetFeatureCount(TRUE), expected.feature_count);
        const auto resolved = resolver.resolve(expected.name);
        ASSERT_TRUE(resolved.has_value());

        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open());
        ASSERT_NE(engine.table(), nullptr);
        const auto name_field = std::find_if(
            engine.table()->fields().begin(), engine.table()->fields().end(),
            [](const FieldDescriptor& field) { return field.name == "Name"; });
        ASSERT_NE(name_field, engine.table()->fields().end());
        const size_t name_index = static_cast<size_t>(
            name_field - engine.table()->fields().begin());

        size_t compared_features = 0;
        layer->ResetReading();
        while (OGRFeature* raw_feature = layer->GetNextFeature()) {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
                feature(raw_feature, OGRFeature::DestroyFeature);
            const GIntBig gdal_fid = feature->GetFID();
            ASSERT_GT(gdal_fid, 0);
            const RowSlot row_slot = static_cast<RowSlot>(gdal_fid - 1);
            ASSERT_TRUE(engine.table()->has_feature(row_slot))
                << "gdal_fid=" << gdal_fid;

            FeatureRecord record;
            ASSERT_TRUE(engine.table()->read_record_by_fid(row_slot, record))
                << "gdal_fid=" << gdal_fid;
            ASSERT_LT(name_index, record.field_values.size());
            const auto* fast_name =
                std::get_if<std::string>(&record.field_values[name_index]);
            ASSERT_NE(fast_name, nullptr) << "gdal_fid=" << gdal_fid;
            EXPECT_EQ(*fast_name, feature->GetFieldAsString("Name"))
                << "gdal_fid=" << gdal_fid;

            const uint8_t* source_blob = nullptr;
            size_t source_blob_size = 0;
            ASSERT_TRUE(engine.peek_bbox_source(row_slot, source_blob,
                                                source_blob_size))
                << "gdal_fid=" << gdal_fid;
            QueryRequest preserve_request;
            preserve_request.kind = QueryKind::ReadByFid;
            preserve_request.fid = row_slot;
            {
                auto preserve_cursor = engine.open_cursor(preserve_request);
                ASSERT_EQ(preserve_cursor.query_result().status,
                          QueryStatus::Ok)
                    << preserve_cursor.error();
                QueryFeature preserved_feature;
                ASSERT_TRUE(preserve_cursor.next(preserved_feature))
                    << preserve_cursor.error();
                EXPECT_EQ(preserved_feature.geometry.status,
                          GeometryStatus::UnsupportedCurve)
                    << "gdal_fid=" << gdal_fid;
                EXPECT_TRUE(preserved_feature.geometry.source_was_curve)
                    << "gdal_fid=" << gdal_fid;
                EXPECT_EQ(preserved_feature.geometry.source_payload_state,
                          GeometrySourcePayloadState::Preserved)
                    << "gdal_fid=" << gdal_fid;
                ASSERT_EQ(preserved_feature.geometry.source_payload.size(),
                          source_blob_size)
                    << "gdal_fid=" << gdal_fid;
                EXPECT_TRUE(std::equal(
                    preserved_feature.geometry.source_payload.begin(),
                    preserved_feature.geometry.source_payload.end(),
                    source_blob))
                    << "gdal_fid=" << gdal_fid;
            }

            const OGRGeometry* gdal_geometry = feature->GetGeometryRef();
            ASSERT_NE(gdal_geometry, nullptr) << "gdal_fid=" << gdal_fid;
            GeometryModel model;
            const bool model_read =
                engine.table()->read_geometry_model(row_slot, model);
            if (!model_read) {
                EXPECT_FALSE(model.diagnostic.empty())
                    << "gdal_fid=" << gdal_fid;
                GeometryValue value;
                EXPECT_FALSE(engine.table()->read_geometry_value(row_slot,
                                                                  value))
                    << "gdal_fid=" << gdal_fid;
                EXPECT_TRUE(value.wkb.empty());
                EXPECT_EQ(value.status, model.status);
                switch (model.status) {
                    case GeometryStatus::TouchingRings:
                        ++touching_rings_rows;
                        break;
                    case GeometryStatus::InvalidEncoding:
                        ++invalid_encoding_rows;
                        break;
                    case GeometryStatus::SelfIntersection:
                        ++self_intersection_rows;
                        break;
                    default:
                        ADD_FAILURE() << "unexpected geometry status for "
                                      << expected.name << " FID " << gdal_fid
                                      << ": " << model.diagnostic;
                        break;
                }
                ++compared_features;
                continue;
            }
            ASSERT_TRUE(model_read) << "gdal_fid=" << gdal_fid
                                    << ", diagnostic=" << model.diagnostic;
            EXPECT_TRUE(model.valid()) << model.diagnostic;
            EXPECT_TRUE(model.source_was_curve) << "gdal_fid=" << gdal_fid;
            EXPECT_TRUE(model.linearized) << "gdal_fid=" << gdal_fid;
            EXPECT_EQ(model.has_z, expected.has_z) << "gdal_fid=" << gdal_fid;
            EXPECT_EQ(model.has_m, expected.has_m) << "gdal_fid=" << gdal_fid;
            ++valid_geometry_rows;

            EXPECT_EQ(model.has_z, gdal_geometry->Is3D())
                << "gdal_fid=" << gdal_fid;

            GeometryValue value;
            ASSERT_TRUE(engine.table()->read_geometry_value(row_slot, value))
                << "gdal_fid=" << gdal_fid
                << ", diagnostic=" << value.diagnostic;
            ASSERT_TRUE(value.valid()) << value.diagnostic;
            ASSERT_FALSE(value.wkb.empty()) << "gdal_fid=" << gdal_fid;
            EXPECT_EQ(value.has_z, expected.has_z) << "gdal_fid=" << gdal_fid;
            EXPECT_EQ(value.has_m, expected.has_m) << "gdal_fid=" << gdal_fid;

            OGRGeometry* raw_fast_geometry = nullptr;
            ASSERT_EQ(OGRGeometryFactory::createFromWkb(
                          value.wkb.data(), nullptr, &raw_fast_geometry,
                          static_cast<int>(value.wkb.size())),
                      OGRERR_NONE)
                << "gdal_fid=" << gdal_fid;
            std::unique_ptr<OGRGeometry,
                            decltype(&OGRGeometryFactory::destroyGeometry)>
                fast_geometry(raw_fast_geometry,
                              OGRGeometryFactory::destroyGeometry);
            ASSERT_NE(fast_geometry, nullptr) << "gdal_fid=" << gdal_fid;

            std::unique_ptr<OGRGeometry,
                            decltype(&OGRGeometryFactory::destroyGeometry)>
                gdal_linear(gdal_geometry->getLinearGeometry(),
                            OGRGeometryFactory::destroyGeometry);
            ASSERT_NE(gdal_linear, nullptr) << "gdal_fid=" << gdal_fid;
            EXPECT_EQ(wkbFlatten(fast_geometry->getGeometryType()),
                      wkbFlatten(gdal_linear->getGeometryType()))
                << "gdal_fid=" << gdal_fid;
            EXPECT_EQ(fast_geometry->Is3D(), gdal_linear->Is3D())
                << "gdal_fid=" << gdal_fid;
            EXPECT_EQ(fast_geometry->Is3D(), expected.has_z)
                << "gdal_fid=" << gdal_fid;
            EXPECT_EQ(fast_geometry->IsMeasured(), expected.has_m)
                << "gdal_fid=" << gdal_fid;
            if (gdal_linear->IsMeasured()) {
                EXPECT_EQ(fast_geometry->IsMeasured(),
                          gdal_linear->IsMeasured())
                    << "gdal_fid=" << gdal_fid;
            }

            OGREnvelope fast_envelope;
            OGREnvelope gdal_envelope;
            fast_geometry->getEnvelope(&fast_envelope);
            gdal_linear->getEnvelope(&gdal_envelope);
            const double coordinate_scale = std::max(
                {1.0, std::abs(gdal_envelope.MaxX - gdal_envelope.MinX),
                 std::abs(gdal_envelope.MaxY - gdal_envelope.MinY)});
            const double envelope_tolerance = coordinate_scale * 1e-3;
            EXPECT_NEAR(fast_envelope.MinX, gdal_envelope.MinX,
                        envelope_tolerance) << "gdal_fid=" << gdal_fid;
            EXPECT_NEAR(fast_envelope.MinY, gdal_envelope.MinY,
                        envelope_tolerance) << "gdal_fid=" << gdal_fid;
            EXPECT_NEAR(fast_envelope.MaxX, gdal_envelope.MaxX,
                        envelope_tolerance) << "gdal_fid=" << gdal_fid;
            EXPECT_NEAR(fast_envelope.MaxY, gdal_envelope.MaxY,
                        envelope_tolerance) << "gdal_fid=" << gdal_fid;
            ++compared_features;
        }
        EXPECT_EQ(compared_features,
                  static_cast<size_t>(expected.feature_count));
    }
    EXPECT_GT(valid_geometry_rows, 0u);
    EXPECT_GT(touching_rings_rows + invalid_encoding_rows +
                  self_intersection_rows,
              0u);
    RecordProperty("ValidCurveGeometryRows",
                   std::to_string(valid_geometry_rows));
    RecordProperty("TouchingRingsCurveGeometryRows",
                   std::to_string(touching_rings_rows));
    RecordProperty("InvalidEncodingCurveGeometryRows",
                   std::to_string(invalid_encoding_rows));
    RecordProperty("SelfIntersectionCurveGeometryRows",
                   std::to_string(self_intersection_rows));
}

TEST(RealDataReleaseContractTest, Sparse32BitFidsMatchGdalFeatureRows) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_CURVE_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to testcurve.gdb";
    }

    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(
        GDALOpenEx(dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr) << dataset_path;
    OGRLayer* layer = dataset->GetLayerByName("Point_FIDGap");
    if (layer == nullptr) {
        GDALClose(dataset);
        GTEST_SKIP() << "FAST_GDB_CURVE_DATASET has no Point_FIDGap layer";
    }

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve("Point_FIDGap");
    ASSERT_TRUE(resolved.has_value());

    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());
    ASSERT_NE(engine.table(), nullptr);
    const std::vector<RowSlot> expected_slots{0, 1, 5, 6};
    EXPECT_EQ(engine.table()->active_feature_fids(), expected_slots);
    EXPECT_EQ(engine.table()->active_feature_count(), expected_slots.size());
    EXPECT_EQ(layer->GetFeatureCount(TRUE),
              static_cast<GIntBig>(expected_slots.size()));

    const auto name_field = std::find_if(
        engine.table()->fields().begin(), engine.table()->fields().end(),
        [](const FieldDescriptor& field) { return field.name == "Name"; });
    ASSERT_NE(name_field, engine.table()->fields().end());
    const size_t name_index = static_cast<size_t>(
        name_field - engine.table()->fields().begin());

    size_t verified_features = 0;
    layer->ResetReading();
    while (OGRFeature* raw_feature = layer->GetNextFeature()) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
            feature(raw_feature, OGRFeature::DestroyFeature);
        const GIntBig gdal_fid = feature->GetFID();
        ASSERT_GT(gdal_fid, 0);
        const RowSlot row_slot = static_cast<RowSlot>(gdal_fid - 1);
        ASSERT_TRUE(engine.table()->has_feature(row_slot))
            << "gdal_fid=" << gdal_fid << ", row_slot=" << row_slot;

        FeatureRecord record;
        ASSERT_TRUE(engine.table()->read_record_by_fid(row_slot, record))
            << "gdal_fid=" << gdal_fid << ", row_slot=" << row_slot;
        ASSERT_LT(name_index, record.field_values.size());
        const auto* fast_name =
            std::get_if<std::string>(&record.field_values[name_index]);
        ASSERT_NE(fast_name, nullptr) << "gdal_fid=" << gdal_fid;
        EXPECT_EQ(*fast_name, feature->GetFieldAsString("Name"))
            << "gdal_fid=" << gdal_fid;

        GeometryValue fast_geometry;
        ASSERT_TRUE(engine.table()->read_geometry_value(row_slot, fast_geometry))
            << "gdal_fid=" << gdal_fid << ", " << fast_geometry.diagnostic;
        ASSERT_TRUE(fast_geometry.valid()) << fast_geometry.diagnostic;
        OGRGeometry* raw_decoded_geometry = nullptr;
        const OGRErr wkb_status = OGRGeometryFactory::createFromWkb(
            fast_geometry.wkb.data(), nullptr, &raw_decoded_geometry,
            static_cast<int>(fast_geometry.wkb.size()));
        std::unique_ptr<OGRGeometry, void (*)(OGRGeometry*)>
            decoded_geometry(raw_decoded_geometry,
                             OGRGeometryFactory::destroyGeometry);
        ASSERT_EQ(wkb_status, OGRERR_NONE) << "gdal_fid=" << gdal_fid;
        ASSERT_NE(decoded_geometry, nullptr);
        ASSERT_NE(feature->GetGeometryRef(), nullptr);
        EXPECT_TRUE(decoded_geometry->Equals(feature->GetGeometryRef()))
            << "gdal_fid=" << gdal_fid;
        ++verified_features;
    }

    GDALClose(dataset);
    EXPECT_EQ(verified_features, expected_slots.size());
}

TEST(RealDataReleaseContractTest, MillionPointRowsAndGeometryMatchGdal) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_CURVE_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to testcurve.gdb";
    }

    GDALAllRegister();
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(
        static_cast<GDALDataset*>(GDALOpenEx(
            dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr)), GDALClose);
    ASSERT_NE(dataset, nullptr) << dataset_path;
    OGRLayer* layer = dataset->GetLayerByName("Perf_Point_1M");
    if (layer == nullptr) {
        GTEST_SKIP() << "FAST_GDB_CURVE_DATASET has no Perf_Point_1M layer";
    }

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    const auto resolved = resolver.resolve("Perf_Point_1M");
    ASSERT_TRUE(resolved.has_value());
    QueryEngine engine(catalog, *resolved);
    ASSERT_TRUE(engine.open());
    ASSERT_NE(engine.table(), nullptr);

    constexpr uint64_t expected_count = 1'000'000;
    ASSERT_EQ(layer->GetFeatureCount(TRUE),
              static_cast<GIntBig>(expected_count));
    const auto& active_fids = engine.table()->active_feature_fids();
    ASSERT_EQ(active_fids.size(), expected_count);

    uint64_t fid_mismatches = 0;
    for (size_t index = 0; index < active_fids.size(); ++index) {
        if (active_fids[index] != static_cast<RowSlot>(index)) {
            ++fid_mismatches;
        }
    }
    EXPECT_EQ(fid_mismatches, 0u)
        << "the 1M sample should expose every dense zero-based row slot";

    uint64_t compared = 0;
    uint64_t mismatches = 0;
    std::string first_mismatch;
    layer->ResetReading();
    while (OGRFeature* raw_feature = layer->GetNextFeature()) {
        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
            feature(raw_feature, OGRFeature::DestroyFeature);
        const GIntBig gdal_fid = feature->GetFID();
        const OGRGeometry* gdal_geometry = feature->GetGeometryRef();
        const OGRPoint* gdal_point =
            gdal_geometry != nullptr &&
                    wkbFlatten(gdal_geometry->getGeometryType()) == wkbPoint
                ? gdal_geometry->toPoint()
                : nullptr;
        if (gdal_fid <= 0 ||
            gdal_fid > static_cast<GIntBig>(expected_count) ||
            gdal_point == nullptr) {
            ++mismatches;
            if (first_mismatch.empty()) {
                first_mismatch = "GDAL returned a non-positive FID or non-point geometry";
            }
            continue;
        }

        const RowSlot row_slot = static_cast<RowSlot>(gdal_fid - 1);
        GeometryValue fast_geometry;
        if (!engine.table()->read_geometry_value(row_slot, fast_geometry) ||
            !fast_geometry.valid()) {
            ++mismatches;
            if (first_mismatch.empty()) {
                first_mismatch = "FastGdb geometry read failed at FID " +
                                 std::to_string(gdal_fid) + ": " +
                                 fast_geometry.diagnostic;
            }
            continue;
        }

        OGRGeometry* raw_fast_geometry = nullptr;
        const OGRErr wkb_status = OGRGeometryFactory::createFromWkb(
            fast_geometry.wkb.data(), nullptr, &raw_fast_geometry,
            static_cast<int>(fast_geometry.wkb.size()));
        std::unique_ptr<OGRGeometry, decltype(&OGRGeometryFactory::destroyGeometry)>
            fast_geometry_owner(raw_fast_geometry,
                                OGRGeometryFactory::destroyGeometry);
        const OGRPoint* fast_point =
            fast_geometry_owner != nullptr &&
                    wkbFlatten(fast_geometry_owner->getGeometryType()) == wkbPoint
                ? fast_geometry_owner->toPoint()
                : nullptr;
        if (wkb_status != OGRERR_NONE || fast_point == nullptr ||
            fast_point->getX() != gdal_point->getX() ||
            fast_point->getY() != gdal_point->getY()) {
            ++mismatches;
            if (first_mismatch.empty()) {
                first_mismatch = "point WKB differs at FID " +
                                 std::to_string(gdal_fid);
            }
        }
        ++compared;
    }

    EXPECT_EQ(compared, expected_count);
    EXPECT_EQ(mismatches, 0u) << first_mismatch;
}

TEST(RealDataReleaseContractTest,
     RealBadPolygonTopologyIsReportedInsteadOfSilentlyRepaired) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_CURVE_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to testcurve.gdb";
    }

    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(
        GDALOpenEx(dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr) << dataset_path;
    if (dataset->GetLayerByName("SelfIntersect_Polygon") == nullptr ||
        dataset->GetLayerByName("DegenerateRing_Polygon") == nullptr) {
        GDALClose(dataset);
        GTEST_SKIP() << "FAST_GDB_CURVE_DATASET has no real bad-topology layers";
    }

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());

    const auto verify_topology = [&](const char* layer_name,
                                     GeometryStatus expected_status,
                                     bool expected_read_success) {
        OGRLayer* layer = dataset->GetLayerByName(layer_name);
        ASSERT_NE(layer, nullptr) << layer_name;
        ASSERT_EQ(layer->GetFeatureCount(TRUE), 1) << layer_name;
        const auto resolved = resolver.resolve(layer_name);
        ASSERT_TRUE(resolved.has_value()) << layer_name;

        QueryEngine engine(catalog, *resolved);
        ASSERT_TRUE(engine.open()) << layer_name;
        ASSERT_NE(engine.table(), nullptr);
        ASSERT_TRUE(engine.table()->has_feature(0)) << layer_name;

        GeometryValue value;
        EXPECT_EQ(engine.table()->read_geometry_value(0, value),
                  expected_read_success) << layer_name;
        EXPECT_EQ(value.status, expected_status)
            << layer_name << ": " << value.diagnostic;
        EXPECT_TRUE(value.wkb.empty()) << layer_name;
        EXPECT_FALSE(value.diagnostic.empty()) << layer_name;

        std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>
            feature(layer->GetFeature(1), OGRFeature::DestroyFeature);
        ASSERT_NE(feature, nullptr) << layer_name;
        const OGRGeometry* gdal_geometry = feature->GetGeometryRef();
        if (std::string(layer_name) == "SelfIntersect_Polygon") {
            ASSERT_NE(gdal_geometry, nullptr);
            EXPECT_EQ(wkbFlatten(gdal_geometry->getGeometryType()),
                      wkbMultiPolygon);
            EXPECT_EQ(gdal_geometry->toMultiPolygon()->getNumGeometries(), 2);
            EXPECT_NEAR(OGR_G_Area(reinterpret_cast<OGRGeometryH>(
                            const_cast<OGRGeometry*>(gdal_geometry))),
                        50.0, 1e-9);
        } else {
            EXPECT_TRUE(gdal_geometry == nullptr || gdal_geometry->IsEmpty())
                << "GDAL should expose the degenerate-ring record as empty geometry";
        }
    };

    verify_topology("SelfIntersect_Polygon", GeometryStatus::TouchingRings,
                    false);
    verify_topology("DegenerateRing_Polygon", GeometryStatus::Null, true);
    GDALClose(dataset);
}

TEST(RealDataReleaseContractTest,
     SpatialReferenceNamesAndEllipsoidsMatchGdalForSuppliedCrsLayers) {
    const char* dataset_path = required_dataset_from_env("FAST_GDB_CURVE_DATASET");
    if (dataset_path == nullptr) {
        GTEST_SKIP() << "Set FAST_GDB_CURVE_DATASET to testcurve.gdb";
    }

    GDALAllRegister();
    GDALDataset* dataset = static_cast<GDALDataset*>(
        GDALOpenEx(dataset_path, GDAL_OF_VECTOR | GDAL_OF_READONLY,
                   nullptr, nullptr, nullptr));
    ASSERT_NE(dataset, nullptr) << dataset_path;
    const std::vector<std::pair<const char*, const char*>> expected = {
        {"CRS_WGS84_Point_FC", "4326"},
        {"CRS_CGCS2000_Point_FC", "4490"},
        {"CRS_Xian80_Point_FC", "4610"},
    };
    for (const auto& item : expected) {
        if (dataset->GetLayerByName(item.first) == nullptr) {
            GDALClose(dataset);
            GTEST_SKIP() << "FAST_GDB_CURVE_DATASET lacks CRS layer "
                         << item.first;
        }
    }

    GdbCatalog catalog;
    ASSERT_TRUE(catalog.scan(dataset_path));
    CatalogResolver resolver(catalog);
    ASSERT_TRUE(resolver.load());
    for (const auto& item : expected) {
        OGRLayer* layer = dataset->GetLayerByName(item.first);
        ASSERT_NE(layer, nullptr);
        const OGRSpatialReference* gdal_srs = layer->GetSpatialRef();
        ASSERT_NE(gdal_srs, nullptr) << item.first;
        EXPECT_STREQ(gdal_srs->GetAuthorityName(nullptr), "EPSG")
            << item.first;
        EXPECT_STREQ(gdal_srs->GetAuthorityCode(nullptr), item.second)
            << item.first;

        const auto resolved = resolver.resolve(item.first);
        ASSERT_TRUE(resolved.has_value()) << item.first;
        GdbTableParser table(resolved->table_path);
        ASSERT_TRUE(table.open()) << item.first;
        ASSERT_TRUE(table.load_tablx(resolved->tablx_path)) << item.first;
        const auto geometry = std::find_if(
            table.fields().begin(), table.fields().end(),
            [](const FieldDescriptor& field) {
                return field.type == FieldType::Geometry;
            });
        ASSERT_NE(geometry, table.fields().end()) << item.first;
        ASSERT_FALSE(geometry->wkt.empty()) << item.first;

        OGRSpatialReference fast_srs;
        ASSERT_EQ(fast_srs.SetFromUserInput(geometry->wkt.c_str()),
                  OGRERR_NONE) << item.first << ": " << geometry->wkt;
        std::unique_ptr<OGRSpatialReference> comparable_gdal_srs(
            gdal_srs->Clone());
        ASSERT_NE(comparable_gdal_srs, nullptr) << item.first;
        EXPECT_STREQ(fast_srs.GetName(), comparable_gdal_srs->GetName())
            << item.first;
        EXPECT_NEAR(fast_srs.GetSemiMajor(), comparable_gdal_srs->GetSemiMajor(),
                    1e-6) << item.first;
        EXPECT_NEAR(fast_srs.GetInvFlattening(),
                    comparable_gdal_srs->GetInvFlattening(), 1e-9)
            << item.first;
        EXPECT_NEAR(fast_srs.GetAngularUnits(),
                    comparable_gdal_srs->GetAngularUnits(), 1e-12)
            << item.first;
    }

    GDALClose(dataset);
}
