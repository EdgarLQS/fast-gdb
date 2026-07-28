// src/edgar/explorgdb/adaptive/adaptive_gdal_backend.cpp

#include "adaptive_backend_internal.inc"

#include "cpl_error.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace explorgdb::adaptive_backend_detail {
namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

const char* attr_operator(AttrOp op) {
    switch (op) {
        case AttrOp::Eq: return "=";
        case AttrOp::Lt: return "<";
        case AttrOp::Le: return "<=";
        case AttrOp::Gt: return ">";
        case AttrOp::Ge: return ">=";
        case AttrOp::Ne: return "<>";
    }
    return "=";
}

std::string quote_identifier(const std::string& identifier) {
    std::string quoted = "\"";
    for (char character : identifier) {
        quoted.push_back(character);
        if (character == '"') quoted.push_back('"');
    }
    quoted.push_back('"');
    return quoted;
}

std::string quote_string(const std::string& value) {
    std::string quoted = "'";
    for (char character : value) {
        quoted.push_back(character);
        if (character == '\'') quoted.push_back('\'');
    }
    quoted.push_back('\'');
    return quoted;
}

bool valid_bbox(const QueryRequest& request) {
    return std::isfinite(request.xmin) && std::isfinite(request.ymin) &&
           std::isfinite(request.xmax) && std::isfinite(request.ymax) &&
           request.xmin <= request.xmax && request.ymin <= request.ymax;
}

std::optional<std::string> resolve_attribute_field(
    const AdaptiveLayerBinding& binding,
    const QueryRequest& request,
    OGRFeatureDefn* definition) {
    const auto found = binding.attribute_index_fields.find(
        ascii_lower(request.index_name));
    if (found != binding.attribute_index_fields.end()) return found->second;
    if (definition != nullptr &&
        definition->GetFieldIndex(request.index_name.c_str()) >= 0) {
        return request.index_name;
    }
    return std::nullopt;
}

struct LayerQueryConfiguration {
    bool empty = false;
    bool single_fid = false;
    GIntBig gdal_fid = OGRNullFID;
};

bool configure_layer(OGRLayer* layer,
                     const AdaptiveLayerBinding& binding,
                     const QueryRequest& request,
                     LayerQueryConfiguration& configuration,
                     std::string& error) {
    if (layer == nullptr) {
        error = "OpenFileGDB layer is null";
        return false;
    }

    layer->SetSpatialFilter(nullptr);
    CPLErrorReset();
    if (layer->SetAttributeFilter(nullptr) != OGRERR_NONE) {
        error = CPLGetLastErrorMsg();
        if (error.empty()) error = "failed to clear OGR attribute filter";
        return false;
    }

    std::string where;
    switch (request.kind) {
        case QueryKind::ReadByFid:
            configuration.single_fid = true;
            configuration.gdal_fid = static_cast<GIntBig>(request.fid) + 1;
            return true;
        case QueryKind::SequentialScan:
            return true;
        case QueryKind::SpatialBbox:
            if (!valid_bbox(request)) {
                configuration.empty = true;
                return true;
            }
            layer->SetSpatialFilterRect(
                request.xmin, request.ymin, request.xmax, request.ymax);
            return true;
        case QueryKind::AttributeDouble: {
            const auto field = resolve_attribute_field(
                binding, request, layer->GetLayerDefn());
            if (!field.has_value()) {
                error = "attribute index is not bound to an OGR field: " +
                        request.index_name;
                return false;
            }
            std::ostringstream stream;
            stream.precision(std::numeric_limits<double>::max_digits10);
            stream << quote_identifier(*field) << ' '
                   << attr_operator(request.attr_op) << ' '
                   << request.double_value;
            where = stream.str();
            break;
        }
        case QueryKind::AttributeString: {
            const auto field = resolve_attribute_field(
                binding, request, layer->GetLayerDefn());
            if (!field.has_value()) {
                error = "attribute index is not bound to an OGR field: " +
                        request.index_name;
                return false;
            }
            where = quote_identifier(*field) + " " +
                    attr_operator(request.attr_op) + " " +
                    quote_string(request.string_value);
            break;
        }
        case QueryKind::WhereClause:
            where = request.where_clause;
            break;
        case QueryKind::SpatialWhere:
            if (!valid_bbox(request)) {
                configuration.empty = true;
                return true;
            }
            layer->SetSpatialFilterRect(
                request.xmin, request.ymin, request.xmax, request.ymax);
            where = request.where_clause;
            break;
    }

    if (!where.empty()) {
        CPLErrorReset();
        if (layer->SetAttributeFilter(where.c_str()) != OGRERR_NONE) {
            error = CPLGetLastErrorMsg();
            if (error.empty()) error = "OGR SetAttributeFilter failed: " + where;
            return false;
        }
    }
    return true;
}

int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era =
        static_cast<unsigned>(year - era * 400);
    const int adjusted_month =
        static_cast<int>(month) + (month > 2 ? -3 : 9);
    const unsigned day_of_year =
        static_cast<unsigned>((153 * adjusted_month + 2) / 5) + day - 1;
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
        day_of_year;
    return static_cast<int64_t>(era) * 146097 +
           static_cast<int64_t>(day_of_era) - 719468;
}

double ole_date_value(int year,
                      int month,
                      int day,
                      int hour,
                      int minute,
                      float second,
                      FieldType type) {
    const double seconds = static_cast<double>(hour) * 3600.0 +
                           static_cast<double>(minute) * 60.0 +
                           static_cast<double>(second);
    const double fraction = seconds / 86400.0;
    if (type == FieldType::Time) return fraction;
    const double date = static_cast<double>(days_from_civil(
        year, static_cast<unsigned>(month), static_cast<unsigned>(day))) +
        25569.0;
    return type == FieldType::Date ? date : date + fraction;
}

int16_t ogr_timezone_offset_minutes(int timezone_flag) {
    if (timezone_flag >= 2 && timezone_flag <= 198) {
        return static_cast<int16_t>((timezone_flag - 100) * 15);
    }
    return 0;
}

void mark_nullable_null(FeatureRecord& record, size_t nullable_bit) {
    const size_t byte_index = nullable_bit / 8U;
    const size_t bit_index = nullable_bit % 8U;
    if (byte_index < record.nullable_flags.size()) {
        record.nullable_flags[byte_index] |=
            static_cast<uint8_t>(1U << bit_index);
    }
}

bool materialize_geometry(OGRFeature* feature,
                          GeometryValue& geometry,
                          std::string& error) {
    geometry = GeometryValue{};
    geometry.backend = GeometryBackend::Gdal;

    OGRGeometry* source = feature != nullptr ? feature->GetGeometryRef() : nullptr;
    if (source == nullptr || source->IsEmpty()) {
        geometry.status = GeometryStatus::Empty;
        geometry.diagnostic = source == nullptr
            ? "OGR feature has null geometry"
            : "OGR geometry is empty";
        return true;
    }

    const OGRwkbGeometryType type = source->getGeometryType();
    geometry.has_z = wkbHasZ(type);
    geometry.has_m = wkbHasM(type);
    geometry.geometry_type = static_cast<uint32_t>(wkbFlatten(type)) +
        (geometry.has_z && geometry.has_m ? 3000U
         : geometry.has_z ? 1000U
         : geometry.has_m ? 2000U
                          : 0U);
    geometry.source_was_curve = source->hasCurveGeometry(TRUE) != FALSE;

    const OGRSpatialReference* reference = source->getSpatialReference();
    if (reference != nullptr) {
        const char* code = reference->GetAuthorityCode(nullptr);
        if (code != nullptr) {
            geometry.srid = static_cast<int32_t>(
                std::strtol(code, nullptr, 10));
        }
    }

    geometry.wkb.resize(static_cast<size_t>(source->WkbSize()));
    if (source->exportToWkb(
            wkbNDR, geometry.wkb.data(), wkbVariantIso) != OGRERR_NONE) {
        geometry.wkb.clear();
        geometry.status = GeometryStatus::InvalidEncoding;
        geometry.diagnostic = "OGR exportToWkb(wkbVariantIso) failed";
        error = geometry.diagnostic;
        return false;
    }
    geometry.status = GeometryStatus::Valid;
    return true;
}

bool materialize_field(OGRFeature* feature,
                       int field_index,
                       const FieldDescriptor& field,
                       FieldValue& output,
                       std::string& error) {
    switch (field.type) {
        case FieldType::Int16:
        case FieldType::Int32:
            output = static_cast<int32_t>(
                feature->GetFieldAsInteger(field_index));
            return true;
        case FieldType::Int64:
            output = static_cast<int64_t>(
                feature->GetFieldAsInteger64(field_index));
            return true;
        case FieldType::Float32:
        case FieldType::Float64:
            output = feature->GetFieldAsDouble(field_index);
            return true;
        case FieldType::String:
        case FieldType::XML:
        case FieldType::UUID_1:
        case FieldType::UUID_2:
            output = std::string(feature->GetFieldAsString(field_index));
            return true;
        case FieldType::Binary: {
            int byte_count = 0;
            const GByte* bytes = feature->GetFieldAsBinary(
                field_index, &byte_count);
            output = bytes == nullptr || byte_count <= 0
                ? std::vector<uint8_t>{}
                : std::vector<uint8_t>(
                      bytes, bytes + static_cast<size_t>(byte_count));
            return true;
        }
        case FieldType::DateTime:
        case FieldType::Date:
        case FieldType::Time:
        case FieldType::DateTimeWithOffset: {
            int year = 0;
            int month = 0;
            int day = 0;
            int hour = 0;
            int minute = 0;
            int timezone = 0;
            float second = 0.0F;
            if (!feature->GetFieldAsDateTime(
                    field_index, &year, &month, &day, &hour, &minute,
                    &second, &timezone)) {
                error = "OGR date/time conversion failed: " + field.name;
                return false;
            }
            const double value = ole_date_value(
                year, month, day, hour, minute, second, field.type);
            output = field.type == FieldType::DateTimeWithOffset
                ? FieldValue(DateTimeOffsetValue{
                      value, ogr_timezone_offset_minutes(timezone)})
                : FieldValue(value);
            return true;
        }
        case FieldType::ObjectId:
        case FieldType::Geometry:
        case FieldType::Raster:
            error = "field type is handled outside materialize_field";
            return false;
    }
    error = "unknown fast-gdb field type";
    return false;
}

bool materialize_feature(OGRFeature* feature,
                         const AdaptiveLayerBinding& binding,
                         QueryFeature& output,
                         std::string& error) {
    if (feature == nullptr || feature->GetFID() <= 0) {
        error = "OGR feature has an invalid FileGDB FID";
        return false;
    }

    output = QueryFeature{};
    output.fid = static_cast<uint32_t>(feature->GetFID() - 1);
    output.record.fid = output.fid;

    size_t nullable_count = 0;
    for (const FieldDescriptor& field : binding.fields) {
        if ((field.flag & 1U) != 0) ++nullable_count;
    }
    output.record.nullable_flags.assign(
        (nullable_count + 7U) / 8U, 0U);
    output.record.field_values.reserve(binding.fields.size());

    bool geometry_materialized = false;
    size_t nullable_bit = 0;
    for (const FieldDescriptor& field : binding.fields) {
        const bool nullable = (field.flag & 1U) != 0;
        const size_t current_nullable_bit = nullable_bit;
        if (nullable) ++nullable_bit;

        if (field.type == FieldType::ObjectId) {
            output.record.field_values.push_back(
                static_cast<int32_t>(output.fid + 1U));
            continue;
        }
        if (field.type == FieldType::Geometry) {
            if (feature->GetGeometryRef() == nullptr && nullable) {
                mark_nullable_null(output.record, current_nullable_bit);
            }
            output.record.field_values.push_back(std::string{});
            if (!geometry_materialized &&
                !materialize_geometry(feature, output.geometry, error)) {
                return false;
            }
            geometry_materialized = true;
            continue;
        }
        if (field.type == FieldType::Raster) {
            if (nullable) mark_nullable_null(
                output.record, current_nullable_bit);
            output.record.field_values.push_back(nullptr);
            continue;
        }

        const int field_index = feature->GetFieldIndex(field.name.c_str());
        const bool is_null = field_index < 0 ||
            !feature->IsFieldSetAndNotNull(field_index);
        if (is_null) {
            if (!nullable && field_index < 0) {
                error = "OGR field is missing from bound schema: " + field.name;
                return false;
            }
            if (nullable) mark_nullable_null(
                output.record, current_nullable_bit);
            output.record.field_values.push_back(nullptr);
            continue;
        }

        FieldValue value;
        if (!materialize_field(feature, field_index, field, value, error)) {
            return false;
        }
        output.record.field_values.push_back(std::move(value));
    }

    return geometry_materialized ||
           materialize_geometry(feature, output.geometry, error);
}

struct OpenedGdalLayer {
    GDALDataset* dataset = nullptr;
    OGRLayer* layer = nullptr;
    LayerQueryConfiguration configuration;

    ~OpenedGdalLayer() {
        if (dataset != nullptr) GDALClose(dataset);
    }
};

struct OpenGdalLayerResult {
    std::unique_ptr<OpenedGdalLayer> opened;
    BackendFailureKind failure = BackendFailureKind::Open;
    std::string error;
};

OpenGdalLayerResult open_gdal_layer(
    const std::string& gdb_path,
    const AdaptiveLayerBinding& binding,
    const QueryRequest& request) {
    OpenGdalLayerResult result;
    GDALAllRegister();
    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        gdb_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        allowed_drivers, nullptr, nullptr));
    if (dataset == nullptr) {
        result.error = CPLGetLastErrorMsg();
        if (result.error.empty()) {
            result.error = "GDALOpenEx(OpenFileGDB readonly) failed";
        }
        return result;
    }

    result.opened = std::make_unique<OpenedGdalLayer>();
    result.opened->dataset = dataset;
    result.opened->layer = dataset->GetLayerByName(binding.layer_name.c_str());
    if (result.opened->layer == nullptr) {
        result.error = "OpenFileGDB layer not found: " + binding.layer_name;
        return result;
    }
    if (!configure_layer(result.opened->layer, binding, request,
                         result.opened->configuration, result.error)) {
        result.failure = BackendFailureKind::Read;
        result.opened.reset();
        return result;
    }
    result.failure = BackendFailureKind::None;
    return result;
}

BackendReadResult failed_open(const OpenGdalLayerResult& result) {
    return result.failure == BackendFailureKind::Read
        ? BackendReadResult::read_failure(result.error)
        : BackendReadResult::open_failure(result.error);
}

struct GdalCursorState {
    std::unique_ptr<OpenedGdalLayer> opened;
    AdaptiveLayerBinding binding;
    bool single_fid_emitted = false;

    bool next(QueryFeature& output, std::string& error) {
        if (!opened || opened->configuration.empty) return false;

        OGRFeature* feature = nullptr;
        CPLErrorReset();
        if (opened->configuration.single_fid) {
            if (single_fid_emitted) return false;
            single_fid_emitted = true;
            feature = opened->layer->GetFeature(
                opened->configuration.gdal_fid);
        } else {
            feature = opened->layer->GetNextFeature();
        }

        if (feature == nullptr) {
            if (CPLGetLastErrorType() >= CE_Failure) {
                error = CPLGetLastErrorMsg();
                if (error.empty()) error = "OGR feature read failed";
            }
            return false;
        }

        const bool ok = materialize_feature(feature, binding, output, error);
        OGRFeature::DestroyFeature(feature);
        return ok;
    }

    void close() { opened.reset(); }
};

}  // namespace

BackendReadResult read_gdal(const std::string& gdb_path,
                            const AdaptiveLayerBinding& binding,
                            const QueryRequest& request) {
    OpenGdalLayerResult opened = open_gdal_layer(
        gdb_path, binding, request);
    if (!opened.opened) return failed_open(opened);

    QueryResult result;
    result.execution_path = "gdal:openfilegdb:fresh";
    if (opened.opened->configuration.empty) {
        return BackendReadResult::success(std::move(result));
    }

    if (opened.opened->configuration.single_fid) {
        CPLErrorReset();
        OGRFeature* feature = opened.opened->layer->GetFeature(
            opened.opened->configuration.gdal_fid);
        if (feature == nullptr) {
            if (CPLGetLastErrorType() >= CE_Failure) {
                std::string error = CPLGetLastErrorMsg();
                if (error.empty()) error = "OGR GetFeature failed";
                return BackendReadResult::read_failure(std::move(error));
            }
            return BackendReadResult::success(std::move(result));
        }

        QueryFeature materialized;
        std::string error;
        if (!materialize_feature(feature, binding, materialized, error)) {
            OGRFeature::DestroyFeature(feature);
            return BackendReadResult::read_failure(std::move(error));
        }
        OGRFeature::DestroyFeature(feature);
        result.matched_fids.push_back(materialized.fid);
        result.record = std::move(materialized.record);
        return BackendReadResult::success(std::move(result));
    }

    opened.opened->layer->ResetReading();
    while (true) {
        CPLErrorReset();
        OGRFeature* feature = opened.opened->layer->GetNextFeature();
        if (feature == nullptr) {
            if (CPLGetLastErrorType() >= CE_Failure) {
                std::string error = CPLGetLastErrorMsg();
                if (error.empty()) error = "OGR GetNextFeature failed";
                return BackendReadResult::read_failure(std::move(error));
            }
            break;
        }
        if (feature->GetFID() <= 0) {
            OGRFeature::DestroyFeature(feature);
            return BackendReadResult::read_failure(
                "OGR feature has an invalid FileGDB FID");
        }
        result.matched_fids.push_back(
            static_cast<uint32_t>(feature->GetFID() - 1));
        OGRFeature::DestroyFeature(feature);
    }

    if (request.kind != QueryKind::SequentialScan) {
        std::sort(result.matched_fids.begin(), result.matched_fids.end());
        result.matched_fids.erase(
            std::unique(result.matched_fids.begin(),
                        result.matched_fids.end()),
            result.matched_fids.end());
    }
    return BackendReadResult::success(std::move(result));
}

BackendCursor open_gdal_cursor(const std::string& gdb_path,
                               const AdaptiveLayerBinding& binding,
                               const QueryRequest& request) {
    OpenGdalLayerResult opened = open_gdal_layer(
        gdb_path, binding, request);
    if (!opened.opened) throw std::runtime_error(opened.error);

    auto state = std::make_shared<GdalCursorState>();
    state->opened = std::move(opened.opened);
    state->binding = binding;

    BackendCursor cursor;
    cursor.next = [state](QueryFeature& feature, std::string& next_error) {
        return state->next(feature, next_error);
    };
    cursor.close = [state] { state->close(); };
    return cursor;
}

}  // namespace explorgdb::adaptive_backend_detail
