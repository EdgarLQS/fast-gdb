// src/edgar/explorgdb/adaptive/adaptive_backends.cpp

#include "adaptive_backends.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"

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

namespace explorgdb {
namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::string attr_operator(AttrOp op) {
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
    if (found != binding.attribute_index_fields.end()) {
        return found->second;
    }
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
            configuration.gdal_fid =
                static_cast<GIntBig>(request.fid) + 1;
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
            if (error.empty()) {
                error = "OGR SetAttributeFilter failed: " + where;
            }
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
    const unsigned day_of_year =
        (153 * (month + (month > 2 ? static_cast<unsigned>(-3) : 9)) + 2) /
            5 +
        day - 1;
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

    OGRGeometry* source = feature != nullptr
        ? feature->GetGeometryRef()
        : nullptr;
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
    const uint32_t base_type =
        static_cast<uint32_t>(wkbFlatten(type));
    geometry.geometry_type = base_type +
        (geometry.has_z && geometry.has_m ? 3000U
         : geometry.has_z ? 1000U
         : geometry.has_m ? 2000U
                          : 0U);
    geometry.source_was_curve = source->hasCurveGeometry(TRUE) != FALSE;
    geometry.linearized = false;

    const OGRSpatialReference* reference = source->getSpatialReference();
    if (reference != nullptr) {
        const char* code = reference->GetAuthorityCode(nullptr);
        if (code != nullptr) {
            geometry.srid = static_cast<int32_t>(std::strtol(code, nullptr, 10));
        }
    }

    const size_t wkb_size = static_cast<size_t>(source->WkbSize());
    geometry.wkb.resize(wkb_size);
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
            const bool is_null = feature->GetGeometryRef() == nullptr;
            if (is_null && nullable) {
                mark_nullable_null(output.record, current_nullable_bit);
            }
            output.record.field_values.push_back(std::string{});
            if (!geometry_materialized) {
                if (!materialize_geometry(feature, output.geometry, error)) {
                    return false;
                }
                geometry_materialized = true;
            }
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

        switch (field.type) {
            case FieldType::Int16:
            case FieldType::Int32:
                output.record.field_values.push_back(
                    static_cast<int32_t>(
                        feature->GetFieldAsInteger(field_index)));
                break;
            case FieldType::Int64:
                output.record.field_values.push_back(
                    static_cast<int64_t>(
                        feature->GetFieldAsInteger64(field_index)));
                break;
            case FieldType::Float32:
            case FieldType::Float64:
                output.record.field_values.push_back(
                    feature->GetFieldAsDouble(field_index));
                break;
            case FieldType::String:
            case FieldType::XML:
            case FieldType::UUID_1:
            case FieldType::UUID_2:
                output.record.field_values.push_back(
                    std::string(feature->GetFieldAsString(field_index)));
                break;
            case FieldType::Binary: {
                int byte_count = 0;
                const GByte* bytes = feature->GetFieldAsBinary(
                    field_index, &byte_count);
                if (bytes == nullptr || byte_count <= 0) {
                    output.record.field_values.push_back(
                        std::vector<uint8_t>{});
                } else {
                    output.record.field_values.push_back(
                        std::vector<uint8_t>(
                            bytes, bytes + static_cast<size_t>(byte_count)));
                }
                break;
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
                if (field.type == FieldType::DateTimeWithOffset) {
                    output.record.field_values.push_back(
                        DateTimeOffsetValue{
                            value, ogr_timezone_offset_minutes(timezone)});
                } else {
                    output.record.field_values.push_back(value);
                }
                break;
            }
            case FieldType::Geometry:
            case FieldType::ObjectId:
            case FieldType::Raster:
                break;
        }
    }

    if (!geometry_materialized) {
        if (!materialize_geometry(feature, output.geometry, error)) {
            return false;
        }
    }
    return true;
}

struct OpenedGdalLayer {
    GDALDataset* dataset = nullptr;
    OGRLayer* layer = nullptr;
    LayerQueryConfiguration configuration;

    ~OpenedGdalLayer() {
        if (dataset != nullptr) GDALClose(dataset);
    }
};

std::unique_ptr<OpenedGdalLayer> open_gdal_layer(
    const std::string& gdb_path,
    const AdaptiveLayerBinding& binding,
    const QueryRequest& request,
    std::string& error) {
    GDALAllRegister();
    const char* allowed_drivers[] = {"OpenFileGDB", nullptr};
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        gdb_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        allowed_drivers, nullptr, nullptr));
    if (dataset == nullptr) {
        error = CPLGetLastErrorMsg();
        if (error.empty()) error = "GDALOpenEx(OpenFileGDB readonly) failed";
        return nullptr;
    }

    auto opened = std::make_unique<OpenedGdalLayer>();
    opened->dataset = dataset;
    opened->layer = dataset->GetLayerByName(binding.layer_name.c_str());
    if (opened->layer == nullptr) {
        error = "OpenFileGDB layer not found: " + binding.layer_name;
        return nullptr;
    }
    if (!configure_layer(
            opened->layer, binding, request, opened->configuration, error)) {
        return nullptr;
    }
    return opened;
}

struct FastCursorState {
    GdbCatalog catalog;
    ResolvedTable resolved;
    std::unique_ptr<QueryEngine> engine;
    std::optional<FeatureCursor> cursor;
    bool closed = false;

    bool open(const std::string& gdb_path,
              const std::string& layer_name,
              const QueryRequest& request,
              std::string& error) {
        if (!catalog.scan(gdb_path)) {
            error = "GdbCatalog::scan failed";
            return false;
        }
        CatalogResolver resolver(catalog);
        if (!resolver.load()) {
            error = "CatalogResolver::load failed";
            return false;
        }
        const auto table = resolver.resolve(layer_name);
        if (!table.has_value()) {
            error = "fast-gdb layer not found: " + layer_name;
            return false;
        }
        resolved = *table;
        engine = std::make_unique<QueryEngine>(catalog, resolved);
        if (!engine->open()) {
            error = "QueryEngine::open failed";
            return false;
        }
        cursor.emplace(engine->open_cursor(request));
        if (!cursor->error().empty()) {
            error = cursor->error();
            cursor.reset();
            engine.reset();
            return false;
        }
        return true;
    }

    bool next(QueryFeature& feature, std::string& error) {
        if (closed || !cursor.has_value()) return false;
        if (cursor->next(feature)) return true;
        if (!cursor->error().empty()) error = cursor->error();
        return false;
    }

    void close() {
        if (closed) return;
        cursor.reset();
        engine.reset();
        closed = true;
    }
};

struct GdalCursorState {
    std::unique_ptr<OpenedGdalLayer> opened;
    AdaptiveLayerBinding binding;
    bool single_fid_emitted = false;
    bool closed = false;

    bool next(QueryFeature& output, std::string& error) {
        if (closed || !opened || opened->configuration.empty) return false;

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

    void close() {
        if (closed) return;
        opened.reset();
        closed = true;
    }
};

}  // namespace

AdaptiveLayerBindingResult load_adaptive_layer_binding(
    const InProcessGdbCoordinator& coordinator,
    const std::string& gdb_path,
    const std::string& layer_name) {
    AdaptiveLayerBindingResult result;
    auto lease = coordinator.try_acquire_fast_reader(gdb_path);
    if (!lease.valid()) {
        result.error = "source is not Stable; layer binding was not read";
        return result;
    }

    GdbCatalog catalog;
    if (!catalog.scan(gdb_path)) {
        result.error = "GdbCatalog::scan failed";
        return result;
    }
    CatalogResolver resolver(catalog);
    if (!resolver.load()) {
        result.error = "CatalogResolver::load failed";
        return result;
    }
    const auto resolved = resolver.resolve(layer_name);
    if (!resolved.has_value()) {
        result.error = "layer not found: " + layer_name;
        return result;
    }

    QueryEngine engine(catalog, *resolved);
    if (!engine.open() || engine.table() == nullptr) {
        result.error = "QueryEngine::open failed while loading binding";
        return result;
    }

    result.binding.layer_name = layer_name;
    result.binding.fields = engine.table()->fields();

    std::vector<IndexEntry> indexes;
    if (catalog.read_index_metadata(resolved->id, indexes)) {
        for (const IndexEntry& index : indexes) {
            if (!index.name.empty() && !index.column_name.empty()) {
                result.binding.attribute_index_fields.emplace(
                    ascii_lower(index.name), index.column_name);
            }
        }
    }
    result.ok = true;
    return result;
}

FastGdbReadBackend::FastGdbReadBackend(std::string gdb_path,
                                       std::string layer_name)
    : gdb_path_(std::move(gdb_path)),
      layer_name_(std::move(layer_name)) {}

BackendReadResult FastGdbReadBackend::read(
    const QueryRequest& request) const {
    GdbCatalog catalog;
    if (!catalog.scan(gdb_path_)) {
        return BackendReadResult::open_failure("GdbCatalog::scan failed");
    }
    CatalogResolver resolver(catalog);
    if (!resolver.load()) {
        return BackendReadResult::open_failure(
            "CatalogResolver::load failed");
    }
    const auto resolved = resolver.resolve(layer_name_);
    if (!resolved.has_value()) {
        return BackendReadResult::open_failure(
            "fast-gdb layer not found: " + layer_name_);
    }
    QueryEngine engine(catalog, *resolved);
    if (!engine.open()) {
        return BackendReadResult::open_failure("QueryEngine::open failed");
    }
    return BackendReadResult::success(engine.query(request));
}

BackendCursor FastGdbReadBackend::open_cursor(
    const QueryRequest& request) const {
    auto state = std::make_shared<FastCursorState>();
    std::string error;
    if (!state->open(gdb_path_, layer_name_, request, error)) {
        throw std::runtime_error(error);
    }

    BackendCursor cursor;
    cursor.next = [state](QueryFeature& feature, std::string& next_error) {
        return state->next(feature, next_error);
    };
    cursor.close = [state] { state->close(); };
    return cursor;
}

GdalOpenFileGdbReadBackend::GdalOpenFileGdbReadBackend(
    std::string gdb_path,
    AdaptiveLayerBinding binding)
    : gdb_path_(std::move(gdb_path)),
      binding_(std::move(binding)) {}

BackendReadResult GdalOpenFileGdbReadBackend::read(
    const QueryRequest& request) const {
    std::string error;
    auto opened = open_gdal_layer(gdb_path_, binding_, request, error);
    if (!opened) return BackendReadResult::open_failure(std::move(error));

    QueryResult result;
    result.execution_path = "gdal:openfilegdb:fresh";
    if (opened->configuration.empty) {
        return BackendReadResult::success(std::move(result));
    }

    if (opened->configuration.single_fid) {
        CPLErrorReset();
        OGRFeature* feature = opened->layer->GetFeature(
            opened->configuration.gdal_fid);
        if (feature == nullptr) {
            if (CPLGetLastErrorType() >= CE_Failure) {
                error = CPLGetLastErrorMsg();
                if (error.empty()) error = "OGR GetFeature failed";
                return BackendReadResult::read_failure(std::move(error));
            }
            return BackendReadResult::success(std::move(result));
        }

        QueryFeature materialized;
        if (!materialize_feature(feature, binding_, materialized, error)) {
            OGRFeature::DestroyFeature(feature);
            return BackendReadResult::read_failure(std::move(error));
        }
        OGRFeature::DestroyFeature(feature);
        result.matched_fids.push_back(materialized.fid);
        result.record = std::move(materialized.record);
        return BackendReadResult::success(std::move(result));
    }

    opened->layer->ResetReading();
    while (true) {
        CPLErrorReset();
        OGRFeature* feature = opened->layer->GetNextFeature();
        if (feature == nullptr) {
            if (CPLGetLastErrorType() >= CE_Failure) {
                error = CPLGetLastErrorMsg();
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
    return BackendReadResult::success(std::move(result));
}

BackendCursor GdalOpenFileGdbReadBackend::open_cursor(
    const QueryRequest& request) const {
    std::string error;
    auto opened = open_gdal_layer(gdb_path_, binding_, request, error);
    if (!opened) throw std::runtime_error(error);

    auto state = std::make_shared<GdalCursorState>();
    state->opened = std::move(opened);
    state->binding = binding_;

    BackendCursor cursor;
    cursor.next = [state](QueryFeature& feature, std::string& next_error) {
        return state->next(feature, next_error);
    };
    cursor.close = [state] { state->close(); };
    return cursor;
}

AdaptiveReadSession make_adaptive_read_session(
    InProcessGdbCoordinator coordinator,
    std::string gdb_path,
    AdaptiveLayerBinding binding) {
    const std::string layer_name = binding.layer_name;
    auto fast = std::make_shared<FastGdbReadBackend>(
        gdb_path, layer_name);
    auto gdal = std::make_shared<GdalOpenFileGdbReadBackend>(
        gdb_path, std::move(binding));

    return AdaptiveReadSession(
        std::move(coordinator), std::move(gdb_path),
        [fast](const QueryRequest& request) {
            return fast->read(request);
        },
        [gdal](const QueryRequest& request) {
            return gdal->read(request);
        },
        [fast](const QueryRequest& request) {
            return fast->open_cursor(request);
        },
        [gdal](const QueryRequest& request) {
            return gdal->open_cursor(request);
        });
}

}  // namespace explorgdb
