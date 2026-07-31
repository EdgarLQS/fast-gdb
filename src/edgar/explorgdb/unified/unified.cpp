#include "unified.h"

#include "ole_date.h"
#include "reader.h"
#include "adaptive_reader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
#include <cpl_error.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#endif

namespace fast_gdb {
namespace unified {
namespace detail {

struct DatasetState {
    Source source;
    OpenOptions options;
    BackendReport report;
    ConsistencyReport consistency;
    std::vector<std::string> layer_names;
    std::vector<GroupInfo> groups;
    std::unordered_map<std::string, std::vector<LayerInfo>> layers_by_group;
    std::unordered_map<std::string, std::vector<std::string>> paths_by_name;
};

struct LayerState {
    std::shared_ptr<DatasetState> dataset;
    std::string name;
    std::string path;
    LayerSchema schema;
    std::vector<std::size_t> source_field_indices;
    BackendReport report;
};

struct GroupState {
    std::shared_ptr<DatasetState> dataset;
    std::string path;
    std::vector<GroupInfo> groups;
    std::vector<LayerInfo> layers;
};

struct CursorState {
    // Declared first so reverse member destruction releases the lease only
    // after Cursor, Layer and Reader have dropped every mmap/fd.
    std::optional<explorgdb::FastReaderLease> fast_lease;
    std::optional<explorgdb::Reader> reader;
    std::optional<explorgdb::Layer> layer;
    explorgdb::FeatureCursor cursor;
    std::shared_ptr<LayerState> layer_state;
    BackendReport report;
    ConsistencyReport consistency;
    QueryReport query_report;
    std::optional<Feature> prefetched;
    bool closed = false;
    std::function<bool()> cancel_requested;
    std::optional<std::chrono::steady_clock::time_point> deadline;
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
    GDALDataset* gdal_dataset = nullptr;
    OGRLayer* gdal_layer = nullptr;
    std::vector<GIntBig> ordered_fids;
    std::size_t ordered_index = 0;
    std::uint64_t remaining_skip = 0;
    std::uint64_t remaining_limit = 0;
    bool limit_enabled = false;
#endif

    ~CursorState() {
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
        if (gdal_dataset != nullptr) GDALClose(gdal_dataset);
#endif
    }
};

}  // namespace detail
namespace {

Error make_error(ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

Result<explorgdb::FastReaderLease> acquire_fast_reader_lease(
        const std::string& source) {
    explorgdb::InProcessGdbCoordinator coordinator;
    auto lease = coordinator.try_acquire_fast_reader(source);
    if (!lease.valid()) {
        return make_error(ErrorCode::SourceBusy,
                          "coordinated Writer is pending or active");
    }
    return std::move(lease);
}

Error validate_public_query(const Query& query,
                            const LayerSchema& schema) {
    if (query.attribute_filter.size() > 64U * 1024U) {
        return make_error(ErrorCode::InvalidRequest,
                          "attribute filter exceeds 64 KiB");
    }
    for (const auto& name : query.projected_fields) {
        const auto found = std::find_if(
            schema.fields.begin(), schema.fields.end(),
            [&name](const FieldDefinition& field) {
                return field.name == name;
            });
        if (found == schema.fields.end()) {
            return make_error(ErrorCode::InvalidRequest,
                              "projected field does not exist: " + name);
        }
    }
    if (query.spatial_filter) {
        const auto& box = *query.spatial_filter;
        if (!std::isfinite(box.xmin) || !std::isfinite(box.ymin) ||
            !std::isfinite(box.xmax) || !std::isfinite(box.ymax) ||
            box.xmin > box.xmax || box.ymin > box.ymax) {
            return make_error(ErrorCode::InvalidRequest,
                              "spatial filter envelope is invalid");
        }
    }
    if (query.order == ResultOrder::FidAscending &&
        query.max_ordered_fid_bytes < sizeof(Fid)) {
        return make_error(ErrorCode::ResultLimitExceeded,
                          "ordered FID budget cannot hold one FID");
    }
    return {};
}

bool is_system_layer(std::string_view name) {
    return name.size() >= 4 &&
           (name[0] == 'G' || name[0] == 'g') &&
           (name[1] == 'D' || name[1] == 'd') &&
           (name[2] == 'B' || name[2] == 'b') &&
           name[3] == '_';
}

std::string canonical_path(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.empty() || value.front() != '/') value.insert(value.begin(), '/');
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

std::string parent_path(std::string_view path) {
    const auto separator = path.find_last_of('/');
    if (separator == std::string_view::npos || separator == 0) return "/";
    return std::string(path.substr(0, separator));
}

std::string path_name(std::string_view path) {
    const auto separator = path.find_last_of('/');
    return std::string(path.substr(separator + 1));
}

bool ascii_equal(std::string_view lhs, std::string_view rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                      [](unsigned char left, unsigned char right) {
                          return std::tolower(left) == std::tolower(right);
                      });
}

bool valid_canonical_path(std::string_view path) {
    if (path.empty() || path.front() != '/' ||
        path.find('\0') != std::string_view::npos ||
        path.find('\\') != std::string_view::npos ||
        path.find("//") != std::string_view::npos) {
        return false;
    }
    std::size_t offset = 1;
    while (offset <= path.size()) {
        const auto next = path.find('/', offset);
        const auto segment = path.substr(
            offset, next == std::string_view::npos
                        ? path.size() - offset : next - offset);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (next == std::string_view::npos) break;
        offset = next + 1;
    }
    return true;
}

void index_group_layer(detail::DatasetState& state,
                       std::string name,
                       std::string path) {
    path = canonical_path(std::move(path));
    state.layers_by_group[parent_path(path)].push_back({name, path});
    state.paths_by_name[std::move(name)].push_back(std::move(path));
}

void index_group(detail::DatasetState& state, std::string group_path) {
    group_path = canonical_path(std::move(group_path));
    if (group_path == "/") return;
    std::string current;
    std::size_t offset = 1;
    while (offset < group_path.size()) {
        const auto next = group_path.find('/', offset);
        const auto length = next == std::string::npos
            ? group_path.size() : next;
        current = group_path.substr(0, length);
        const auto found = std::find_if(
            state.groups.begin(), state.groups.end(),
            [&current](const GroupInfo& group) {
                return group.path == current;
            });
        if (found == state.groups.end()) {
            state.groups.push_back({path_name(current), current});
        }
        if (next == std::string::npos) break;
        offset = next + 1;
    }
}

void populate_fast_groups(detail::DatasetState& state,
                          explorgdb::Reader& reader) {
    std::set<std::string> grouped_names;
    for (const auto& name : state.layer_names) {
        explorgdb::ReaderError status;
        auto layer = reader.open_layer(name, &status);
        if (!layer) continue;
        const auto metadata = layer->read_metadata();
        if (!metadata.ok()) continue;
        for (const auto& group : metadata.snapshot.dataset_groups) {
            const auto group_path = canonical_path(group.group_path);
            index_group(state, group_path);
            for (std::size_t i = 0; i < group.member_names.size(); ++i) {
                if (std::find(state.layer_names.begin(),
                              state.layer_names.end(),
                              group.member_names[i]) ==
                    state.layer_names.end()) {
                    continue;
                }
                const auto path = i < group.member_paths.size()
                    ? group.member_paths[i]
                    : group_path + "/" + group.member_names[i];
                index_group_layer(
                    state, group.member_names[i], path);
                grouped_names.insert(group.member_names[i]);
            }
        }
        break;
    }
    for (const auto& name : state.layer_names) {
        if (grouped_names.find(name) == grouped_names.end()) {
            index_group_layer(state, name, "/" + name);
        }
    }
}

Error reader_error(const explorgdb::ReaderError& error) {
    switch (error.status) {
        case explorgdb::ReaderStatus::SourceNotFound:
            return make_error(ErrorCode::SourceNotFound, error.message);
        case explorgdb::ReaderStatus::LayerNotFound:
            return make_error(ErrorCode::LayerNotFound, error.message);
        case explorgdb::ReaderStatus::SourceChanged:
            return make_error(ErrorCode::SourceChanged, error.message);
        case explorgdb::ReaderStatus::FidRangeUnsupported:
            return make_error(ErrorCode::Unsupported, error.message);
        default:
            return make_error(ErrorCode::OpenFailed, error.message);
    }
}

FieldType public_field_type(explorgdb::FieldType type) {
    switch (type) {
        case explorgdb::FieldType::Int16:
        case explorgdb::FieldType::Int32:
        case explorgdb::FieldType::ObjectId:
            return FieldType::Integer;
        case explorgdb::FieldType::Int64:
            return FieldType::Integer64;
        case explorgdb::FieldType::Float32:
        case explorgdb::FieldType::Float64:
            return FieldType::Real;
        case explorgdb::FieldType::Binary:
            return FieldType::Binary;
        case explorgdb::FieldType::Date:
            return FieldType::Date;
        case explorgdb::FieldType::Time:
            return FieldType::Time;
        case explorgdb::FieldType::DateTime:
        case explorgdb::FieldType::DateTimeWithOffset:
            return FieldType::DateTime;
        default:
            return FieldType::String;
    }
}

std::string trim_xml_text(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::optional<std::string> xml_tag_text(
        std::string_view xml, std::string_view tag) {
    const std::string prefix = "<" + std::string(tag);
    std::size_t offset = 0;
    while ((offset = xml.find(prefix, offset)) != std::string_view::npos) {
        const auto boundary = offset + prefix.size();
        if (boundary < xml.size() &&
            xml[boundary] != '>' &&
            !std::isspace(static_cast<unsigned char>(xml[boundary]))) {
            offset = boundary;
            continue;
        }
        const auto value_begin = xml.find('>', boundary);
        if (value_begin == std::string_view::npos) return std::nullopt;
        const std::string close = "</" + std::string(tag) + ">";
        const auto value_end = xml.find(close, value_begin + 1);
        if (value_end == std::string_view::npos) return std::nullopt;
        return trim_xml_text(std::string(
            xml.substr(value_begin + 1, value_end - value_begin - 1)));
    }
    return std::nullopt;
}

std::vector<std::string_view> xml_blocks(
        std::string_view xml, std::string_view tag) {
    std::vector<std::string_view> blocks;
    const std::string prefix = "<" + std::string(tag);
    const std::string close = "</" + std::string(tag) + ">";
    std::size_t offset = 0;
    while ((offset = xml.find(prefix, offset)) != std::string_view::npos) {
        const auto boundary = offset + prefix.size();
        if (boundary < xml.size() &&
            xml[boundary] != '>' &&
            !std::isspace(static_cast<unsigned char>(xml[boundary]))) {
            offset = boundary;
            continue;
        }
        const auto end = xml.find(close, boundary);
        if (end == std::string_view::npos) break;
        const auto length = end + close.size() - offset;
        blocks.push_back(xml.substr(offset, length));
        offset += length;
    }
    return blocks;
}

struct LayerDefinitionDefaults {
    std::vector<std::pair<std::string, std::string>> field_defaults;
    std::string area_field;
    std::string length_field;
    std::optional<std::uint32_t> geometry_type;
};

LayerDefinitionDefaults parse_layer_definition_defaults(
        const std::optional<explorgdb::LayerMetadata>& metadata) {
    LayerDefinitionDefaults result;
    if (!metadata) return result;
    const auto& xml = metadata->definition;
    result.area_field = xml_tag_text(xml, "AreaFieldName").value_or("");
    result.length_field = xml_tag_text(xml, "LengthFieldName").value_or("");
    const auto shape_type = xml_tag_text(xml, "ShapeType");
    std::uint32_t base_type = 0;
    if (shape_type) {
        if (ascii_equal(*shape_type, "esriGeometryPoint")) base_type = 1;
        if (ascii_equal(*shape_type, "esriGeometryMultipoint")) base_type = 4;
        if (ascii_equal(*shape_type, "esriGeometryPolyline")) base_type = 5;
        if (ascii_equal(*shape_type, "esriGeometryPolygon")) base_type = 6;
    }
    if (base_type != 0) {
        const bool has_z =
            ascii_equal(xml_tag_text(xml, "HasZ").value_or("false"), "true");
        const bool has_m =
            ascii_equal(xml_tag_text(xml, "HasM").value_or("false"), "true");
        result.geometry_type =
            base_type + (has_z && has_m
                ? 3000U : (has_z ? 1000U : (has_m ? 2000U : 0U)));
    }
    for (const auto block : xml_blocks(xml, "GPFieldInfoEx")) {
        const auto name = xml_tag_text(block, "Name");
        auto value = xml_tag_text(block, "DefaultValueNumeric");
        if (!value) value = xml_tag_text(block, "DefaultValue");
        if (!value) value = xml_tag_text(block, "DefaultValueInteger");
        if (name && value) {
            result.field_defaults.emplace_back(*name, *value);
        }
    }
    return result;
}

std::optional<std::string> numeric_xml_default(
        const LayerDefinitionDefaults& defaults,
        std::string_view field_name) {
    const auto found = std::find_if(
        defaults.field_defaults.begin(), defaults.field_defaults.end(),
        [field_name](const auto& value) {
            return ascii_equal(value.first, field_name);
        });
    if (found == defaults.field_defaults.end()) return std::nullopt;
    return found->second;
}

std::string quote_sql_string(
        const std::vector<std::uint8_t>& bytes) {
    std::string result("'");
    result.reserve(bytes.size() + 2);
    for (const auto byte : bytes) {
        const char value = static_cast<char>(byte);
        result.push_back(value);
        if (value == '\'') result.push_back('\'');
    }
    result.push_back('\'');
    return result;
}

template <typename T>
std::optional<T> read_little_endian_scalar(
        const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < sizeof(T)) return std::nullopt;
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bits |= static_cast<std::uint64_t>(bytes[i]) << (i * 8U);
    }
    T value{};
    std::memcpy(&value, &bits, sizeof(T));
    return value;
}

std::optional<std::string> date_default(
        const explorgdb::FieldDescriptor& descriptor) {
    const auto value =
        read_little_endian_scalar<double>(descriptor.default_value_raw);
    if (!value) return std::nullopt;
    std::string text;
    if (descriptor.type == explorgdb::FieldType::Date) {
        text = explorgdb::ole_date_only(*value);
    } else if (descriptor.type == explorgdb::FieldType::Time) {
        text = explorgdb::ole_time_only(*value);
    } else {
        text = explorgdb::ole_datetime(*value);
    }
    std::replace(text.begin(), text.end(), '-', '/');
    if (descriptor.type == explorgdb::FieldType::DateTimeWithOffset) {
        const auto offset = descriptor.default_value_raw.size() >= 10
            ? static_cast<std::int16_t>(
                  descriptor.default_value_raw[8] |
                  (descriptor.default_value_raw[9] << 8U))
            : 0;
        std::ostringstream zone;
        zone << ".000" << (offset >= 0 ? '+' : '-')
             << std::setfill('0') << std::setw(2)
             << std::abs(offset) / 60 << ':'
             << std::setw(2) << std::abs(offset) % 60;
        text += zone.str();
    }
    return "'" + text + "'";
}

std::optional<std::string> public_default_value(
        const explorgdb::FieldDescriptor& descriptor,
        const LayerDefinitionDefaults& defaults) {
    if (ascii_equal(descriptor.name, defaults.area_field) &&
        public_field_type(descriptor.type) == FieldType::Real) {
        return "FILEGEODATABASE_SHAPE_AREA";
    }
    if (ascii_equal(descriptor.name, defaults.length_field) &&
        public_field_type(descriptor.type) == FieldType::Real) {
        return "FILEGEODATABASE_SHAPE_LENGTH";
    }
    if (descriptor.default_value_raw.empty()) return std::nullopt;
    if (descriptor.type == explorgdb::FieldType::String ||
        descriptor.type == explorgdb::FieldType::XML) {
        return quote_sql_string(descriptor.default_value_raw);
    }
    if (descriptor.type == explorgdb::FieldType::DateTime ||
        descriptor.type == explorgdb::FieldType::Date ||
        descriptor.type == explorgdb::FieldType::Time ||
        descriptor.type == explorgdb::FieldType::DateTimeWithOffset) {
        return date_default(descriptor);
    }
    return numeric_xml_default(defaults, descriptor.name);
}

Result<LayerSchema> freeze_schema(
        explorgdb::Layer& layer,
        std::vector<std::size_t>& indices) {
    LayerSchema schema;
    schema.name = layer.name();
    const auto metadata = layer.read_metadata();
    if (!metadata.ok()) return reader_error(metadata.error);
    const auto defaults =
        parse_layer_definition_defaults(metadata.snapshot.layer);
    const auto& descriptors = layer.fields();
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        const auto& descriptor = descriptors[i];
        if (descriptor.type == explorgdb::FieldType::Geometry) {
            schema.geometry_field = 0;
            schema.srs_wkt = descriptor.wkt;
            continue;
        }
        if (descriptor.type == explorgdb::FieldType::Raster ||
            descriptor.type == explorgdb::FieldType::ObjectId) {
            continue;
        }
        FieldDefinition field;
        field.name = descriptor.name;
        field.alias = descriptor.alias;
        field.type = public_field_type(descriptor.type);
        field.nullable = (descriptor.flag & 0x01U) != 0;
        field.default_value = public_default_value(descriptor, defaults);
        const auto binding = std::find_if(
            metadata.snapshot.field_domains.begin(),
            metadata.snapshot.field_domains.end(),
            [&descriptor](const explorgdb::FieldDomainBinding& value) {
                return value.field_name == descriptor.name;
            });
        if (binding != metadata.snapshot.field_domains.end()) {
            field.domain_name = binding->domain_name;
        }
        indices.push_back(i);
        schema.fields.push_back(std::move(field));
    }
    if (schema.geometry_field && defaults.geometry_type) {
        schema.geometry_type = *defaults.geometry_type;
    } else if (schema.geometry_field) {
        explorgdb::QueryRequest request;
        request.kind = explorgdb::QueryKind::SequentialScan;
        request.limit = 1;
        auto cursor = layer.open_cursor(request);
        explorgdb::QueryFeature feature;
        if (cursor.next(feature)) {
            schema.geometry_type = feature.geometry.geometry_type;
        }
    }
    return schema;
}

Field map_field(const explorgdb::FieldDescriptor& descriptor,
                const explorgdb::FeatureRecord& record,
                std::size_t source_index) {
    if (!record.materialized_fields.empty() &&
        (source_index >= record.materialized_fields.size() ||
         record.materialized_fields[source_index] == 0)) {
        return {};
    }
    if (source_index >= record.field_values.size()) {
        return {};
    }
    const auto& source = record.field_values[source_index];
    if (std::holds_alternative<std::nullptr_t>(source)) {
        return {ValueState::Null, std::nullopt};
    }
    Field result;
    result.state = ValueState::Value;
    if (const auto* value = std::get_if<std::int32_t>(&source)) {
        result.value = *value;
    } else if (const auto* value = std::get_if<std::int64_t>(&source)) {
        result.value = *value;
    } else if (const auto* value = std::get_if<double>(&source)) {
        if (descriptor.type == explorgdb::FieldType::Date) {
            result.value = DateValue{explorgdb::ole_date_only(*value)};
        } else if (descriptor.type == explorgdb::FieldType::Time) {
            result.value = TimeValue{explorgdb::ole_time_only(*value)};
        } else if (descriptor.type == explorgdb::FieldType::DateTime) {
            result.value = DateTimeValue{
                explorgdb::ole_datetime(*value), std::nullopt};
        } else {
            result.value = *value;
        }
    } else if (const auto* value =
                   std::get_if<explorgdb::DateTimeOffsetValue>(&source)) {
        result.value = DateTimeValue{
            explorgdb::ole_datetime(value->date), value->offset_minutes};
    } else if (const auto* value = std::get_if<std::string>(&source)) {
        result.value = *value;
    } else if (const auto* value =
                   std::get_if<std::vector<std::uint8_t>>(&source)) {
        result.value = *value;
    }
    return result;
}

Geometry map_geometry(const explorgdb::GeometryValue& source) {
    Geometry geometry;
    geometry.type = source.geometry_type;
    geometry.srid = source.srid;
    geometry.has_z = source.has_z;
    geometry.has_m = source.has_m;
    geometry.wkb = source.wkb;
    if (source.status == explorgdb::GeometryStatus::Null) {
        geometry.state = GeometryState::Null;
    } else if (source.status == explorgdb::GeometryStatus::Empty) {
        geometry.state = GeometryState::Empty;
    } else if (source.status == explorgdb::GeometryStatus::Valid) {
        geometry.state = source.wkb.empty()
            ? GeometryState::Null : GeometryState::Value;
    } else {
        geometry.state = GeometryState::Unset;
    }
    return geometry;
}

Feature map_feature(const explorgdb::QueryFeature& source,
                    const detail::LayerState& state,
                    const explorgdb::Layer& layer) {
    Feature feature;
    feature.fid = static_cast<Fid>(source.fid) + 1;
    feature.fields.reserve(state.source_field_indices.size());
    const auto& descriptors = layer.fields();
    for (const auto source_index : state.source_field_indices) {
        feature.fields.push_back(
            map_field(descriptors[source_index], source.record, source_index));
    }
    feature.geometry = map_geometry(source.geometry);
    feature.geometry.srs_wkt = state.schema.srs_wkt;
    return feature;
}

Error validate_query(const Query& query, const detail::LayerState& state,
                     explorgdb::QueryRequest& request) {
    if (query.offset > std::numeric_limits<std::size_t>::max() ||
        query.limit > std::numeric_limits<std::size_t>::max()) {
        return make_error(ErrorCode::ResultLimitExceeded,
                          "offset or limit exceeds platform range");
    }
    request.offset = static_cast<std::size_t>(query.offset);
    request.limit = static_cast<std::size_t>(query.limit);
    request.sort_fids = query.order == ResultOrder::FidAscending;
    if (request.sort_fids) {
        const auto max_fids = query.max_ordered_fid_bytes / sizeof(Fid);
        request.max_result_features = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                max_fids, std::numeric_limits<std::size_t>::max()));
    }
    request.cancel_requested = query.cancel_requested;
    if (query.deadline) {
        const auto deadline = *query.deadline;
        const auto cancelled = request.cancel_requested;
        request.cancel_requested = [deadline, cancelled] {
            return (cancelled && cancelled()) ||
                   std::chrono::steady_clock::now() >= deadline;
        };
    }
    if (!query.projected_fields.empty()) {
        std::vector<std::size_t> projection;
        for (const auto& name : query.projected_fields) {
            auto found = std::find_if(
                state.schema.fields.begin(), state.schema.fields.end(),
                [&name](const FieldDefinition& field) {
                    return field.name == name;
                });
            if (found == state.schema.fields.end()) {
                return make_error(ErrorCode::Unsupported,
                                  "projected field does not exist: " + name);
            }
            const auto public_index = static_cast<std::size_t>(
                std::distance(state.schema.fields.begin(), found));
            projection.push_back(state.source_field_indices[public_index]);
        }
        request.field_projection = std::move(projection);
    }
    if (query.spatial_filter && !query.attribute_filter.empty()) {
        request.kind = explorgdb::QueryKind::SpatialWhere;
    } else if (query.spatial_filter) {
        request.kind = explorgdb::QueryKind::SpatialBbox;
    } else if (!query.attribute_filter.empty()) {
        request.kind = explorgdb::QueryKind::WhereClause;
    } else {
        request.kind = explorgdb::QueryKind::SequentialScan;
    }
    request.where_clause = query.attribute_filter;
    if (query.spatial_filter) {
        request.xmin = query.spatial_filter->xmin;
        request.ymin = query.spatial_filter->ymin;
        request.xmax = query.spatial_filter->xmax;
        request.ymax = query.spatial_filter->ymax;
    }
    return {};
}

bool fast_query_has_known_capability_gap(const Query& query) {
    std::string upper = query.attribute_filter;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::toupper(value));
                   });
    return upper.find(" LIKE ") != std::string::npos ||
           upper.find(" IS NULL") != std::string::npos ||
           upper.find(" IS NOT NULL") != std::string::npos;
}

std::uint64_t feature_payload_bytes(const Feature& feature) {
    std::uint64_t bytes = feature.geometry.wkb.capacity() +
                          feature.geometry.srs_wkt.capacity();
    bytes += feature.fields.capacity() * sizeof(Field);
    for (const auto& field : feature.fields) {
        if (!field.value) continue;
        if (const auto* text = std::get_if<std::string>(&*field.value)) {
            bytes += text->capacity();
        } else if (const auto* binary =
                       std::get_if<std::vector<std::uint8_t>>(&*field.value)) {
            bytes += binary->capacity();
        } else if (const auto* date = std::get_if<DateValue>(&*field.value)) {
            bytes += date->iso8601.capacity();
        } else if (const auto* time = std::get_if<TimeValue>(&*field.value)) {
            bytes += time->iso8601.capacity();
        } else if (const auto* datetime =
                       std::get_if<DateTimeValue>(&*field.value)) {
            bytes += datetime->iso8601.capacity();
        }
    }
    return bytes;
}

std::uint64_t feature_bytes(const Feature& feature) {
    const auto payload = feature_payload_bytes(feature);
    return payload > std::numeric_limits<std::uint64_t>::max() -
                         sizeof(Feature)
        ? std::numeric_limits<std::uint64_t>::max()
        : payload + sizeof(Feature);
}

ErrorCode query_error_code(
        explorgdb::QueryStatus status,
        const std::optional<std::chrono::steady_clock::time_point>& deadline) {
    switch (status) {
        case explorgdb::QueryStatus::Unsupported:
            return ErrorCode::Unsupported;
        case explorgdb::QueryStatus::SourceChanged:
            return ErrorCode::SourceChanged;
        case explorgdb::QueryStatus::ResultLimitExceeded:
            return ErrorCode::ResultLimitExceeded;
        case explorgdb::QueryStatus::Cancelled:
            return deadline &&
                           std::chrono::steady_clock::now() >= *deadline
                ? ErrorCode::DeadlineExceeded : ErrorCode::Cancelled;
        default:
            return ErrorCode::ReadFailed;
    }
}

Result<FeatureCursor> open_fast_cursor(
        const std::shared_ptr<detail::LayerState>& state,
        const Query& query) {
    auto lease_result = acquire_fast_reader_lease(
        state->dataset->source.normalized_uri);
    if (!lease_result) return lease_result.error();
    auto lease = std::move(lease_result).value();
    explorgdb::ReaderError reader_status;
    auto reader = explorgdb::Reader::open(
        state->dataset->source.normalized_uri, {}, &reader_status);
    if (!reader) return reader_error(reader_status);
    auto layer = reader->open_layer(state->name, &reader_status);
    if (!layer) return reader_error(reader_status);
    if (query.order == ResultOrder::FidAscending &&
        layer->active_feature_count() >
            query.max_ordered_fid_bytes / sizeof(Fid)) {
        return make_error(
            ErrorCode::ResultLimitExceeded,
            "ordered FID candidate set exceeds byte budget");
    }

    explorgdb::QueryRequest request;
    if (const auto error = validate_query(query, *state, request)) {
        return error;
    }
    auto cursor = layer->open_cursor(request);
    if (cursor.query_result().status != explorgdb::QueryStatus::Ok) {
        return make_error(
            query_error_code(cursor.query_result().status, query.deadline),
            cursor.query_result().error);
    }
    auto cursor_state = std::make_unique<detail::CursorState>();
    cursor_state->reader.emplace(std::move(*reader));
    cursor_state->layer.emplace(std::move(*layer));
    cursor_state->cursor = std::move(cursor);
    cursor_state->layer_state = state;
    cursor_state->report = state->report;
    cursor_state->consistency = state->dataset->consistency;
    cursor_state->fast_lease.emplace(std::move(lease));
    cursor_state->cancel_requested = query.cancel_requested;
    cursor_state->deadline = query.deadline;
    cursor_state->query_report.execution_path =
        cursor_state->cursor.query_result().execution_path;

    explorgdb::QueryFeature prefetched;
    if (cursor_state->cursor.next(prefetched)) {
        cursor_state->prefetched =
            map_feature(prefetched, *state, *cursor_state->layer);
    } else if (!cursor_state->cursor.error().empty()) {
        return make_error(query_error_code(
                cursor_state->cursor.query_result().status, query.deadline),
            cursor_state->cursor.error());
    }
    return FeatureCursor(std::move(cursor_state));
}

const BackendReport kEmptyBackendReport{};
const LayerSchema kEmptyLayerSchema{};

#if defined(FAST_GDB_UNIFIED_WITH_GDAL)

void ensure_gdal_initialized() {
    static std::once_flag initialized;
    std::call_once(initialized, [] { GDALAllRegister(); });
}

GDALDataset* open_openfilegdb(
        const std::string& uri, bool include_system_tables = false) {
    ensure_gdal_initialized();
    CPLErrorReset();
    const char* allowed[] = {"OpenFileGDB", nullptr};
    const char* list_all_tables[] = {"LIST_ALL_TABLES=YES", nullptr};
    return static_cast<GDALDataset*>(GDALOpenEx(
        uri.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
        allowed, include_system_tables ? list_all_tables : nullptr, nullptr));
}

OGRLayer* open_gdal_layer_by_path(GDALDataset& dataset,
                                  std::string_view path,
                                  std::string_view fallback_name) {
    if (path.empty() || path.front() != '/') return nullptr;
    auto group = dataset.GetRootGroup();
    if (!group) {
        return path.find('/', 1) == std::string_view::npos
            ? dataset.GetLayerByName(std::string(fallback_name).c_str())
            : nullptr;
    }
    std::size_t offset = 1;
    while (offset < path.size()) {
        const auto next = path.find('/', offset);
        const auto segment = std::string(path.substr(
            offset, next == std::string_view::npos
                        ? path.size() - offset : next - offset));
        if (next == std::string_view::npos) {
            return group->OpenVectorLayer(segment);
        }
        group = group->OpenGroup(segment);
        if (!group) return nullptr;
        offset = next + 1;
    }
    return nullptr;
}

Error gdal_error(ErrorCode code, std::string prefix) {
    const auto error_number = CPLGetLastErrorNo();
    if (error_number != 0) {
        prefix += ": ";
        prefix += "GDAL error ";
        prefix += std::to_string(error_number);
    }
    return make_error(code, std::move(prefix));
}

Error populate_gdal_groups(
        detail::DatasetState& state,
        const std::shared_ptr<GDALGroup>& group,
        const std::string& path = "/") {
    if (!group) return {};
    CPLErrorReset();
    const auto layer_names = group->GetVectorLayerNames();
    if (CPLGetLastErrorType() >= CE_Failure) {
        return gdal_error(ErrorCode::ReadFailed,
                          "OpenFileGDB Group Layer enumeration failed");
    }
    for (const auto& name : layer_names) {
        if (state.options.include_system_tables ||
            !is_system_layer(name)) {
            index_group_layer(
                state, name,
                path == "/" ? "/" + name : path + "/" + name);
        }
    }
    CPLErrorReset();
    const auto group_names = group->GetGroupNames();
    if (CPLGetLastErrorType() >= CE_Failure) {
        return gdal_error(ErrorCode::ReadFailed,
                          "OpenFileGDB Group enumeration failed");
    }
    for (const auto& name : group_names) {
        const auto child_path =
            path == "/" ? "/" + name : path + "/" + name;
        index_group(state, child_path);
        CPLErrorReset();
        auto child = group->OpenGroup(name);
        if (!child || CPLGetLastErrorType() >= CE_Failure) {
            return gdal_error(ErrorCode::ReadFailed,
                              "OpenFileGDB Group open failed");
        }
        if (auto error = populate_gdal_groups(
                state, child, child_path)) {
            return error;
        }
    }
    return {};
}

FieldType public_field_type(OGRFieldType type) {
    switch (type) {
        case OFTInteger:
            return FieldType::Integer;
        case OFTInteger64:
            return FieldType::Integer64;
        case OFTReal:
            return FieldType::Real;
        case OFTBinary:
            return FieldType::Binary;
        case OFTDate:
            return FieldType::Date;
        case OFTTime:
            return FieldType::Time;
        case OFTDateTime:
            return FieldType::DateTime;
        default:
            return FieldType::String;
    }
}

LayerSchema freeze_gdal_schema(OGRLayer& layer) {
    LayerSchema schema;
    OGRFeatureDefn* definition = layer.GetLayerDefn();
    schema.name = definition->GetName();
    for (int i = 0; i < definition->GetFieldCount(); ++i) {
        const OGRFieldDefn* source = definition->GetFieldDefn(i);
        FieldDefinition field;
        field.name = source->GetNameRef();
        field.alias = source->GetAlternativeNameRef();
        field.type = public_field_type(source->GetType());
        field.nullable = source->IsNullable();
        if (source->GetDefault() != nullptr) {
            field.default_value = source->GetDefault();
        }
        if (!source->GetDomainName().empty()) {
            field.domain_name = source->GetDomainName();
        }
        schema.fields.push_back(std::move(field));
    }
    if (definition->GetGeomFieldCount() > 0) {
        const OGRGeomFieldDefn* geometry = definition->GetGeomFieldDefn(0);
        schema.geometry_field = 0;
        const auto geometry_type = geometry->GetType();
        schema.geometry_type =
            static_cast<std::uint32_t>(wkbFlatten(geometry_type)) +
            (OGR_GT_HasZ(geometry_type) && OGR_GT_HasM(geometry_type)
                ? 3000U
                : (OGR_GT_HasZ(geometry_type)
                    ? 1000U
                    : (OGR_GT_HasM(geometry_type) ? 2000U : 0U)));
        if (geometry->GetSpatialRef() != nullptr) {
            char* wkt = nullptr;
            if (geometry->GetSpatialRef()->exportToWkt(&wkt) == OGRERR_NONE &&
                wkt != nullptr) {
                schema.srs_wkt = wkt;
            }
            CPLFree(wkt);
        }
    }
    return schema;
}

Error load_gdal_layer_schema(
        const std::shared_ptr<detail::LayerState>& state,
        FailureKind reason) {
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(
        open_openfilegdb(
            state->dataset->source.normalized_uri,
            state->dataset->options.include_system_tables),
        GDALClose);
    if (!dataset) {
        return gdal_error(ErrorCode::OpenFailed,
                          "OpenFileGDB schema open failed");
    }
    CPLErrorReset();
    OGRLayer* layer = open_gdal_layer_by_path(
        *dataset, state->path, state->name);
    if (!layer) {
        return gdal_error(ErrorCode::LayerNotFound,
                          "OpenFileGDB schema Layer open failed");
    }
    state->schema = freeze_gdal_schema(*layer);
    state->report.selected = Backend::GdalOpenFileGDB;
    if (reason != FailureKind::None) {
        state->report.reason = RouteReason::FastCapabilityGap;
        state->report.fallback_reason = reason;
    }
    return {};
}

bool schemas_compatible(const LayerSchema& frozen,
                        const LayerSchema& candidate) {
    if (frozen.fields.size() != candidate.fields.size()) return false;
    for (std::size_t i = 0; i < frozen.fields.size(); ++i) {
        const auto& lhs = frozen.fields[i];
        const auto& rhs = candidate.fields[i];
        if (lhs.name != rhs.name || lhs.alias != rhs.alias ||
            lhs.type != rhs.type ||
            lhs.nullable != rhs.nullable ||
            lhs.default_value != rhs.default_value ||
            lhs.domain_name != rhs.domain_name) {
            return false;
        }
    }
    if (frozen.geometry_field.has_value() !=
        candidate.geometry_field.has_value()) {
        return false;
    }
    if (frozen.geometry_type != candidate.geometry_type) {
        return false;
    }
    if (frozen.srs_wkt == candidate.srs_wkt) return true;
    if (frozen.srs_wkt.empty() || candidate.srs_wkt.empty()) return false;
    OGRSpatialReference frozen_srs;
    OGRSpatialReference candidate_srs;
    if (frozen_srs.SetFromUserInput(frozen.srs_wkt.c_str()) != OGRERR_NONE ||
        candidate_srs.SetFromUserInput(candidate.srs_wkt.c_str()) !=
            OGRERR_NONE) {
        return false;
    }
    return frozen_srs.IsSame(&candidate_srs);
}

Result<Feature> map_gdal_feature(const OGRFeature& source,
                                 const detail::LayerState& state);

Result<std::shared_ptr<detail::LayerState>> make_gdal_fallback_state(
        const std::shared_ptr<detail::LayerState>& state,
        FailureKind reason) {
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(
        open_openfilegdb(
            state->dataset->source.normalized_uri,
            state->dataset->options.include_system_tables),
        GDALClose);
    if (!dataset) {
        return gdal_error(ErrorCode::OpenFailed,
                          "OpenFileGDB fallback open failed");
    }
    CPLErrorReset();
    OGRLayer* layer = open_gdal_layer_by_path(
        *dataset, state->path, state->name);
    if (!layer) {
        return gdal_error(ErrorCode::LayerNotFound,
                          "OpenFileGDB fallback Layer open failed");
    }
    const auto candidate = freeze_gdal_schema(*layer);
    if (!schemas_compatible(state->schema, candidate)) {
        return make_error(
            ErrorCode::SchemaMismatch,
            "OpenFileGDB fallback schema differs from frozen schema "
            "(geometry types " +
                std::to_string(state->schema.geometry_type) + " vs " +
                std::to_string(candidate.geometry_type) + ")");
    }
    auto fallback = std::make_shared<detail::LayerState>(*state);
    fallback->dataset =
        std::make_shared<detail::DatasetState>(*state->dataset);
    fallback->report.selected = Backend::GdalOpenFileGDB;
    fallback->report.reason = RouteReason::FastCapabilityGap;
    fallback->report.fallback_reason = reason;
    return fallback;
}

Result<Feature> read_gdal_feature_by_fid(
        const std::shared_ptr<detail::LayerState>& state, Fid fid) {
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(
        open_openfilegdb(
            state->dataset->source.normalized_uri,
            state->dataset->options.include_system_tables),
        GDALClose);
    if (!dataset) {
        return gdal_error(ErrorCode::OpenFailed,
                          "OpenFileGDB Dataset open failed");
    }
    CPLErrorReset();
    OGRLayer* layer = open_gdal_layer_by_path(
        *dataset, state->path, state->name);
    if (!layer) {
        return gdal_error(ErrorCode::LayerNotFound,
                          "OpenFileGDB Layer open failed");
    }
    CPLErrorReset();
    std::unique_ptr<OGRFeature> feature(layer->GetFeature(fid));
    if (!feature) {
        if (CPLGetLastErrorType() >= CE_Failure) {
            return gdal_error(ErrorCode::ReadFailed,
                              "OpenFileGDB feature read failed");
        }
        return make_error(ErrorCode::FeatureNotFound, "FID does not exist");
    }
    return map_gdal_feature(*feature, *state);
}

std::string format_ogr_temporal(const OGRField& field, bool date, bool time) {
    char buffer[64] = {};
    if (date && time) {
        std::snprintf(buffer, sizeof(buffer),
                      "%04d-%02d-%02d %02d:%02d:%g",
                      field.Date.Year, field.Date.Month, field.Date.Day,
                      field.Date.Hour, field.Date.Minute, field.Date.Second);
    } else if (date) {
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d",
                      field.Date.Year, field.Date.Month, field.Date.Day);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%g",
                      field.Date.Hour, field.Date.Minute, field.Date.Second);
    }
    return buffer;
}

Field map_gdal_field(const OGRFeature& source, int index) {
    if (!source.IsFieldSet(index)) return {};
    if (source.IsFieldNull(index)) {
        return {ValueState::Null, std::nullopt};
    }
    Field field;
    field.state = ValueState::Value;
    const OGRFieldDefn* definition =
        source.GetDefnRef()->GetFieldDefn(index);
    switch (definition->GetType()) {
        case OFTInteger:
            field.value = static_cast<std::int32_t>(
                source.GetFieldAsInteger(index));
            break;
        case OFTInteger64:
            field.value = static_cast<std::int64_t>(
                source.GetFieldAsInteger64(index));
            break;
        case OFTReal:
            field.value = source.GetFieldAsDouble(index);
            break;
        case OFTBinary: {
            int size = 0;
            const auto* bytes = source.GetFieldAsBinary(index, &size);
            field.value = std::vector<std::uint8_t>(
                bytes, bytes + std::max(size, 0));
            break;
        }
        case OFTDate:
            field.value = DateValue{
                format_ogr_temporal(*source.GetRawFieldRef(index), true, false)};
            break;
        case OFTTime:
            field.value = TimeValue{
                format_ogr_temporal(*source.GetRawFieldRef(index), false, true)};
            break;
        case OFTDateTime: {
            const auto* raw = source.GetRawFieldRef(index);
            std::optional<std::int16_t> offset;
            if (raw->Date.TZFlag > 1 && raw->Date.TZFlag != 100) {
                offset = static_cast<std::int16_t>(
                    (raw->Date.TZFlag - 100) * 15);
            }
            field.value = DateTimeValue{
                format_ogr_temporal(*raw, true, true), offset};
            break;
        }
        default:
            field.value = std::string(source.GetFieldAsString(index));
            break;
    }
    return field;
}

Result<Geometry> map_gdal_geometry(const OGRFeature& source,
                                   const LayerSchema& schema) {
    Geometry result;
    result.srs_wkt = schema.srs_wkt;
    const OGRGeometry* geometry = source.GetGeometryRef();
    if (geometry == nullptr) {
        result.state = GeometryState::Null;
        return result;
    }
    result.type = static_cast<std::uint32_t>(geometry->getGeometryType());
    result.has_z = wkbHasZ(geometry->getGeometryType());
    result.has_m = wkbHasM(geometry->getGeometryType());
    if (geometry->IsEmpty()) {
        result.state = GeometryState::Empty;
        return result;
    }
    const auto size = geometry->WkbSize();
    result.wkb.resize(size);
    if (geometry->exportToWkb(wkbNDR, result.wkb.data(),
                              wkbVariantIso) != OGRERR_NONE) {
        return make_error(ErrorCode::ReadFailed,
                          "OpenFileGDB geometry WKB export failed");
    }
    result.state = GeometryState::Value;
    return result;
}

Result<Feature> map_gdal_feature(const OGRFeature& source,
                                 const detail::LayerState& state) {
    Feature feature;
    feature.fid = static_cast<Fid>(source.GetFID());
    feature.fields.reserve(state.schema.fields.size());
    for (int i = 0; i < source.GetFieldCount(); ++i) {
        feature.fields.push_back(map_gdal_field(source, i));
    }
    auto geometry = map_gdal_geometry(source, state.schema);
    if (!geometry) return geometry.error();
    feature.geometry = std::move(geometry).value();
    return feature;
}

std::unique_ptr<OGRFeature> next_gdal_source(detail::CursorState& state) {
    if (!state.ordered_fids.empty()) {
        if (state.ordered_index >= state.ordered_fids.size()) return nullptr;
        return std::unique_ptr<OGRFeature>(
            state.gdal_layer->GetFeature(
                state.ordered_fids[state.ordered_index++]));
    }
    return std::unique_ptr<OGRFeature>(state.gdal_layer->GetNextFeature());
}

Result<std::optional<Feature>> next_gdal(detail::CursorState& state) {
    const auto stop_error = [&]() -> std::optional<Error> {
        if (state.deadline &&
            std::chrono::steady_clock::now() >= *state.deadline) {
            return make_error(ErrorCode::DeadlineExceeded,
                              "OpenFileGDB read deadline exceeded");
        }
        if (state.cancel_requested && state.cancel_requested()) {
            return make_error(ErrorCode::Cancelled,
                              "OpenFileGDB read cancelled");
        }
        return std::nullopt;
    };
    if (auto error = stop_error()) return *error;
    while (state.remaining_skip > 0) {
        CPLErrorReset();
        auto skipped = next_gdal_source(state);
        if (!skipped) {
            if (CPLGetLastErrorType() >= CE_Failure) {
                return gdal_error(ErrorCode::ReadFailed,
                                  "OpenFileGDB feature skip failed");
            }
            return std::optional<Feature>{};
        }
        if (auto error = stop_error()) return *error;
        --state.remaining_skip;
    }
    if (state.limit_enabled && state.remaining_limit == 0) {
        return std::optional<Feature>{};
    }
    CPLErrorReset();
    auto source = next_gdal_source(state);
    if (!source) {
        if (CPLGetLastErrorType() >= CE_Failure) {
            return gdal_error(ErrorCode::ReadFailed,
                              "OpenFileGDB feature read failed");
        }
        return std::optional<Feature>{};
    }
    if (auto error = stop_error()) return *error;
    if (state.limit_enabled) --state.remaining_limit;
    auto feature = map_gdal_feature(*source, *state.layer_state);
    if (!feature) return feature.error();
    return std::optional<Feature>{std::move(feature).value()};
}

Result<FeatureCursor> open_gdal_cursor(
        const std::shared_ptr<detail::LayerState>& state,
        const Query& query) {
    auto cursor_state = std::make_unique<detail::CursorState>();
    cursor_state->cancel_requested = query.cancel_requested;
    cursor_state->deadline = query.deadline;
    cursor_state->gdal_dataset =
        open_openfilegdb(
            state->dataset->source.normalized_uri,
            state->dataset->options.include_system_tables);
    if (!cursor_state->gdal_dataset) {
        return gdal_error(ErrorCode::OpenFailed,
                          "OpenFileGDB Dataset open failed");
    }
    CPLErrorReset();
    cursor_state->gdal_layer = open_gdal_layer_by_path(
        *cursor_state->gdal_dataset, state->path, state->name);
    if (!cursor_state->gdal_layer) {
        return gdal_error(ErrorCode::LayerNotFound,
                          "OpenFileGDB Layer open failed");
    }
    if (!query.attribute_filter.empty() &&
        (CPLErrorReset(),
         cursor_state->gdal_layer->SetAttributeFilter(
             query.attribute_filter.c_str())) != OGRERR_NONE) {
        return gdal_error(ErrorCode::Unsupported,
                          "OpenFileGDB attribute filter rejected");
    }
    if (query.spatial_filter) {
        const auto& box = *query.spatial_filter;
        CPLErrorReset();
        cursor_state->gdal_layer->SetSpatialFilterRect(
            box.xmin, box.ymin, box.xmax, box.ymax);
        if (CPLGetLastErrorType() >= CE_Failure) {
            return gdal_error(ErrorCode::Unsupported,
                              "OpenFileGDB spatial filter rejected");
        }
    }
    if (!query.projected_fields.empty()) {
        std::vector<std::string> ignored_names;
        for (const auto& field : state->schema.fields) {
            if (std::find(query.projected_fields.begin(),
                          query.projected_fields.end(),
                          field.name) == query.projected_fields.end()) {
                ignored_names.push_back(field.name);
            }
        }
        if (ignored_names.size() + query.projected_fields.size() !=
            state->schema.fields.size()) {
            return make_error(ErrorCode::Unsupported,
                              "projected field does not exist");
        }
        std::vector<const char*> ignored;
        ignored.reserve(ignored_names.size() + 1);
        for (const auto& field : ignored_names) {
            ignored.push_back(field.c_str());
        }
        ignored.push_back(nullptr);
        CPLErrorReset();
        if (cursor_state->gdal_layer->SetIgnoredFields(
                ignored.data()) != OGRERR_NONE) {
            return gdal_error(ErrorCode::Unsupported,
                              "OpenFileGDB projection rejected");
        }
    }
    if (query.order == ResultOrder::FidAscending) {
        CPLErrorReset();
        while (true) {
            if (cursor_state->deadline &&
                std::chrono::steady_clock::now() >=
                    *cursor_state->deadline) {
                return make_error(ErrorCode::DeadlineExceeded,
                                  "OpenFileGDB read deadline exceeded");
            }
            if (cursor_state->cancel_requested &&
                cursor_state->cancel_requested()) {
                return make_error(ErrorCode::Cancelled,
                                  "OpenFileGDB read cancelled");
            }
            auto feature = std::unique_ptr<OGRFeature>(
                cursor_state->gdal_layer->GetNextFeature());
            if (!feature) break;
            if (cursor_state->ordered_fids.size() >=
                query.max_ordered_fid_bytes / sizeof(Fid)) {
                return make_error(
                    ErrorCode::ResultLimitExceeded,
                    "ordered FID materialization exceeds byte budget");
            }
            cursor_state->ordered_fids.push_back(feature->GetFID());
        }
        if (CPLGetLastErrorType() >= CE_Failure) {
            return gdal_error(ErrorCode::ReadFailed,
                              "OpenFileGDB FID materialization failed");
        }
        std::sort(cursor_state->ordered_fids.begin(),
                  cursor_state->ordered_fids.end());
    }
    cursor_state->remaining_skip = query.offset;
    cursor_state->remaining_limit = query.limit;
    cursor_state->limit_enabled = query.limit != 0;
    cursor_state->layer_state = state;
    cursor_state->report = state->report;
    cursor_state->consistency = state->dataset->consistency;
    cursor_state->query_report.execution_path =
        query.order == ResultOrder::FidAscending
            ? "gdal/openfilegdb/fid_ascending"
            : "gdal/openfilegdb/native";
    auto prefetched = next_gdal(*cursor_state);
    if (!prefetched) return prefetched.error();
    if (prefetched.value()) {
        cursor_state->prefetched = std::move(*prefetched.value());
    }
    return FeatureCursor(std::move(cursor_state));
}

#endif

}  // namespace

FeatureCursor::FeatureCursor() = default;
FeatureCursor::FeatureCursor(std::unique_ptr<detail::CursorState> state)
    : state_(std::move(state)) {}
FeatureCursor::FeatureCursor(FeatureCursor&&) noexcept = default;
FeatureCursor& FeatureCursor::operator=(FeatureCursor&&) noexcept = default;
FeatureCursor::~FeatureCursor() = default;

Result<std::optional<Feature>> FeatureCursor::next() {
    if (!state_ || state_->closed) return std::optional<Feature>{};
    if (state_->deadline &&
        std::chrono::steady_clock::now() >= *state_->deadline) {
        state_->closed = true;
        return make_error(ErrorCode::DeadlineExceeded,
                          "feature cursor deadline exceeded");
    }
    if (state_->cancel_requested && state_->cancel_requested()) {
        state_->closed = true;
        return make_error(ErrorCode::Cancelled,
                          "feature cursor cancelled");
    }
    if (state_->prefetched) {
        auto feature = std::move(state_->prefetched);
        state_->prefetched.reset();
        ++state_->query_report.feature_count;
        state_->query_report.materialized_bytes += feature_bytes(*feature);
        return feature;
    }
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
    if (state_->gdal_dataset != nullptr) {
        auto result = next_gdal(*state_);
        if (result && result.value()) {
            ++state_->query_report.feature_count;
            state_->query_report.materialized_bytes +=
                feature_bytes(*result.value());
        }
        if (result && !result.value()) state_->closed = true;
        return result;
    }
#endif
    explorgdb::QueryFeature source;
    if (state_->cursor.next(source)) {
        auto feature =
            map_feature(source, *state_->layer_state, *state_->layer);
        ++state_->query_report.feature_count;
        state_->query_report.materialized_bytes += feature_bytes(feature);
        return std::optional<Feature>{std::move(feature)};
    }
    if (!state_->cursor.error().empty()) {
        return make_error(
            query_error_code(state_->cursor.query_result().status,
                             state_->deadline),
            state_->cursor.error());
    }
    state_->closed = true;
    return std::optional<Feature>{};
}

const BackendReport& FeatureCursor::backend_report() const noexcept {
    return state_ ? state_->report : kEmptyBackendReport;
}

const ConsistencyReport& FeatureCursor::consistency_report() const noexcept {
    static const ConsistencyReport empty;
    return state_ ? state_->consistency : empty;
}

const QueryReport& FeatureCursor::query_report() const noexcept {
    static const QueryReport empty;
    return state_ ? state_->query_report : empty;
}

void FeatureCursor::close() noexcept {
    state_.reset();
}

Layer::Layer(std::shared_ptr<detail::LayerState> state)
    : state_(std::move(state)) {}

const LayerSchema& Layer::schema() const noexcept {
    return state_ ? state_->schema : kEmptyLayerSchema;
}

Result<FeatureCursor> Layer::open_cursor(const Query& query) const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Layer is closed");
    if (const auto error = validate_public_query(query, state_->schema)) {
        return error;
    }
    if (state_->report.selected == Backend::FastGdb) {
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
        if (fast_query_has_known_capability_gap(query)) {
            if (state_->dataset->options.backend ==
                BackendPreference::FastOnly) {
                return make_error(ErrorCode::Unsupported,
                                  "query exceeds fast backend capabilities");
            }
            auto fallback = make_gdal_fallback_state(
                state_, FailureKind::UnsupportedQuery);
            if (!fallback) return fallback.error();
            return open_gdal_cursor(fallback.value(), query);
        }
#else
        if (fast_query_has_known_capability_gap(query)) {
            return make_error(ErrorCode::BackendUnavailable,
                              "query fallback requires OpenFileGDB");
        }
#endif
        auto result = open_fast_cursor(state_, query);
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
        if (!result && result.error().code == ErrorCode::SourceBusy &&
            state_->dataset->options.backend == BackendPreference::Auto &&
            state_->dataset->options.concurrent_read ==
                ConcurrentReadPolicy::GdalUnverified) {
            auto fallback = make_gdal_fallback_state(
                state_, FailureKind::CapabilityGap);
            if (!fallback) return fallback.error();
            fallback.value()->dataset->consistency.consistency =
                Consistency::UnverifiedConcurrentRead;
            return open_gdal_cursor(fallback.value(), query);
        }
        if (!result && result.error().code == ErrorCode::Unsupported &&
            state_->dataset->options.backend == BackendPreference::Auto) {
            auto fallback = make_gdal_fallback_state(
                state_, FailureKind::UnsupportedQuery);
            if (!fallback) return fallback.error();
            return open_gdal_cursor(fallback.value(), query);
        }
#endif
        return result;
    }
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
    return open_gdal_cursor(state_, query);
#else
    return make_error(ErrorCode::BackendUnavailable,
                      "GDAL adapter is unavailable");
#endif
}

Result<Feature> Layer::read_by_fid(Fid fid) const {
    if (fid <= 0) {
        return make_error(ErrorCode::Unsupported,
                          "FID must be a positive 64-bit value");
    }
    const bool exceeds_fast_slot =
        static_cast<std::uint64_t>(fid - 1) >
        std::numeric_limits<std::uint32_t>::max();
    if (exceeds_fast_slot && state_ &&
        state_->report.selected == Backend::FastGdb) {
        if (state_->dataset->options.backend ==
            BackendPreference::FastOnly) {
            return make_error(ErrorCode::Unsupported,
                              "FID cannot be mapped to a fast row slot");
        }
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
        auto fallback = make_gdal_fallback_state(
            state_, FailureKind::UnsupportedFid);
        if (!fallback) return fallback.error();
        return read_gdal_feature_by_fid(fallback.value(), fid);
#else
        return make_error(ErrorCode::BackendUnavailable,
                          "FID fallback requires OpenFileGDB");
#endif
    }
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
    if (state_ &&
        state_->report.selected == Backend::GdalOpenFileGDB) {
        return read_gdal_feature_by_fid(state_, fid);
    }
#endif
    Query query;
    auto cursor_result = open_cursor(query);
    if (!cursor_result) return cursor_result.error();
    auto cursor = std::move(cursor_result).value();
    while (true) {
        auto next = cursor.next();
        if (!next) return next.error();
        if (!next.value()) {
            return make_error(ErrorCode::FeatureNotFound,
                              "FID does not exist");
        }
        if (next.value()->fid == fid) return std::move(*next.value());
        if (next.value()->fid > fid) {
            return make_error(ErrorCode::FeatureNotFound,
                              "FID does not exist");
        }
    }
}

Result<ReadBatch> Layer::read_all(const Query& query,
                                  ReadAllOptions options) const {
    auto cursor_result = open_cursor(query);
    if (!cursor_result) return cursor_result.error();
    auto cursor = std::move(cursor_result).value();
    ReadBatch batch;
    batch.backend_report = cursor.backend_report();
    std::uint64_t payload_bytes = 0;
    while (true) {
        auto next = cursor.next();
        if (!next) return next.error();
        if (!next.value()) break;
        const auto bytes = feature_payload_bytes(*next.value());
        std::size_t next_capacity = batch.features.capacity();
        if (batch.features.size() == next_capacity) {
            next_capacity = next_capacity == 0
                ? 1
                : (next_capacity >
                           std::numeric_limits<std::size_t>::max() / 2
                       ? std::numeric_limits<std::size_t>::max()
                       : next_capacity * 2);
        }
        const bool container_overflow =
            next_capacity >
            std::numeric_limits<std::uint64_t>::max() / sizeof(Feature);
        const auto container_bytes = container_overflow
            ? std::numeric_limits<std::uint64_t>::max()
            : static_cast<std::uint64_t>(next_capacity) * sizeof(Feature);
        const bool total_overflow =
            container_overflow ||
            bytes > std::numeric_limits<std::uint64_t>::max() -
                        payload_bytes ||
            container_bytes >
                std::numeric_limits<std::uint64_t>::max() -
                    (payload_bytes + std::min(
                        bytes,
                        std::numeric_limits<std::uint64_t>::max() -
                            payload_bytes));
        const auto projected_bytes = total_overflow
            ? std::numeric_limits<std::uint64_t>::max()
            : payload_bytes + bytes + container_bytes;
        if (!options.unlimited &&
            (bytes > options.max_feature_bytes ||
             batch.features.size() >= options.max_features ||
             projected_bytes > options.max_materialized_bytes)) {
            return make_error(ErrorCode::ResultLimitExceeded,
                              "read_all materialization limit exceeded");
        }
        payload_bytes += bytes;
        batch.features.push_back(std::move(*next.value()));
        batch.materialized_bytes =
            payload_bytes + batch.features.capacity() * sizeof(Feature);
    }
    return batch;
}

const BackendReport& Layer::backend_report() const noexcept {
    return state_ ? state_->report : kEmptyBackendReport;
}

const ConsistencyReport& Layer::consistency_report() const noexcept {
    static const ConsistencyReport empty;
    return state_ ? state_->dataset->consistency : empty;
}

Result<CapabilityReport> Layer::capabilities() const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Layer is closed");
    CapabilityReport report;
    report.native_extensions =
        state_->report.selected == Backend::FastGdb;
    return report;
}

Result<QueryPlan> Layer::explain(const Query& query) const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Layer is closed");
    if (const auto error = validate_public_query(query, state_->schema)) {
        return error;
    }
    QueryPlan plan;
    plan.backend = state_->report.selected;
    plan.materializes_fids =
        query.order == ResultOrder::FidAscending;
    plan.fallback_possible_before_publication =
        state_->dataset->options.backend == BackendPreference::Auto;
    if (plan.backend == Backend::FastGdb) {
        if (query.spatial_filter && !query.attribute_filter.empty()) {
            plan.execution_path = "fast/spatial_where";
        } else if (query.spatial_filter) {
            plan.execution_path = "fast/spatial_bbox";
        } else if (!query.attribute_filter.empty()) {
            plan.execution_path = "fast/where";
        } else {
            plan.execution_path = "fast/sequential";
        }
    } else {
        plan.execution_path = plan.materializes_fids
            ? "gdal/openfilegdb/fid_ascending"
            : "gdal/openfilegdb/native";
    }
    return plan;
}

Result<FastLayerExtensions> Layer::fast_extensions() const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Layer is closed");
    if (state_->report.selected != Backend::FastGdb) {
        return make_error(ErrorCode::Unsupported,
                          "native extensions require FastOnly semantics");
    }
    return FastLayerExtensions(state_);
}

FastLayerExtensions::FastLayerExtensions(
        std::shared_ptr<detail::LayerState> state)
    : state_(std::move(state)) {}

Result<FastNativeFeature> FastLayerExtensions::read_native_by_fid(
        Fid fid, NativeReadLimits limits) const {
    if (!state_ || fid <= 0 ||
        static_cast<std::uint64_t>(fid - 1) >
            std::numeric_limits<std::uint32_t>::max()) {
        return make_error(ErrorCode::Unsupported,
                          "FID cannot be mapped to a fast row slot");
    }
    auto lease_result = acquire_fast_reader_lease(
        state_->dataset->source.normalized_uri);
    if (!lease_result) return lease_result.error();
    auto lease = std::move(lease_result).value();
    explorgdb::ReaderError status;
    auto reader = explorgdb::Reader::open(
        state_->dataset->source.normalized_uri, {}, &status);
    if (!reader) return reader_error(status);
    auto layer = reader->open_layer(state_->name, &status);
    if (!layer) return reader_error(status);
    std::uint64_t descriptor_bytes = sizeof(FastNativeFeature);
    for (const auto& source : layer->fields()) {
        const auto physical_type = explorgdb::field_type_name(source.type);
        const auto payload = static_cast<std::uint64_t>(source.name.size()) +
                             std::strlen(physical_type) +
                             source.default_value_raw.size();
        const auto fixed = sizeof(NativeFieldDescriptor);
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (fixed > maximum - descriptor_bytes ||
            payload > maximum - descriptor_bytes - fixed) {
            return make_error(ErrorCode::ResultLimitExceeded,
                              "native descriptor size overflow");
        }
        descriptor_bytes += fixed + payload;
    }
    if (descriptor_bytes > limits.max_materialized_bytes) {
        return make_error(ErrorCode::ResultLimitExceeded,
                          "native descriptors exceed byte limit");
    }
    const auto raw_budget = std::min(
        limits.max_raw_bytes,
        limits.max_materialized_bytes - descriptor_bytes);
    const auto bounded_raw_budget = static_cast<std::size_t>(
        std::min<std::uint64_t>(
            raw_budget, std::numeric_limits<std::size_t>::max()));
    FastNativeFeature feature;
    feature.fid = fid;
    feature.row_slot = static_cast<std::uint32_t>(fid - 1);
    bool limit_exceeded = false;
    if (!layer->read_raw_by_fid(
            feature.row_slot, feature.raw_record, bounded_raw_budget,
            &limit_exceeded)) {
        if (limit_exceeded) {
            return make_error(ErrorCode::ResultLimitExceeded,
                              "native raw record exceeds byte limit");
        }
        return make_error(ErrorCode::ReadFailed,
                          "native FileGDB record read failed");
    }
    feature.descriptors.reserve(layer->fields().size());
    for (const auto& source : layer->fields()) {
        NativeFieldDescriptor descriptor;
        descriptor.name = source.name;
        descriptor.physical_type = explorgdb::field_type_name(source.type);
        descriptor.width = source.width;
        descriptor.flags = source.flag;
        descriptor.default_value_raw = source.default_value_raw;
        feature.descriptors.push_back(std::move(descriptor));
    }
    return feature;
}

Dataset::Dataset(std::shared_ptr<detail::DatasetState> state)
    : state_(std::move(state)) {}

Result<Dataset> Dataset::open(std::string uri, OpenOptions options) {
    Source source;
    if (auto error = parse_source(uri, source)) return error;
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
    constexpr bool gdal_available = true;
#else
    constexpr bool gdal_available = false;
#endif
    const auto routed = route(
        {source, options.backend, true, FailureKind::None, gdal_available});
    if (!routed) return routed.error;
    std::optional<explorgdb::FastReaderLease> metadata_lease;
    if (routed.report.selected == Backend::FastGdb) {
        auto lease = acquire_fast_reader_lease(source.normalized_uri);
        if (!lease) return lease.error();
        metadata_lease.emplace(std::move(lease).value());
    }
    auto state = std::make_shared<detail::DatasetState>();
    state->source = std::move(source);
    state->options = options;
    state->report = routed.report;
    if (metadata_lease) {
        state->consistency.generation = metadata_lease->generation();
    }
    state->consistency.consistency =
        state->source.kind == SourceKind::LocalFileGdb
            ? Consistency::LocalSnapshot
            : (options.remote_source ==
                       RemoteSourcePolicy::ImmutablePrefixRequired
                   ? Consistency::ImmutablePrefixAssumed
                   : Consistency::RemoteUnverified);
    if (routed.report.selected == Backend::FastGdb) {
        explorgdb::ReaderError status;
        auto reader = explorgdb::Reader::open(
            state->source.normalized_uri, {}, &status);
        if (!reader) return reader_error(status);
        state->layer_names = reader->layer_names();
    } else {
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
        std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(
            open_openfilegdb(
                state->source.normalized_uri,
                options.include_system_tables),
            GDALClose);
        if (!dataset) {
            return gdal_error(ErrorCode::OpenFailed,
                              "OpenFileGDB Dataset open failed");
        }
        for (int i = 0; i < dataset->GetLayerCount(); ++i) {
            OGRLayer* layer = dataset->GetLayer(i);
            if (layer != nullptr) state->layer_names.push_back(layer->GetName());
        }
        if (auto error = populate_gdal_groups(
                *state, dataset->GetRootGroup())) {
            return error;
        }
#else
        return make_error(ErrorCode::BackendUnavailable,
                          "GDAL adapter is unavailable");
#endif
    }
    if (!options.include_system_tables) {
        state->layer_names.erase(
            std::remove_if(state->layer_names.begin(),
                           state->layer_names.end(),
                           is_system_layer),
            state->layer_names.end());
    }
    if (routed.report.selected == Backend::FastGdb) {
        explorgdb::ReaderError status;
        auto reader = explorgdb::Reader::open(
            state->source.normalized_uri, {}, &status);
        if (!reader) return reader_error(status);
        populate_fast_groups(*state, *reader);
    } else if (state->paths_by_name.empty()) {
        for (const auto& name : state->layer_names) {
            index_group_layer(*state, name, "/" + name);
        }
    }
    return Dataset(std::move(state));
}

Result<std::vector<std::string>> Dataset::layer_names() const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Dataset is closed");
    return state_->layer_names;
}

Result<Layer> Dataset::open_layer(std::string_view name) const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Dataset is closed");
    std::optional<explorgdb::FastReaderLease> metadata_lease;
    if (state_->report.selected == Backend::FastGdb) {
        auto lease =
            acquire_fast_reader_lease(state_->source.normalized_uri);
        if (!lease) return lease.error();
        metadata_lease.emplace(std::move(lease).value());
    }
    std::vector<std::pair<std::string, const std::vector<std::string>*>>
        matches;
    const auto exact = state_->paths_by_name.find(std::string(name));
    if (exact != state_->paths_by_name.end()) {
        matches.push_back({exact->first, &exact->second});
    } else {
        for (const auto& candidate : state_->paths_by_name) {
            if (ascii_equal(candidate.first, name)) {
                matches.push_back({candidate.first, &candidate.second});
            }
        }
    }
    if (matches.empty()) {
        return make_error(ErrorCode::LayerNotFound, "Layer does not exist");
    }
    std::size_t path_count = 0;
    for (const auto& match : matches) path_count += match.second->size();
    if (path_count > 1) {
        return make_error(ErrorCode::AmbiguousLayer, "Layer name is ambiguous");
    }
    const std::string selected_name = matches.front().first;
    auto layer_state = std::make_shared<detail::LayerState>();
    layer_state->dataset = state_;
    layer_state->name = selected_name;
    layer_state->path = matches.front().second->front();
    layer_state->report = state_->report;
    if (state_->report.selected == Backend::FastGdb) {
        explorgdb::ReaderError status;
        auto reader = explorgdb::Reader::open(
            state_->source.normalized_uri, {}, &status);
        if (!reader) return reader_error(status);
        auto source_layer = reader->open_layer(selected_name, &status);
        if (!source_layer) {
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
            if (status.status ==
                    explorgdb::ReaderStatus::FidRangeUnsupported &&
                state_->options.backend == BackendPreference::Auto) {
                if (auto error = load_gdal_layer_schema(
                        layer_state,
                        FailureKind::UnsupportedFid)) {
                    return error;
                }
            } else {
                return reader_error(status);
            }
#else
            return reader_error(status);
#endif
        } else {
            auto schema = freeze_schema(
                *source_layer, layer_state->source_field_indices);
            if (schema) {
                layer_state->schema = std::move(schema).value();
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
            } else if (state_->options.backend ==
                       BackendPreference::Auto) {
                layer_state->source_field_indices.clear();
                if (auto error = load_gdal_layer_schema(
                        layer_state,
                        FailureKind::CapabilityGap)) {
                    return error;
                }
#endif
            } else {
                return schema.error();
            }
        }
    } else {
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
        if (auto error = load_gdal_layer_schema(
                layer_state, FailureKind::None)) {
            return error;
        }
#else
        return make_error(ErrorCode::BackendUnavailable,
                          "GDAL adapter is unavailable");
#endif
    }
    return Layer(std::move(layer_state));
}

Result<Layer> Dataset::open_layer_by_path(std::string_view path) const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Dataset is closed");
    if (!valid_canonical_path(path)) {
        return make_error(ErrorCode::InvalidUri, "invalid canonical layer path");
    }
    const std::string canonical(path);
    std::vector<std::pair<std::string, std::string>> matches;
    for (const auto& named_paths : state_->paths_by_name) {
        for (const auto& candidate : named_paths.second) {
            if (candidate == canonical) {
                matches.push_back({named_paths.first, candidate});
            }
        }
    }
    if (matches.empty()) {
        for (const auto& named_paths : state_->paths_by_name) {
            for (const auto& candidate : named_paths.second) {
                if (ascii_equal(candidate, canonical)) {
                    matches.push_back({named_paths.first, candidate});
                }
            }
        }
    }
    if (matches.empty()) {
        return make_error(ErrorCode::LayerNotFound,
                          "Layer path does not exist");
    }
    if (matches.size() > 1) {
        return make_error(ErrorCode::AmbiguousLayer,
                          "Layer path is ambiguous");
    }
    const auto& name = matches.front().first;
    const auto& selected_path = matches.front().second;
    auto selected = std::make_shared<detail::DatasetState>(*state_);
    selected->paths_by_name[name] = {selected_path};
    const auto original = state_->paths_by_name.find(name);
    if (state_->report.selected == Backend::FastGdb &&
        original != state_->paths_by_name.end() &&
        original->second.size() > 1) {
        if (state_->options.backend == BackendPreference::FastOnly) {
            return make_error(
                ErrorCode::Unsupported,
                "duplicate layer names require OpenFileGDB path resolution");
        }
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
        selected->report.selected = Backend::GdalOpenFileGDB;
        selected->report.reason = RouteReason::FastCapabilityGap;
        selected->report.fallback_reason = FailureKind::CapabilityGap;
#else
        return make_error(
            ErrorCode::BackendUnavailable,
            "duplicate layer names require OpenFileGDB path resolution");
#endif
    }
    return Dataset(std::move(selected)).open_layer(name);
}

Result<Group> Dataset::root_group() const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Dataset is closed");
    auto group = std::make_shared<detail::GroupState>();
    group->dataset = state_;
    group->path = "/";
    group->layers = state_->layers_by_group["/"];
    for (const auto& candidate : state_->groups) {
        if (parent_path(candidate.path) == "/") {
            group->groups.push_back(candidate);
        }
    }
    return Group(std::move(group));
}

const BackendReport& Dataset::backend_report() const noexcept {
    return state_ ? state_->report : kEmptyBackendReport;
}

const ConsistencyReport& Dataset::consistency_report() const noexcept {
    static const ConsistencyReport empty;
    return state_ ? state_->consistency : empty;
}

Group::Group(std::shared_ptr<detail::GroupState> state)
    : state_(std::move(state)) {}

Result<std::vector<GroupInfo>> Group::groups() const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Group is closed");
    return state_->groups;
}

Result<std::vector<LayerInfo>> Group::layers() const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Group is closed");
    return state_->layers;
}

Result<Group> Group::open_group(std::string_view name) const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Group is closed");
    const GroupInfo* found = nullptr;
    const auto exact = std::find_if(
        state_->groups.begin(), state_->groups.end(),
        [name](const GroupInfo& group) {
            return group.name == name;
        });
    if (exact != state_->groups.end()) {
        found = &*exact;
    } else {
        for (const auto& group : state_->groups) {
            if (!ascii_equal(group.name, name)) continue;
            if (found != nullptr) {
                return make_error(
                    ErrorCode::AmbiguousLayer,
                    "Group name is ambiguous under ASCII case folding");
            }
            found = &group;
        }
    }
    if (found == nullptr) {
        return make_error(ErrorCode::LayerNotFound, "Group does not exist");
    }
    auto child = std::make_shared<detail::GroupState>();
    child->dataset = state_->dataset;
    child->path = found->path;
    child->layers = state_->dataset->layers_by_group[found->path];
    for (const auto& candidate : state_->dataset->groups) {
        if (parent_path(candidate.path) == found->path) {
            child->groups.push_back(candidate);
        }
    }
    return Group(std::move(child));
}

}  // namespace unified
}  // namespace fast_gdb
