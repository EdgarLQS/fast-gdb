// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

#include "unified.h"

#include "ole_date.h"
#include "reader.h"
#include "gdb_tablx_cache.h"
#include "adaptive_reader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <list>
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

struct DatasetLifetime {
    std::mutex mutex;
    bool expired = false;
    std::size_t active_cursors = 0;
};

struct LayerState;

struct LayerStateSnapshot {
    std::string name;
    std::string path;
    std::shared_ptr<const LayerSchema> schema;
    std::vector<std::size_t> source_field_indices;
    std::vector<explorgdb::FieldDescriptor> source_fields;
    std::optional<LayerMetadataSnapshot> metadata;
    BackendReport report;
};

struct LayerStateCacheEntry {
    std::weak_ptr<LayerState> live_state;
    std::shared_ptr<const LayerStateSnapshot> snapshot;
    std::list<std::string>::iterator lru_position;
};

struct LayerStateCache {
    std::mutex mutex;
    std::list<std::string> lru;
    std::unordered_map<std::string, LayerStateCacheEntry> entries;
};

constexpr std::size_t kLayerStateSnapshotCacheCapacity = 64;

struct DatasetState {
    Source source;
    OpenOptions options;
    std::shared_ptr<DatasetLifetime> lifetime =
        std::make_shared<DatasetLifetime>();
    std::unordered_map<std::string, explorgdb::TablxCacheKey>
        source_snapshot;
    bool has_source_snapshot = false;
    BackendReport report;
    ConsistencyReport consistency;
    WorkspaceMetadataSnapshot metadata;
    std::vector<std::string> layer_names;
    std::vector<GroupInfo> groups;
    std::unordered_map<std::string, std::vector<LayerInfo>> layers_by_group;
    std::unordered_map<std::string, std::vector<std::string>> paths_by_name;
    std::shared_ptr<explorgdb::Reader> fast_reader;
    std::shared_ptr<LayerStateCache> layer_state_cache =
        std::make_shared<LayerStateCache>();
};

struct CursorRegistration {
    explicit CursorRegistration(std::shared_ptr<DatasetLifetime> owner)
        : lifetime(std::move(owner)) {}
    CursorRegistration(const CursorRegistration&) = delete;
    CursorRegistration& operator=(const CursorRegistration&) = delete;
    CursorRegistration(CursorRegistration&& other) noexcept
        : lifetime(std::move(other.lifetime)) {}
    CursorRegistration& operator=(CursorRegistration&& other) noexcept {
        if (this != &other) {
            release();
            lifetime = std::move(other.lifetime);
        }
        return *this;
    }
    ~CursorRegistration() { release(); }

    void release() noexcept {
        if (!lifetime) return;
        auto owner = std::move(lifetime);
        std::lock_guard<std::mutex> lock(owner->mutex);
        if (owner->active_cursors > 0) --owner->active_cursors;
    }

    std::shared_ptr<DatasetLifetime> lifetime;
};

struct LayerState {
    std::shared_ptr<DatasetState> dataset;
    std::string name;
    std::string path;
    std::shared_ptr<const LayerSchema> schema;
    std::vector<std::size_t> source_field_indices;
    std::vector<explorgdb::FieldDescriptor> source_fields;
    std::optional<LayerMetadataSnapshot> metadata;
    BackendReport report;
};

struct GroupState {
    std::shared_ptr<DatasetState> dataset;
    std::string path;
    std::vector<GroupInfo> groups;
    std::vector<LayerInfo> layers;
};

struct CursorState {
    // Reverse member destruction drops the Cursor and Layer parser before
    // releasing the generation lease that protects refresh and file access.
    std::optional<CursorRegistration> registration;
    std::optional<explorgdb::FastReaderLease> fast_lease;
    std::optional<explorgdb::Layer> layer;
    explorgdb::FeatureCursor cursor;
    std::shared_ptr<LayerState> layer_state;
    BackendReport report;
    ConsistencyReport consistency;
    QueryReport query_report;
    std::optional<Feature> prefetched;
    bool closed = false;
    bool include_geometry = true;
    std::function<bool()> cancel_requested;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    GeometryReadOptions geometry_options;
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
    GDALDataset* gdal_dataset = nullptr;
    OGRLayer* gdal_layer = nullptr;
    std::vector<std::string> projected_fields;
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

bool dataset_expired(
        const std::shared_ptr<detail::DatasetState>& state) noexcept {
    if (!state || !state->lifetime) return true;
    std::lock_guard<std::mutex> lock(state->lifetime->mutex);
    return state->lifetime->expired;
}

Error expired_dataset_error() {
    return make_error(
        ErrorCode::ReaderExpired,
        "Dataset snapshot expired; retry refresh or open a new Dataset");
}

bool capture_source_snapshot(
        const std::string& path,
        std::unordered_map<std::string, explorgdb::TablxCacheKey>& snapshot) {
    try {
        snapshot.clear();
        std::error_code error;
        std::filesystem::directory_iterator iterator(
            std::filesystem::u8path(path), error);
        if (error) return false;
        for (const auto& entry : iterator) {
            if (!entry.is_regular_file(error)) {
                if (error) return false;
                continue;
            }
            explorgdb::TablxCacheKey identity;
            if (!explorgdb::TablxCache::make_key(
                    entry.path().u8string(), identity)) {
                return false;
            }
            snapshot.emplace(entry.path().filename().u8string(), identity);
        }
        return !error;
    } catch (...) {
        return false;
    }
}

bool source_is_current(
        const std::shared_ptr<detail::DatasetState>& state) noexcept {
    if (!state) return false;
    if (!state->has_source_snapshot) return true;
    try {
        std::unordered_map<std::string, explorgdb::TablxCacheKey> current;
        return capture_source_snapshot(
                   state->source.normalized_uri, current) &&
               current == state->source_snapshot;
    } catch (...) {
        return false;
    }
}

void invalidate_tablx_cache(
        const std::unordered_map<std::string,
                                 explorgdb::TablxCacheKey>& snapshot) {
    constexpr std::string_view extension = ".gdbtablx";
    auto& cache = explorgdb::TablxCache::instance();
    for (const auto& entry : snapshot) {
        if (entry.first.size() >= extension.size() &&
            entry.first.compare(entry.first.size() - extension.size(),
                                extension.size(), extension) == 0) {
            cache.invalidate(entry.second);
        }
    }
}

Error changed_source_error() {
    return make_error(
        ErrorCode::SourceChanged,
        "GDB files changed since this Dataset snapshot; refresh before reading");
}

Result<detail::CursorRegistration> register_cursor(
        const std::shared_ptr<detail::DatasetState>& state) {
    if (!state || !state->lifetime) {
        return make_error(ErrorCode::OpenFailed, "Dataset is closed");
    }
    {
        std::lock_guard<std::mutex> lock(state->lifetime->mutex);
        if (state->lifetime->expired) return expired_dataset_error();
        ++state->lifetime->active_cursors;
    }
    return detail::CursorRegistration(state->lifetime);
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

Error reader_error(const explorgdb::ReaderError& error);

const explorgdb::MetadataTableAudit* find_audit(
        const std::vector<explorgdb::MetadataTableAudit>& audits,
        std::string_view table_name) {
    const auto found = std::find_if(
        audits.begin(), audits.end(), [table_name](const auto& audit) {
            return ascii_equal(audit.table_name, table_name);
        });
    return found == audits.end() ? nullptr : &*found;
}

MetadataStatus audit_status(const explorgdb::MetadataTableAudit* audit) {
    if (audit == nullptr || audit->status == "missing") {
        return MetadataStatus::Missing;
    }
    if (audit->status == "unreadable") return MetadataStatus::Unavailable;
    if (audit->status == "empty") return MetadataStatus::Empty;
    return MetadataStatus::Available;
}

MetadataStatus metadata_status(explorgdb::MetadataXmlStatus status) {
    switch (status) {
        case explorgdb::MetadataXmlStatus::Unavailable:
            return MetadataStatus::Unavailable;
        case explorgdb::MetadataXmlStatus::Missing:
            return MetadataStatus::Missing;
        case explorgdb::MetadataXmlStatus::Empty:
            return MetadataStatus::Empty;
        case explorgdb::MetadataXmlStatus::Available:
            return MetadataStatus::Available;
        case explorgdb::MetadataXmlStatus::Malformed:
            return MetadataStatus::Malformed;
        case explorgdb::MetadataXmlStatus::UnknownStructure:
            return MetadataStatus::UnknownStructure;
    }
    return MetadataStatus::Unavailable;
}

CatalogInventoryStatus catalog_inventory_status(
        explorgdb::CatalogInventoryStatus status) {
    switch (status) {
        case explorgdb::CatalogInventoryStatus::Unavailable:
            return CatalogInventoryStatus::Unavailable;
        case explorgdb::CatalogInventoryStatus::Empty:
            return CatalogInventoryStatus::Empty;
        case explorgdb::CatalogInventoryStatus::Complete:
            return CatalogInventoryStatus::Complete;
        case explorgdb::CatalogInventoryStatus::Partial:
            return CatalogInventoryStatus::Partial;
    }
    return CatalogInventoryStatus::Unavailable;
}

CatalogObjectLookupStatus catalog_object_status(
        explorgdb::CatalogObjectReadStatus status) {
    switch (status) {
        case explorgdb::CatalogObjectReadStatus::Available:
            return CatalogObjectLookupStatus::Available;
        case explorgdb::CatalogObjectReadStatus::Partial:
            return CatalogObjectLookupStatus::Partial;
        case explorgdb::CatalogObjectReadStatus::InvalidArgument:
            return CatalogObjectLookupStatus::InvalidArgument;
        case explorgdb::CatalogObjectReadStatus::NotFound:
            return CatalogObjectLookupStatus::NotFound;
        case explorgdb::CatalogObjectReadStatus::Ambiguous:
            return CatalogObjectLookupStatus::Ambiguous;
        case explorgdb::CatalogObjectReadStatus::Unavailable:
            return CatalogObjectLookupStatus::Unavailable;
    }
    return CatalogObjectLookupStatus::Unavailable;
}

std::string catalog_integrity_issue_code(
        explorgdb::CatalogIntegrityIssueCode code) {
    switch (code) {
        case explorgdb::CatalogIntegrityIssueCode::MissingRequiredField:
            return "missing_required_field";
        case explorgdb::CatalogIntegrityIssueCode::RowReadFailed:
            return "row_read_failed";
        case explorgdb::CatalogIntegrityIssueCode::RowShapeMismatch:
            return "row_shape_mismatch";
        case explorgdb::CatalogIntegrityIssueCode::EmptyUuid:
            return "empty_uuid";
        case explorgdb::CatalogIntegrityIssueCode::DuplicateUuid:
            return "duplicate_uuid";
    }
    return "unknown";
}

CatalogObjectSummaryMetadata public_catalog_object_summary(
        const explorgdb::CatalogObjectSummary& source) {
    CatalogObjectSummaryMetadata result;
    result.row_slot = source.row_slot;
    result.uuid = source.uuid;
    result.name = source.name;
    result.physical_name = source.physical_name;
    result.path = source.path;
    result.type = source.type;
    result.type_name = source.type_name;
    result.dataset_type = source.dataset_type;
    result.geometry_type = source.geometry_type;
    result.advanced_kind =
        static_cast<AdvancedObjectKind>(source.advanced_kind);
    result.semantic_status =
        static_cast<AdvancedObjectSemanticStatus>(source.semantic_status);
    result.physical_table_resolves = source.physical_table_resolves;
    result.definition_present = source.definition_present;
    result.definition_size = source.definition_size;
    return result;
}

CatalogIntegrityIssueMetadata public_catalog_integrity_issue(
        const explorgdb::CatalogIntegrityIssue& source) {
    return {source.row_slot, source.uuid,
            catalog_integrity_issue_code(source.code), source.diagnostic};
}

CatalogObjectMetadata public_catalog_object(
        const explorgdb::CatalogObject& source) {
    CatalogObjectMetadata result;
    result.summary = public_catalog_object_summary(source.summary);
    result.definition_present = source.definition_present;
    result.raw_definition = source.raw_definition;
    result.attributes.reserve(source.attributes.size());
    for (const auto& attribute : source.attributes) {
        CatalogObjectAttributeMetadata value;
        value.name = attribute.name;
        value.source_type = explorgdb::field_type_name(attribute.type);
        if (const auto* integer = std::get_if<std::int32_t>(&attribute.value)) {
            value.value = *integer;
        } else if (const auto* integer64 =
                       std::get_if<std::int64_t>(&attribute.value)) {
            value.value = *integer64;
        } else if (const auto* real = std::get_if<double>(&attribute.value)) {
            value.value = *real;
        } else if (const auto* offset =
                       std::get_if<explorgdb::DateTimeOffsetValue>(
                           &attribute.value)) {
            value.value = CatalogDateTimeOffset{
                offset->date, offset->offset_minutes};
        } else if (const auto* text =
                       std::get_if<std::string>(&attribute.value)) {
            value.value = *text;
        } else if (const auto* bytes =
                       std::get_if<std::vector<std::uint8_t>>(
                           &attribute.value)) {
            value.value = *bytes;
        } else {
            value.is_null = true;
        }
        result.attributes.push_back(std::move(value));
    }
    return result;
}

std::string audit_diagnostic(
        const explorgdb::MetadataTableAudit* audit,
        std::string_view table_name) {
    if (audit == nullptr) {
        return "metadata audit has no entry for " + std::string(table_name);
    }
    if (!audit->diagnostic.empty()) return audit->diagnostic;
    if (audit->status == "missing") {
        return std::string(table_name) + " is missing from GDB_SystemCatalog";
    }
    if (audit->status == "unreadable") {
        return std::string(table_name) + " could not be read";
    }
    return {};
}

DomainMetadata public_domain(const explorgdb::DomainInfo& source) {
    DomainMetadata value;
    value.name = source.name;
    value.type = source.type;
    value.field_type = source.field_type;
    value.description = source.description;
    value.merge_policy = source.merge_policy;
    value.split_policy = source.split_policy;
    value.min_value = source.min_value;
    value.max_value = source.max_value;
    value.raw_xml = source.raw_xml;
    value.coded_values.reserve(source.coded_values.size());
    for (const auto& coded : source.coded_values) {
        value.coded_values.push_back({coded.code, coded.name});
    }
    return value;
}

RelationshipClassMetadata public_relationship(
        const explorgdb::RelationshipClassInfo& source) {
    RelationshipClassMetadata value;
    value.uuid = source.uuid;
    value.name = source.name;
    value.path = source.path;
    value.type = source.type;
    value.origin_class_names = source.origin_class_names;
    value.origin_class_paths = source.origin_class_paths;
    value.destination_class_names = source.destination_class_names;
    value.destination_class_paths = source.destination_class_paths;
    value.cardinality = source.cardinality;
    value.notification = source.notification;
    value.forward_label = source.forward_path_label;
    value.backward_label = source.backward_path_label;
    value.key_type = source.key_type;
    value.class_key = source.class_key;
    value.is_attributed = source.is_attributed;
    value.is_composite = source.is_composite;
    value.is_containment = source.is_composite;
    value.is_reflexive = source.is_reflexive;
    value.is_attachment_relationship = source.is_attachment_relationship;
    value.change_tracked = source.change_tracked;
    value.replica_tracked = source.replica_tracked;
    value.raw_definition = source.raw_definition;
    for (const auto& key : source.keys) {
        value.keys.push_back({
            key.object_key_name, key.class_key_name, key.key_role});
        if (key.key_role == "esriRelKeyRoleOriginPrimary") {
            value.origin_primary_key = key.object_key_name;
        } else if (key.key_role == "esriRelKeyRoleOriginForeign") {
            value.destination_foreign_key = key.object_key_name;
        } else if (key.key_role == "esriRelKeyRoleDestinationPrimary") {
            value.destination_primary_key = key.object_key_name;
        } else if (key.key_role == "esriRelKeyRoleDestinationForeign") {
            value.origin_foreign_key = key.object_key_name;
        }
    }
    return value;
}

AttachmentRelationshipMetadata public_attachment_relationship(
        const explorgdb::AttachmentRelationshipInfo& source) {
    AttachmentRelationshipMetadata value;
    value.relationship_uuid = source.relationship_uuid;
    value.relationship_name = source.relationship_name;
    value.relationship_path = source.relationship_path;
    value.origin_class_uuid = source.origin_class_uuid;
    value.origin_class_name = source.origin_class_name;
    value.origin_class_path = source.origin_class_path;
    value.attachment_table_uuid = source.attachment_table_uuid;
    value.attachment_table_name = source.attachment_table_name;
    value.attachment_table_path = source.attachment_table_path;
    value.cardinality = source.cardinality;
    value.raw_definition = source.raw_definition;
    value.attachment_table_inferred = source.attachment_table_inferred;
    value.attachment_table_present = source.attachment_table_present;
    value.keys.reserve(source.keys.size());
    for (const auto& key : source.keys) {
        value.keys.push_back({
            key.object_key_name, key.class_key_name, key.key_role});
    }
    return value;
}

WorkspaceMetadataSnapshot public_workspace_metadata(
        const explorgdb::WorkspaceMetadataSnapshot& source) {
    WorkspaceMetadataSnapshot result;
    const auto* items_audit = find_audit(source.system_table_audit, "GDB_Items");
    const auto* relationships_audit = find_audit(
        source.system_table_audit, "GDB_ItemRelationships");

    result.domains.source = MetadataSource::FastGdb;
    const auto domains_table_status = audit_status(items_audit);
    result.domains.status = domains_table_status;
    if (domains_table_status == MetadataStatus::Available ||
        domains_table_status == MetadataStatus::Empty) {
        result.domains.status = metadata_status(source.domains_xml.status);
        result.domains.diagnostic = source.domains_xml.diagnostic;
        if (result.domains.status == MetadataStatus::Available &&
            source.domains.empty()) {
            result.domains.status = MetadataStatus::UnknownStructure;
            result.domains.diagnostic =
                "recognized domain XML produced no structured domain values";
        }
        result.domains.value.reserve(source.domains.size());
        for (const auto& domain : source.domains) {
            result.domains.value.push_back(public_domain(domain));
            if (!domain.raw_xml.empty()) {
                result.raw_payloads.push_back(
                    {"domain_definition", domain.name, domain.raw_xml, {}});
            }
        }
    } else {
        result.domains.diagnostic = audit_diagnostic(items_audit, "GDB_Items");
    }

    result.relationships.source = MetadataSource::FastGdb;
    const auto relationships_table_status = audit_status(items_audit);
    result.relationships.status = relationships_table_status;
    if (relationships_table_status == MetadataStatus::Available ||
        relationships_table_status == MetadataStatus::Empty) {
        result.relationships.status =
            metadata_status(source.relationship_classes_xml.status);
        result.relationships.diagnostic =
            source.relationship_classes_xml.diagnostic;
        if (result.relationships.status == MetadataStatus::Available &&
            source.relationship_classes.empty()) {
            result.relationships.status = MetadataStatus::UnknownStructure;
            result.relationships.diagnostic =
                "recognized relationship XML produced no structured definitions";
        }
    } else {
        result.relationships.diagnostic =
            audit_diagnostic(items_audit, "GDB_Items");
    }
    result.relationships.value.reserve(source.relationship_classes.size());
    for (const auto& relationship : source.relationship_classes) {
        result.relationships.value.push_back(public_relationship(relationship));
    }
    if (result.relationships.status == MetadataStatus::Available) {
        for (const auto& relationship : result.relationships.value) {
            if (!relationship.raw_definition.empty()) {
                result.raw_payloads.push_back({
                    "relationship_class_definition", relationship.uuid,
                    relationship.raw_definition, {}});
            }
        }
    }

    result.attachments.source = MetadataSource::FastGdb;
    result.attachments.status = relationships_table_status;
    if (relationships_table_status == MetadataStatus::Available ||
        relationships_table_status == MetadataStatus::Empty) {
        result.attachments.status =
            metadata_status(source.attachments_xml.status);
        result.attachments.diagnostic = source.attachments_xml.diagnostic;
        result.attachments.value.reserve(source.attachments.size());
        for (const auto& attachment : source.attachments) {
            result.attachments.value.push_back(
                public_attachment_relationship(attachment));
            if (!attachment.raw_definition.empty()) {
                result.raw_payloads.push_back({
                    "attachment_relationship_definition",
                    attachment.relationship_uuid,
                    attachment.raw_definition, {}});
            }
        }
    } else {
        result.attachments.diagnostic =
            audit_diagnostic(items_audit, "GDB_Items");
    }

    result.catalog_relationships.source = MetadataSource::FastGdb;
    const auto catalog_relationship_status = audit_status(relationships_audit);
    result.catalog_relationships.status = catalog_relationship_status;
    if (catalog_relationship_status == MetadataStatus::Available ||
        catalog_relationship_status == MetadataStatus::Empty) {
        for (const auto& relationship : source.relationships) {
            result.catalog_relationships.value.push_back({
                relationship.uuid, relationship.type_uuid,
                relationship.type_name, relationship.forward_label,
                relationship.backward_label, relationship.is_containment,
                relationship.origin_uuid, relationship.origin_name,
                relationship.origin_path, relationship.destination_uuid,
                relationship.destination_name, relationship.destination_path});
        }
        result.catalog_relationships.status =
            result.catalog_relationships.value.empty()
                ? MetadataStatus::Empty : MetadataStatus::Available;
    } else {
        result.catalog_relationships.diagnostic =
            audit_diagnostic(relationships_audit, "GDB_ItemRelationships");
    }

    result.feature_datasets.source = MetadataSource::FastGdb;
    result.feature_datasets.status = audit_status(items_audit);
    if (result.feature_datasets.status == MetadataStatus::Available ||
        result.feature_datasets.status == MetadataStatus::Empty) {
        result.feature_datasets.status =
            metadata_status(source.feature_datasets_xml.status);
        result.feature_datasets.diagnostic =
            source.feature_datasets_xml.diagnostic;
        result.feature_datasets.value.reserve(source.dataset_groups.size());
        for (const auto& group : source.dataset_groups) {
            FeatureDatasetMetadata dataset;
            dataset.path = canonical_path(group.group_path);
            dataset.name = path_name(dataset.path);
            if (!group.raw_definition.empty()) {
                result.raw_payloads.push_back({
                    "feature_dataset_definition", dataset.path,
                    group.raw_definition, {}});
            }
            for (std::size_t index = 0; index < group.member_names.size(); ++index) {
                const std::string member_path = index < group.member_paths.size()
                    ? canonical_path(group.member_paths[index])
                    : dataset.path + "/" + group.member_names[index];
                dataset.members.push_back(
                    {group.member_names[index], member_path});
                if (index < group.member_raw_definitions.size() &&
                    !group.member_raw_definitions[index].empty()) {
                    result.raw_payloads.push_back({
                        "feature_dataset_member_definition", member_path,
                        group.member_raw_definitions[index], {}});
                }
            }
            result.feature_datasets.value.push_back(std::move(dataset));
        }
        if (result.feature_datasets.status == MetadataStatus::Available &&
            result.feature_datasets.value.empty()) {
            result.feature_datasets.status = MetadataStatus::UnknownStructure;
            result.feature_datasets.diagnostic =
                "recognized Feature Dataset XML produced no group summaries";
        }
    } else {
        result.feature_datasets.diagnostic =
            audit_diagnostic(items_audit, "GDB_Items");
    }

    result.system_table_audit.source = MetadataSource::FastGdb;
    result.system_table_audit.status = source.system_table_audit.empty()
        ? MetadataStatus::Unavailable : MetadataStatus::Available;
    result.system_table_audit.value.reserve(source.system_table_audit.size());
    for (const auto& audit : source.system_table_audit) {
        result.system_table_audit.value.push_back({
            audit.table_name, audit.status, audit.diagnostic,
            audit.row_count, audit.digest});
    }

    result.catalog_objects.source = MetadataSource::FastGdb;
    result.catalog_objects.status =
        catalog_inventory_status(source.catalog_objects.status);
    result.catalog_objects.diagnostic = source.catalog_objects.diagnostic;
    result.catalog_objects.physical_row_slots =
        source.catalog_objects.physical_row_slots;
    result.catalog_objects.active_rows = source.catalog_objects.active_rows;
    result.catalog_objects.decoded_rows = source.catalog_objects.decoded_rows;
    result.catalog_objects.objects.reserve(source.catalog_objects.objects.size());
    for (const auto& object : source.catalog_objects.objects) {
        result.catalog_objects.objects.push_back(
            public_catalog_object_summary(object));
    }
    result.catalog_objects.issues.reserve(source.catalog_objects.issues.size());
    for (const auto& issue : source.catalog_objects.issues) {
        result.catalog_objects.issues.push_back(
            public_catalog_integrity_issue(issue));
    }
    return result;
}

LayerMetadataSnapshot public_layer_metadata(
        const explorgdb::LayerMetadataSnapshot& source,
        std::string path) {
    LayerMetadataSnapshot result;
    result.name = source.name;
    result.path = std::move(path);
    result.definition.source = MetadataSource::FastGdb;
    if (source.layer) {
        const auto& layer = *source.layer;
        result.definition.status = metadata_status(source.definition_xml.status);
        result.definition.diagnostic = source.definition_xml.diagnostic;
        result.definition.value = {
            layer.name, layer.path, layer.physical_name, layer.uuid, layer.type,
            layer.catalog_path, layer.dataset_type, layer.items};
        if (layer.definition_present) {
            result.raw_payloads.push_back(
                {"layer_definition", layer.name, layer.definition, {}});
        }
        if (!layer.documentation.empty()) {
            result.raw_payloads.push_back(
                {"layer_documentation", layer.name, layer.documentation, {}});
        }
        for (const auto& item : layer.items) {
            const auto raw = layer.raw_items.find(item.first);
            if (raw == layer.raw_items.end()) continue;
            result.raw_payloads.push_back(
                {"metadata_item", item.first, item.second, raw->second});
        }
    } else {
        result.definition.status = MetadataStatus::Missing;
        result.definition.diagnostic = "layer item is missing from GDB_Items";
    }

    result.field_domains.source = MetadataSource::FastGdb;
    if (source.layer) {
        result.field_domains.status = metadata_status(
            source.field_domains_xml.status);
        result.field_domains.diagnostic = source.field_domains_xml.diagnostic;
    } else {
        result.field_domains.status = MetadataStatus::Missing;
    }
    result.field_domains.value.reserve(source.field_domains.size());
    for (const auto& binding : source.field_domains) {
        FieldDomainBinding value;
        value.field_name = binding.field_name;
        value.domain_name = binding.domain_name;
        if (binding.domain) value.domain = public_domain(*binding.domain);
        result.field_domains.value.push_back(std::move(value));
    }

    result.subtypes.source = MetadataSource::FastGdb;
    if (source.layer) {
        result.subtypes.status = metadata_status(source.subtypes_xml.status);
        result.subtypes.diagnostic = source.subtypes_xml.diagnostic;
    } else {
        result.subtypes.status = MetadataStatus::Missing;
    }
    result.subtypes.value.reserve(source.subtypes.size());
    for (const auto& subtype : source.subtypes) {
        SubtypeMetadata value;
        value.code = subtype.code;
        value.name = subtype.name;
        if (!subtype.default_value.empty()) {
            value.default_value = subtype.default_value;
        }
        value.raw_xml = subtype.raw_xml;
        value.field_overrides.reserve(subtype.field_overrides.size());
        for (const auto& field : subtype.field_overrides) {
            SubtypeFieldOverride override_value;
            override_value.field_name = field.field_name;
            override_value.default_value = field.default_value;
            override_value.domain_name = field.domain_name;
            if (field.domain) {
                override_value.domain = public_domain(*field.domain);
            }
            value.field_overrides.push_back(std::move(override_value));
        }
        if (!value.raw_xml.empty()) {
            result.raw_payloads.push_back(
                {"subtype_definition", std::to_string(value.code),
                 value.raw_xml, {}});
        }
        result.subtypes.value.push_back(std::move(value));
    }

    result.relationships.source = MetadataSource::FastGdb;
    const auto* items_audit = find_audit(
        source.system_table_audit, "GDB_Items");
    result.relationships.status = audit_status(items_audit);
    if (result.relationships.status == MetadataStatus::Available ||
        result.relationships.status == MetadataStatus::Empty) {
        result.relationships.status = metadata_status(
            source.relationship_classes_xml.status);
        result.relationships.diagnostic =
            source.relationship_classes_xml.diagnostic;
    } else {
        result.relationships.diagnostic =
            audit_diagnostic(items_audit, "GDB_Items");
    }
    for (const auto& relationship : source.relationship_classes) {
        if (!source.layer ||
            (std::none_of(
                 relationship.origin_class_names.begin(),
                 relationship.origin_class_names.end(),
                 [&source](const std::string& name) {
                     return ascii_equal(name, source.name);
                 }) &&
             std::none_of(
                 relationship.destination_class_names.begin(),
                 relationship.destination_class_names.end(),
                 [&source](const std::string& name) {
                     return ascii_equal(name, source.name);
                 }))) {
            continue;
        }
        result.relationships.value.push_back(public_relationship(relationship));
    }
    if (!result.relationships.value.empty() &&
        result.relationships.status != MetadataStatus::Malformed &&
        result.relationships.status != MetadataStatus::UnknownStructure) {
        result.relationships.status = MetadataStatus::Available;
    } else if (result.relationships.value.empty() &&
               result.relationships.status == MetadataStatus::Available) {
        result.relationships.status = MetadataStatus::Empty;
    }
    result.attachments.source = MetadataSource::FastGdb;
    result.attachments.status = source.layer
        ? metadata_status(source.attachments_xml.status)
        : MetadataStatus::Missing;
    result.attachments.diagnostic = source.attachments_xml.diagnostic;
    for (const auto& attachment : source.attachments) {
        if (!source.layer ||
            (!ascii_equal(attachment.origin_class_name, source.name) &&
             !ascii_equal(attachment.attachment_table_name, source.name))) {
            continue;
        }
        result.attachments.value.push_back(
            public_attachment_relationship(attachment));
        if (!attachment.raw_definition.empty()) {
            result.raw_payloads.push_back({
                "attachment_relationship_definition",
                attachment.relationship_uuid, attachment.raw_definition, {}});
        }
    }
    if (result.attachments.status == MetadataStatus::Available &&
        result.attachments.value.empty()) {
        result.attachments.status = MetadataStatus::Empty;
        result.attachments.diagnostic.clear();
    }
    return result;
}

Error populate_fast_groups(detail::DatasetState& state,
                           explorgdb::Reader& reader) {
    std::set<std::string> grouped_names;
    const auto metadata = reader.read_workspace_metadata();
    if (!metadata.ok()) return reader_error(metadata.error);
    state.metadata = public_workspace_metadata(metadata.snapshot);
    for (const auto& group : metadata.snapshot.dataset_groups) {
        const auto group_path = canonical_path(group.group_path);
        index_group(state, group_path);
        for (std::size_t i = 0; i < group.member_names.size(); ++i) {
            if (std::find(state.layer_names.begin(),
                          state.layer_names.end(),
                          group.member_names[i]) == state.layer_names.end()) {
                continue;
            }
            const auto path = i < group.member_paths.size()
                ? group.member_paths[i]
                : group_path + "/" + group.member_names[i];
            index_group_layer(state, group.member_names[i], path);
            grouped_names.insert(group.member_names[i]);
        }
    }
    for (const auto& name : state.layer_names) {
        if (grouped_names.find(name) == grouped_names.end()) {
            index_group_layer(state, name, "/" + name);
        }
    }
    return {};
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

using LayerDefinitionDefaults = explorgdb::LayerDefinitionDefaults;

LayerDefinitionDefaults parse_layer_definition_defaults(
        const std::optional<explorgdb::LayerMetadata>& metadata) {
    return metadata
        ? explorgdb::MetadataReader::decode_layer_definition_defaults(
              metadata->definition)
        : LayerDefinitionDefaults{};
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
    using Bits = std::conditional_t<sizeof(T) == 2, std::uint16_t,
        std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>;
    Bits bits = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bits |= static_cast<Bits>(bytes[i]) << (i * 8U);
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
        if (text.find('.') == std::string::npos) text += ".000";
        zone << (offset >= 0 ? '+' : '-')
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
    if (descriptor.type == explorgdb::FieldType::String ||
        descriptor.type == explorgdb::FieldType::XML) {
        if (descriptor.default_value_raw.empty()) return std::nullopt;
        return quote_sql_string(descriptor.default_value_raw);
    }
    if (descriptor.type == explorgdb::FieldType::DateTime ||
        descriptor.type == explorgdb::FieldType::Date ||
        descriptor.type == explorgdb::FieldType::Time ||
        descriptor.type == explorgdb::FieldType::DateTimeWithOffset) {
        return date_default(descriptor);
    }
    switch (descriptor.type) {
        case explorgdb::FieldType::Int16:
            if (const auto value = read_little_endian_scalar<std::int16_t>(
                    descriptor.default_value_raw))
                return std::to_string(*value);
            break;
        case explorgdb::FieldType::Int32:
            if (const auto value = read_little_endian_scalar<std::int32_t>(
                    descriptor.default_value_raw))
                return std::to_string(*value);
            break;
        case explorgdb::FieldType::Int64:
            if (const auto value = read_little_endian_scalar<std::int64_t>(
                    descriptor.default_value_raw))
                return std::to_string(*value);
            break;
        case explorgdb::FieldType::Float32:
            if (const auto value = read_little_endian_scalar<float>(
                    descriptor.default_value_raw)) {
                std::ostringstream stream;
                stream << std::setprecision(
                    std::numeric_limits<float>::max_digits10) << *value;
                return stream.str();
            }
            break;
        case explorgdb::FieldType::Float64:
            if (const auto value = read_little_endian_scalar<double>(
                    descriptor.default_value_raw)) {
                std::ostringstream stream;
                stream << std::setprecision(
                    std::numeric_limits<double>::max_digits10) << *value;
                return stream.str();
            }
            break;
        default:
            break;
    }
    return numeric_xml_default(defaults, descriptor.name);
}

Result<LayerSchema> freeze_schema(
        explorgdb::Layer& layer,
        std::vector<std::size_t>& indices,
        explorgdb::LayerMetadataSnapshot* metadata_snapshot = nullptr) {
    LayerSchema schema;
    schema.name = layer.name();
    const auto metadata = layer.read_metadata();
    if (!metadata.ok()) return reader_error(metadata.error);
    if (metadata_snapshot != nullptr) {
        *metadata_snapshot = metadata.snapshot;
    }
    const auto defaults =
        parse_layer_definition_defaults(metadata.snapshot.layer);
    const auto& descriptors = layer.fields();
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
        const auto& descriptor = descriptors[i];
        if (descriptor.type == explorgdb::FieldType::Geometry) {
            schema.geometry_field = 0;
            // 跳过 GUID 形式的未知空间参考（如 {B286C06B-0879-11D2-AACA-00C04FA33C20}），
            // 使其与 OpenFileGDB 的 null 空间参考行为一致。仅保留有效 WKT。
            if (!descriptor.wkt.empty() && descriptor.wkt.front() != '{') {
                schema.srs_wkt = descriptor.wkt;
            }
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

bool same_fast_field_schema(
        const std::vector<explorgdb::FieldDescriptor>& frozen,
        const std::vector<explorgdb::FieldDescriptor>& current) {
    if (frozen.size() != current.size()) return false;
    for (std::size_t index = 0; index < frozen.size(); ++index) {
        const auto& lhs = frozen[index];
        const auto& rhs = current[index];
        if (lhs.name != rhs.name || lhs.alias != rhs.alias ||
            lhs.type != rhs.type || lhs.width != rhs.width ||
            lhs.flag != rhs.flag || lhs.default_value != rhs.default_value ||
            lhs.default_value_raw != rhs.default_value_raw ||
            lhs.wkt != rhs.wkt || lhs.xorig != rhs.xorig ||
            lhs.yorig != rhs.yorig || lhs.xyscale != rhs.xyscale ||
            lhs.xytolerance != rhs.xytolerance ||
            lhs.zorig != rhs.zorig || lhs.zscale != rhs.zscale ||
            lhs.ztolerance != rhs.ztolerance ||
            lhs.morig != rhs.morig || lhs.mscale != rhs.mscale ||
            lhs.mtolerance != rhs.mtolerance ||
            lhs.grid_sizes != rhs.grid_sizes) {
            return false;
        }
    }
    return true;
}

Error changed_layer_error(
        const std::shared_ptr<detail::LayerState>& state) {
    if (!state || state->report.selected != Backend::FastGdb) {
        return changed_source_error();
    }
    explorgdb::ReaderError status;
    auto reader = explorgdb::Reader::open(
        state->dataset->source.normalized_uri, {}, &status);
    if (!reader) return reader_error(status);
    auto layer = reader->open_layer(state->name, &status);
    if (!layer) return reader_error(status);
    if (!same_fast_field_schema(state->source_fields, layer->fields())) {
        return make_error(
            ErrorCode::SchemaMismatch,
            "Reader field schema differs from the frozen Layer schema");
    }
    return changed_source_error();
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
    geometry.source_payload = source.source_payload;
    geometry.multipatch = source.multipatch;
    geometry.source_was_curve = source.source_was_curve;
    geometry.linearized = source.linearized;
    geometry.linearization_max_chord_error =
        source.linearization_max_chord_error;
    geometry.linearization_max_angle_step_degrees =
        source.linearization_max_angle_step_degrees;
    geometry.linearization_max_segments_per_curve =
        source.linearization_max_segments_per_curve;
    geometry.diagnostic = source.diagnostic;
    switch (source.backend) {
        case explorgdb::GeometryBackend::FastGdb:
            geometry.backend = GeometryBackend::FastGdb;
            break;
        case explorgdb::GeometryBackend::BuiltinCurve:
            geometry.backend = GeometryBackend::BuiltinCurve;
            break;
        case explorgdb::GeometryBackend::Gdal:
            geometry.backend = GeometryBackend::Gdal;
            break;
        case explorgdb::GeometryBackend::Reject:
            geometry.backend = GeometryBackend::Reject;
            break;
    }
    switch (source.source_payload_state) {
        case explorgdb::GeometrySourcePayloadState::NotPresent:
            geometry.source_payload_state =
                GeometrySourcePayloadState::NotPresent;
            break;
        case explorgdb::GeometrySourcePayloadState::Preserved:
            geometry.source_payload_state =
                GeometrySourcePayloadState::Preserved;
            break;
        case explorgdb::GeometrySourcePayloadState::LimitExceeded:
            geometry.source_payload_state =
                GeometrySourcePayloadState::LimitExceeded;
            break;
    }
    if (source.status == explorgdb::GeometryStatus::NotRequested) {
        geometry.state = GeometryState::NotRequested;
    } else if (source.status == explorgdb::GeometryStatus::Null) {
        geometry.state = GeometryState::Null;
    } else if (source.status == explorgdb::GeometryStatus::Empty) {
        geometry.state = GeometryState::Empty;
    } else if (source.status == explorgdb::GeometryStatus::Valid) {
        geometry.state = source.wkb.empty()
            ? GeometryState::Null : GeometryState::Value;
    } else if (source.source_payload_state !=
               explorgdb::GeometrySourcePayloadState::NotPresent) {
        geometry.state = GeometryState::Unsupported;
    } else {
        geometry.state = GeometryState::Unset;
    }
    return geometry;
}

Result<Feature> map_feature(const explorgdb::QueryFeature& source,
                            const detail::LayerState& state,
                            const explorgdb::Layer& layer) {
    if (!same_fast_field_schema(state.source_fields, layer.fields()) ||
        !state.schema ||
        state.source_field_indices.size() != state.schema->fields.size()) {
        return make_error(
            ErrorCode::SchemaMismatch,
            "Reader schema differs from the frozen Layer schema");
    }
    if (source.record.materialized_fields.empty() &&
        source.record.field_values.size() != layer.fields().size()) {
        return make_error(
            ErrorCode::ReadFailed,
            "Reader returned a record with a mismatched field count");
    }
    if (!source.record.materialized_fields.empty() &&
        source.record.materialized_fields.size() != layer.fields().size()) {
        return make_error(
            ErrorCode::ReadFailed,
            "Reader returned an invalid field materialization map");
    }
    Feature feature;
    feature.fid = static_cast<Fid>(source.fid) + 1;
    feature.fields.reserve(state.source_field_indices.size());
    const auto& descriptors = layer.fields();
    for (const auto source_index : state.source_field_indices) {
        if (source_index >= descriptors.size() ||
            (!source.record.materialized_fields.empty() &&
             source.record.materialized_fields[source_index] != 0 &&
             source_index >= source.record.field_values.size())) {
            return make_error(
                ErrorCode::ReadFailed,
                "Reader field mapping is outside the returned record");
        }
        feature.fields.push_back(
            map_field(descriptors[source_index], source.record, source_index));
    }
    feature.geometry = map_geometry(source.geometry);
    feature.geometry.srs_wkt = state.schema->srs_wkt;
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
                state.schema->fields.begin(), state.schema->fields.end(),
                [&name](const FieldDefinition& field) {
                    return field.name == name;
                });
            if (found == state.schema->fields.end()) {
                return make_error(ErrorCode::Unsupported,
                                  "projected field does not exist: " + name);
            }
            const auto public_index = static_cast<std::size_t>(
                std::distance(state.schema->fields.begin(), found));
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
    request.include_geometry = query.include_geometry;
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
        const Query& query,
        detail::CursorRegistration registration) {
    auto lease_result = acquire_fast_reader_lease(
        state->dataset->source.normalized_uri);
    if (!lease_result) return lease_result.error();
    auto lease = std::move(lease_result).value();
    if (lease.generation() != state->dataset->consistency.generation) {
        return make_error(
            ErrorCode::SourceChanged,
            "Dataset generation expired; refresh the Dataset and reopen the Layer");
    }
    if (!state->dataset->fast_reader) {
        return make_error(ErrorCode::OpenFailed,
                          "cached FastGDB Reader snapshot is unavailable");
    }
    explorgdb::ReaderError reader_status;
    auto layer = state->dataset->fast_reader->open_layer(
        state->name, &reader_status);
    if (!layer) return reader_error(reader_status);
    if (!same_fast_field_schema(state->source_fields, layer->fields())) {
        return make_error(
            ErrorCode::SchemaMismatch,
            "Reader field schema differs from the frozen Layer schema");
    }
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
    switch (query.geometry.curve_policy) {
        case CurveReadPolicy::PreserveSource:
            request.geometry_read_options.curve_policy =
                explorgdb::CurveReadPolicy::PreserveSource;
            break;
        case CurveReadPolicy::Linearize:
            request.geometry_read_options.curve_policy =
                explorgdb::CurveReadPolicy::Linearize;
            break;
        case CurveReadPolicy::Reject:
            request.geometry_read_options.curve_policy =
                explorgdb::CurveReadPolicy::Reject;
            break;
    }
    request.geometry_read_options.curve_linearization = {
        query.geometry.max_chord_error,
        query.geometry.max_angle_step_degrees,
        query.geometry.max_segments_per_curve == 0
            ? 4096U
            : static_cast<std::size_t>(
                  query.geometry.max_segments_per_curve)};
    request.geometry_read_options.max_source_payload_bytes =
        static_cast<std::size_t>(std::min<std::uint64_t>(
            query.geometry.max_source_payload_bytes,
            std::numeric_limits<std::size_t>::max()));
    request.geometry_read_options.preserve_source_payload =
        query.geometry.preserve_source_payload;
    auto cursor = layer->open_cursor(request);
    if (cursor.query_result().status != explorgdb::QueryStatus::Ok) {
        return make_error(
            query_error_code(cursor.query_result().status, query.deadline),
            cursor.query_result().error);
    }
    auto cursor_state = std::make_unique<detail::CursorState>();
    cursor_state->registration.emplace(std::move(registration));
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
    cursor_state->query_report.query_preparation_cache_hit =
        cursor_state->cursor.query_result().query_preparation_cache_hit;

    explorgdb::QueryFeature prefetched;
    if (cursor_state->cursor.next(prefetched)) {
        auto feature = map_feature(
            prefetched, *state, *cursor_state->layer);
        if (!feature) return feature.error();
        cursor_state->prefetched = std::move(feature).value();
    } else if (!cursor_state->cursor.error().empty()) {
        return make_error(query_error_code(
                cursor_state->cursor.query_result().status, query.deadline),
            cursor_state->cursor.error());
    }
    return FeatureCursor(std::move(cursor_state));
}

Result<Feature> read_fast_feature_by_fid(
        const std::shared_ptr<detail::LayerState>& state, Fid fid) {
    explorgdb::RowSlot row_slot = 0;
    if (fid <= 0 || !explorgdb::object_id_to_row_slot(
            static_cast<explorgdb::ObjectId>(fid), row_slot)) {
        return make_error(ErrorCode::Unsupported,
                          "FID cannot be mapped to a fast row slot");
    }

    auto lease_result = acquire_fast_reader_lease(
        state->dataset->source.normalized_uri);
    if (!lease_result) return lease_result.error();
    auto lease = std::move(lease_result).value();
    if (lease.generation() != state->dataset->consistency.generation) {
        return make_error(
            ErrorCode::SourceChanged,
            "Dataset generation expired; refresh the Dataset and reopen the Layer");
    }

    if (!state->dataset->fast_reader) {
        return make_error(ErrorCode::OpenFailed,
                          "cached FastGDB Reader snapshot is unavailable");
    }
    explorgdb::ReaderError reader_status;
    auto layer = state->dataset->fast_reader->open_layer(
        state->name, &reader_status);
    if (!layer) return reader_error(reader_status);
    if (!same_fast_field_schema(state->source_fields, layer->fields())) {
        return make_error(
            ErrorCode::SchemaMismatch,
            "Reader field schema differs from the frozen Layer schema");
    }

    explorgdb::QueryRequest request;
    request.kind = explorgdb::QueryKind::ReadByFid;
    request.fid = row_slot;
    auto cursor = layer->open_cursor(request);
    if (cursor.query_result().status != explorgdb::QueryStatus::Ok) {
        const ErrorCode code =
            cursor.query_result().status == explorgdb::QueryStatus::SourceChanged
                ? ErrorCode::SourceChanged
                : ErrorCode::ReadFailed;
        return make_error(code, cursor.query_result().error);
    }
    if (cursor.query_result().matched_fids.empty()) {
        return make_error(ErrorCode::FeatureNotFound, "FID does not exist");
    }

    explorgdb::QueryFeature source;
    if (!cursor.next(source)) {
        return make_error(
            ErrorCode::ReadFailed,
            cursor.error().empty()
                ? "Reader could not materialize the requested FID"
                : cursor.error());
    }
    return map_feature(source, *state, *layer);
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

std::string gdal_field_value(const OGRField& value, OGRFieldType type) {
    if (OGR_RawField_IsUnset(&value)) return {};
    switch (type) {
        case OFTInteger: return std::to_string(value.Integer);
        case OFTInteger64: return std::to_string(value.Integer64);
        case OFTReal: {
            std::ostringstream stream;
            stream << std::setprecision(
                std::numeric_limits<double>::max_digits10) << value.Real;
            return stream.str();
        }
        case OFTString:
            return value.String == nullptr ? std::string{} : value.String;
        default:
            return {};
    }
}

std::string gdal_domain_type(OGRFieldDomainType type) {
    switch (type) {
        case OFDT_CODED: return "coded";
        case OFDT_RANGE: return "range";
        case OFDT_GLOB: return "glob";
    }
    return "unknown";
}

std::string gdal_domain_split_policy(OGRFieldDomainSplitPolicy policy) {
    switch (policy) {
        case OFDSP_DEFAULT_VALUE: return "default_value";
        case OFDSP_DUPLICATE: return "duplicate";
        case OFDSP_GEOMETRY_RATIO: return "geometry_ratio";
    }
    return "unknown";
}

std::string gdal_domain_merge_policy(OGRFieldDomainMergePolicy policy) {
    switch (policy) {
        case OFDMP_DEFAULT_VALUE: return "default_value";
        case OFDMP_SUM: return "sum";
        case OFDMP_GEOMETRY_WEIGHTED: return "geometry_weighted";
    }
    return "unknown";
}

DomainMetadata public_domain(const OGRFieldDomain& source) {
    DomainMetadata value;
    value.name = source.GetName();
    value.type = gdal_domain_type(source.GetDomainType());
    value.field_type = OGRFieldDefn::GetFieldTypeName(source.GetFieldType());
    value.description = source.GetDescription();
    value.merge_policy = gdal_domain_merge_policy(source.GetMergePolicy());
    value.split_policy = gdal_domain_split_policy(source.GetSplitPolicy());
    if (const auto* coded = dynamic_cast<const OGRCodedFieldDomain*>(&source)) {
        for (const OGRCodedValue* item = coded->GetEnumeration();
             item != nullptr && item->pszCode != nullptr; ++item) {
            value.coded_values.push_back({
                item->pszCode, item->pszValue == nullptr ? "" : item->pszValue});
        }
    } else if (const auto* range =
                   dynamic_cast<const OGRRangeFieldDomain*>(&source)) {
        bool inclusive = false;
        value.min_value = gdal_field_value(
            range->GetMin(inclusive), source.GetFieldType());
        value.max_value = gdal_field_value(
            range->GetMax(inclusive), source.GetFieldType());
    } else if (const auto* glob =
                   dynamic_cast<const OGRGlobFieldDomain*>(&source)) {
        value.raw_xml = glob->GetGlob();
    }
    return value;
}

std::string join_names(const std::vector<std::string>& names) {
    std::string result;
    for (const auto& name : names) {
        if (!result.empty()) result += ",";
        result += name;
    }
    return result;
}

std::string gdal_relationship_cardinality(
        GDALRelationshipCardinality cardinality) {
    switch (cardinality) {
        case GRC_ONE_TO_ONE: return "one-to-one";
        case GRC_ONE_TO_MANY: return "one-to-many";
        case GRC_MANY_TO_ONE: return "many-to-one";
        case GRC_MANY_TO_MANY: return "many-to-many";
    }
    return "unknown";
}

std::string gdal_relationship_type(GDALRelationshipType type) {
    switch (type) {
        case GRT_COMPOSITE: return "composite";
        case GRT_ASSOCIATION: return "association";
        case GRT_AGGREGATION: return "aggregation";
    }
    return "unknown";
}

bool driver_supports_metadata(GDALDataset& dataset,
                              const char* capability) {
    auto* driver = const_cast<GDALDriver*>(dataset.GetDriver());
    const char* value = driver == nullptr
        ? nullptr : driver->GetMetadataItem(capability);
    return value != nullptr && CPLTestBool(value) != 0;
}

void append_gdal_raw_metadata(GDALMajorObject& object,
                              std::string kind,
                              std::string identifier,
                              std::vector<RawMetadataPayload>& output) {
    const auto append_entries = [&kind, &identifier, &output](
                                    std::string domain,
                                    CSLConstList items) {
        for (int index = 0; index < CSLCount(items); ++index) {
            const char* entry = CSLGetField(items, index);
            if (entry == nullptr) continue;
            output.push_back({kind + ":" + domain, identifier, entry, {}});
        }
    };
    append_entries("default", object.GetMetadata());
    char** domains = object.GetMetadataDomainList();
    if (domains == nullptr) return;
    for (int domain_index = 0; domain_index < CSLCount(domains); ++domain_index) {
        const char* domain = CSLGetField(domains, domain_index);
        if (domain == nullptr || *domain == '\0') continue;
        append_entries(domain, object.GetMetadata(domain));
    }
    CSLDestroy(domains);
}

WorkspaceMetadataSnapshot public_gdal_workspace_metadata(
        GDALDataset& dataset,
        const detail::DatasetState& state) {
    WorkspaceMetadataSnapshot result;
    const auto domain_names = dataset.GetFieldDomainNames();
    const bool supports_domains = driver_supports_metadata(
        dataset, GDAL_DCAP_FIELD_DOMAINS);
    result.domains.source = MetadataSource::GdalOpenFileGDB;
    result.domains.status = domain_names.empty()
        ? (supports_domains ? MetadataStatus::Empty
                            : MetadataStatus::Unsupported)
        : MetadataStatus::Available;
    if (result.domains.status == MetadataStatus::Unsupported) {
        result.domains.diagnostic =
            "OpenFileGDB does not advertise field-domain reading for this source";
    }
    for (const auto& name : domain_names) {
        const OGRFieldDomain* domain = dataset.GetFieldDomain(name);
        if (domain == nullptr) continue;
        result.domains.value.push_back(public_domain(*domain));
    }
    if (result.domains.status == MetadataStatus::Available &&
        result.domains.value.empty()) {
        result.domains.status = MetadataStatus::Empty;
    }

    const auto relationship_names = dataset.GetRelationshipNames();
    const bool supports_relationships = driver_supports_metadata(
        dataset, GDAL_DCAP_RELATIONSHIPS);
    result.relationships.source = MetadataSource::GdalOpenFileGDB;
    result.relationships.status = relationship_names.empty()
        ? (supports_relationships ? MetadataStatus::Empty
                                  : MetadataStatus::Unsupported)
        : MetadataStatus::Available;
    if (result.relationships.status == MetadataStatus::Unsupported) {
        result.relationships.diagnostic =
            "OpenFileGDB does not advertise relationship reading for this source";
    } else {
        result.relationships.diagnostic =
            "GDAL relationship records do not expose FileGDB UUIDs or raw XML attributes";
    }
    for (const auto& name : relationship_names) {
        const auto* relationship = dataset.GetRelationship(name);
        if (relationship == nullptr) continue;
        RelationshipClassMetadata value;
        value.name = name;
        value.type =
            gdal_relationship_type(relationship->GetType());
        value.origin_class_names.push_back(relationship->GetLeftTableName());
        value.origin_class_paths.push_back(relationship->GetLeftTableName());
        value.destination_class_names.push_back(relationship->GetRightTableName());
        value.destination_class_paths.push_back(relationship->GetRightTableName());
        value.cardinality =
            gdal_relationship_cardinality(relationship->GetCardinality());
        const auto& left_fields = relationship->GetLeftTableFields();
        const auto& right_fields = relationship->GetRightTableFields();
        value.origin_primary_key = join_names(left_fields);
        value.destination_foreign_key = join_names(right_fields);
        const auto pair_count = std::min(left_fields.size(), right_fields.size());
        for (std::size_t index = 0; index < pair_count; ++index) {
            value.keys.push_back({
                left_fields[index], right_fields[index], "gdal_left_right_pair"});
        }
        for (std::size_t index = pair_count; index < left_fields.size(); ++index) {
            value.keys.push_back(
                {left_fields[index], {}, "gdal_left_table"});
        }
        for (std::size_t index = pair_count; index < right_fields.size(); ++index) {
            value.keys.push_back(
                {{}, right_fields[index], "gdal_right_table"});
        }
        value.is_composite = relationship->GetType() == GRT_COMPOSITE;
        value.is_containment = value.is_composite;
        value.forward_label = relationship->GetForwardPathLabel();
        value.backward_label = relationship->GetBackwardPathLabel();
        result.relationships.value.push_back(std::move(value));
    }
    if (result.relationships.status == MetadataStatus::Available &&
        result.relationships.value.empty()) {
        result.relationships.status = MetadataStatus::Empty;
    }
    result.attachments.source = MetadataSource::GdalOpenFileGDB;
    result.attachments.status = MetadataStatus::Unsupported;
    result.attachments.diagnostic =
        "OpenFileGDB OGR relationships do not expose the ArcGIS attachment flag, UUIDs, or raw Definition XML";
    result.catalog_relationships.source = MetadataSource::GdalOpenFileGDB;
    result.catalog_relationships.status = MetadataStatus::Unsupported;
    result.catalog_relationships.diagnostic =
        "FastGdb system-catalog relationship audit is not exposed by the GDAL adapter";

    result.feature_datasets.source = MetadataSource::GdalOpenFileGDB;
    for (const auto& group : state.groups) {
        FeatureDatasetMetadata feature_dataset;
        feature_dataset.name = group.name;
        feature_dataset.path = group.path;
        const auto members = state.layers_by_group.find(group.path);
        if (members != state.layers_by_group.end()) {
            feature_dataset.members = members->second;
        }
        result.feature_datasets.value.push_back(std::move(feature_dataset));
    }
    result.feature_datasets.status = result.feature_datasets.value.empty()
        ? MetadataStatus::Empty : MetadataStatus::Available;
    result.system_table_audit.source = MetadataSource::GdalOpenFileGDB;
    result.system_table_audit.status = MetadataStatus::Unsupported;
    result.system_table_audit.diagnostic =
        "system-table audit is available only from the FastGdb reader";
    result.catalog_objects.source = MetadataSource::GdalOpenFileGDB;
    result.catalog_objects.status = CatalogInventoryStatus::Unsupported;
    result.catalog_objects.diagnostic =
        "OpenFileGDB does not expose the complete GDB_Items row attributes through the OGR API";
    append_gdal_raw_metadata(
        dataset, "gdal_dataset_metadata", state.source.normalized_uri,
        result.raw_payloads);
    return result;
}

LayerMetadataSnapshot public_gdal_layer_metadata(
        GDALDataset& dataset,
        OGRLayer& layer,
        const detail::LayerState& state) {
    LayerMetadataSnapshot result;
    result.name = state.name;
    result.path = state.path;
    result.definition.source = MetadataSource::GdalOpenFileGDB;
    result.definition.status = MetadataStatus::Unsupported;
    result.definition.diagnostic =
        "OpenFileGDB does not expose the complete FileGDB layer definition through the OGR API";
    result.definition.value.name = state.name;
    result.definition.value.path = state.path;
    append_gdal_raw_metadata(
        layer, "gdal_layer_metadata", state.path, result.raw_payloads);
    for (const auto& payload : result.raw_payloads) {
        if (payload.kind == "gdal_layer_metadata:xml:ESRI") {
            result.definition.status = MetadataStatus::Available;
            result.definition.diagnostic.clear();
            break;
        }
    }

    result.field_domains.source = MetadataSource::GdalOpenFileGDB;
    const bool supports_domains = driver_supports_metadata(
        dataset, GDAL_DCAP_FIELD_DOMAINS);
    result.field_domains.status = supports_domains
        ? MetadataStatus::Empty : MetadataStatus::Unsupported;
    if (!supports_domains) {
        result.field_domains.diagnostic =
            "OpenFileGDB does not advertise field-domain reading for this source";
    }
    for (const auto& field : state.schema->fields) {
        if (!field.domain_name || field.domain_name->empty()) continue;
        FieldDomainBinding binding;
        binding.field_name = field.name;
        binding.domain_name = *field.domain_name;
        if (const auto* domain = dataset.GetFieldDomain(binding.domain_name)) {
            binding.domain = public_domain(*domain);
        }
        result.field_domains.value.push_back(std::move(binding));
    }
    if (supports_domains && !result.field_domains.value.empty()) {
        result.field_domains.status = MetadataStatus::Available;
    }

    result.subtypes.source = MetadataSource::GdalOpenFileGDB;
    result.subtypes.status = MetadataStatus::Unsupported;
    result.subtypes.diagnostic =
        "ArcGIS subtype field defaults and domain overrides are not exposed by this GDAL layer API";

    result.relationships.source = MetadataSource::GdalOpenFileGDB;
    result.relationships.status = state.dataset->metadata.relationships.status;
    result.relationships.diagnostic =
        state.dataset->metadata.relationships.diagnostic;
    for (const auto& relationship :
         state.dataset->metadata.relationships.value) {
        const bool related_to_layer =
            std::any_of(
                relationship.origin_class_names.begin(),
                relationship.origin_class_names.end(),
                [&state](const std::string& name) {
                    return ascii_equal(name, state.name);
                }) ||
            std::any_of(
                relationship.destination_class_names.begin(),
                relationship.destination_class_names.end(),
                [&state](const std::string& name) {
                    return ascii_equal(name, state.name);
                });
        if (related_to_layer) {
            result.relationships.value.push_back(relationship);
        }
    }
    if (!result.relationships.value.empty()) {
        result.relationships.status = MetadataStatus::Available;
    } else if (result.relationships.status == MetadataStatus::Available) {
        result.relationships.status = MetadataStatus::Empty;
    }
    result.attachments.source = MetadataSource::GdalOpenFileGDB;
    result.attachments.status = MetadataStatus::Unsupported;
    result.attachments.diagnostic =
        "OpenFileGDB OGR relationships do not expose the ArcGIS attachment flag, UUIDs, or raw Definition XML";
    return result;
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
    state->schema = std::make_shared<const LayerSchema>(
        freeze_gdal_schema(*layer));
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
                                 const detail::LayerState& state,
                                 const GeometryReadOptions& options,
                                 bool include_geometry = true,
                                 const std::vector<std::string>* projection =
                                     nullptr);

Result<std::shared_ptr<detail::LayerState>> make_gdal_fallback_state(
        const std::shared_ptr<detail::LayerState>& state,
        FailureKind reason,
        bool allow_unverified_concurrent_read = false) {
    std::optional<explorgdb::FastReaderLease> lease;
    if (allow_unverified_concurrent_read) {
        if (state->dataset->options.concurrent_read !=
            ConcurrentReadPolicy::GdalUnverified) {
            return make_error(
                ErrorCode::SourceBusy,
                "unverified concurrent GDAL reads were not enabled");
        }
    } else {
        auto lease_result = acquire_fast_reader_lease(
            state->dataset->source.normalized_uri);
        if (!lease_result) return lease_result.error();
        lease.emplace(std::move(lease_result).value());
        if (lease->generation() !=
            state->dataset->consistency.generation) {
            return make_error(
                ErrorCode::SourceChanged,
                "Dataset generation expired before GDAL fallback");
        }
    }
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
    if (!state->schema || !schemas_compatible(*state->schema, candidate)) {
        return make_error(
            ErrorCode::SchemaMismatch,
            "OpenFileGDB fallback schema differs from frozen schema "
            "(geometry types " +
                std::to_string(state->schema->geometry_type) + " vs " +
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
    return map_gdal_feature(
        *feature, *state, GeometryReadOptions{}, true, nullptr);
}

std::string format_ogr_temporal(const OGRField& field, bool date, bool time) {
    std::ostringstream stream;
    if (date) {
        stream << std::setfill('0') << std::setw(4) << field.Date.Year << '-'
               << std::setw(2) << static_cast<int>(field.Date.Month) << '-'
               << std::setw(2) << static_cast<int>(field.Date.Day);
    }
    if (time) {
        if (date) stream << ' ';
        int whole_seconds = static_cast<int>(field.Date.Second);
        int milliseconds = static_cast<int>(std::lround(
            (field.Date.Second - whole_seconds) * 1000.0F));
        if (milliseconds >= 1000) {
            ++whole_seconds;
            milliseconds = 0;
        }
        stream << std::setfill('0') << std::setw(2)
               << static_cast<int>(field.Date.Hour) << ':'
               << std::setw(2) << static_cast<int>(field.Date.Minute) << ':'
               << std::setw(2) << whole_seconds;
        if (milliseconds > 0) {
            stream << '.' << std::setw(3) << milliseconds;
        }
    }
    return stream.str();
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
            if (raw->Date.TZFlag > 1) {
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
                                   const LayerSchema& schema,
                                   const GeometryReadOptions& options) {
    Geometry result;
    result.srs_wkt = schema.srs_wkt;
    result.backend = GeometryBackend::Gdal;
    const OGRGeometry* geometry = source.GetGeometryRef();
    if (geometry == nullptr) {
        result.state = GeometryState::Null;
        return result;
    }
    result.source_was_curve = geometry->hasCurveGeometry(TRUE);
    result.type = static_cast<std::uint32_t>(geometry->getGeometryType());
    result.has_z = wkbHasZ(geometry->getGeometryType());
    result.has_m = wkbHasM(geometry->getGeometryType());
    if (result.source_was_curve &&
        options.curve_policy == CurveReadPolicy::Reject) {
        result.state = GeometryState::Unsupported;
        result.diagnostic = "curve geometry rejected by query policy";
        return result;
    }
    if (geometry->IsEmpty()) {
        result.state = GeometryState::Empty;
        return result;
    }
    const OGRGeometry* output_geometry = geometry;
    std::unique_ptr<OGRGeometry> linearized_geometry;
    if (result.source_was_curve &&
        options.curve_policy == CurveReadPolicy::Linearize) {
        if (options.max_chord_error != 0.0 ||
            options.max_segments_per_curve != 0) {
            result.state = GeometryState::Unsupported;
            result.diagnostic =
                "OpenFileGDB linearization cannot enforce chord-error or segment-count limits";
            return result;
        }
        if (!std::isfinite(options.max_angle_step_degrees) ||
            options.max_angle_step_degrees <= 0.0 ||
            options.max_angle_step_degrees > 180.0) {
            result.state = GeometryState::Unsupported;
            result.diagnostic =
                "invalid GDAL max_angle_step_degrees value";
            return result;
        }
        linearized_geometry.reset(
            geometry->getLinearGeometry(options.max_angle_step_degrees));
        if (!linearized_geometry) {
            return make_error(ErrorCode::ReadFailed,
                              "OpenFileGDB curve linearization failed");
        }
        output_geometry = linearized_geometry.get();
        result.linearized = true;
        result.linearization_max_angle_step_degrees =
            options.max_angle_step_degrees;
    }
    const auto size = output_geometry->WkbSize();
    result.wkb.resize(size);
    if (output_geometry->exportToWkb(wkbNDR, result.wkb.data(),
                                     wkbVariantIso) != OGRERR_NONE) {
        return make_error(ErrorCode::ReadFailed,
                          "OpenFileGDB geometry WKB export failed");
    }
    result.state = GeometryState::Value;
    return result;
}

Result<Feature> map_gdal_feature(const OGRFeature& source,
                                 const detail::LayerState& state,
                                 const GeometryReadOptions& options,
                                 bool include_geometry,
                                 const std::vector<std::string>* projection) {
    Feature feature;
    feature.fid = static_cast<Fid>(source.GetFID());
    if (!state.schema) {
        return make_error(ErrorCode::SchemaMismatch,
                          "frozen Layer schema is unavailable");
    }
    feature.fields.reserve(state.schema->fields.size());
    for (int i = 0; i < source.GetFieldCount(); ++i) {
        const auto index = static_cast<std::size_t>(i);
        if (index >= state.schema->fields.size()) {
            return make_error(ErrorCode::SchemaMismatch,
                              "OpenFileGDB field count differs from frozen schema");
        }
        const bool selected = projection == nullptr ||
            std::find(projection->begin(), projection->end(),
                      state.schema->fields[index].name) != projection->end();
        feature.fields.push_back(
            selected ? map_gdal_field(source, i) : Field{});
    }
    if (include_geometry) {
        auto geometry = map_gdal_geometry(source, *state.schema, options);
        if (!geometry) return geometry.error();
        feature.geometry = std::move(geometry).value();
    } else {
        feature.geometry.state = GeometryState::NotRequested;
        feature.geometry.backend = GeometryBackend::Gdal;
        feature.geometry.srs_wkt = state.schema->srs_wkt;
        feature.geometry.diagnostic = "geometry was not requested";
    }
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
    auto feature = map_gdal_feature(
        *source, *state.layer_state, state.geometry_options,
        state.include_geometry,
        state.projected_fields.empty() ? nullptr : &state.projected_fields);
    if (!feature) return feature.error();
    return std::optional<Feature>{std::move(feature).value()};
}

Result<FeatureCursor> open_gdal_cursor(
        const std::shared_ptr<detail::LayerState>& state,
        const Query& query,
        detail::CursorRegistration registration) {
    auto cursor_state = std::make_unique<detail::CursorState>();
    cursor_state->registration.emplace(std::move(registration));
    cursor_state->cancel_requested = query.cancel_requested;
    cursor_state->deadline = query.deadline;
    cursor_state->geometry_options = query.geometry;
    cursor_state->include_geometry = query.include_geometry;
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
    if (query.attribute_filter.empty() && !query.spatial_filter &&
        (!query.projected_fields.empty() || !query.include_geometry)) {
        std::vector<std::string> ignored_names;
        if (!query.projected_fields.empty()) {
            for (const auto& field : state->schema->fields) {
                if (std::find(query.projected_fields.begin(),
                              query.projected_fields.end(),
                              field.name) == query.projected_fields.end()) {
                    ignored_names.push_back(field.name);
                }
            }
            for (const auto& name : query.projected_fields) {
                const auto field = std::find_if(
                    state->schema->fields.begin(),
                    state->schema->fields.end(),
                    [&name](const FieldDefinition& candidate) {
                        return candidate.name == name;
                    });
                if (field == state->schema->fields.end()) {
                    return make_error(ErrorCode::InvalidRequest,
                                      "projected field does not exist: " + name);
                }
            }
        }
        if (!query.include_geometry) {
            ignored_names.emplace_back("OGR_GEOMETRY");
        }
        std::vector<const char*> ignored;
        ignored.reserve(ignored_names.size() + 1);
        for (const auto& field : ignored_names) {
            ignored.push_back(field.c_str());
        }
        ignored.push_back(nullptr);
        if (!ignored_names.empty() &&
            cursor_state->gdal_layer->TestCapability(OLCIgnoreFields)) {
            CPLErrorReset();
            if (cursor_state->gdal_layer->SetIgnoredFields(
                    ignored.data()) != OGRERR_NONE) {
                return gdal_error(ErrorCode::Unsupported,
                                  "OpenFileGDB projection rejected");
            }
        }
    }
    cursor_state->projected_fields = query.projected_fields;
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

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
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
        auto feature = map_feature(
            source, *state_->layer_state, *state_->layer);
        if (!feature) return feature.error();
        ++state_->query_report.feature_count;
        state_->query_report.materialized_bytes +=
            feature_bytes(feature.value());
        return std::optional<Feature>{std::move(feature).value()};
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
    return state_ && state_->schema ? *state_->schema : kEmptyLayerSchema;
}

Result<FeatureCursor> Layer::open_cursor(const Query& query) const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Layer is closed");
    if (dataset_expired(state_->dataset)) return expired_dataset_error();
    if (!source_is_current(state_->dataset)) return changed_layer_error(state_);
    if (!state_->schema) {
        return make_error(ErrorCode::SchemaMismatch,
                          "frozen Layer schema is unavailable");
    }
    if (const auto error = validate_public_query(query, *state_->schema)) {
        return error;
    }
    auto verify_cursor = [this](Result<FeatureCursor> cursor_result)
            -> Result<FeatureCursor> {
        if (!cursor_result) return cursor_result.error();
        if (!source_is_current(state_->dataset)) return changed_layer_error(state_);
        return std::move(cursor_result).value();
    };
    auto registration_result = register_cursor(state_->dataset);
    if (!registration_result) return registration_result.error();
    auto registration = std::move(registration_result).value();
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
            return verify_cursor(open_gdal_cursor(
                fallback.value(), query, std::move(registration)));
        }
#else
        if (fast_query_has_known_capability_gap(query)) {
            return make_error(ErrorCode::BackendUnavailable,
                              "query fallback requires OpenFileGDB");
        }
#endif
        auto result = open_fast_cursor(
            state_, query, std::move(registration));
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
        if (!result && result.error().code == ErrorCode::SourceBusy &&
            state_->dataset->options.backend == BackendPreference::Auto &&
            state_->dataset->options.concurrent_read ==
                ConcurrentReadPolicy::GdalUnverified) {
            auto fallback = make_gdal_fallback_state(
                state_, FailureKind::CapabilityGap, true);
            if (!fallback) return fallback.error();
            fallback.value()->dataset->consistency.consistency =
                Consistency::UnverifiedConcurrentRead;
            auto fallback_registration = register_cursor(state_->dataset);
            if (!fallback_registration) return fallback_registration.error();
            return verify_cursor(open_gdal_cursor(
                fallback.value(), query,
                std::move(fallback_registration).value()));
        }
        if (!result && result.error().code == ErrorCode::Unsupported &&
            state_->dataset->options.backend == BackendPreference::Auto) {
            auto fallback = make_gdal_fallback_state(
                state_, FailureKind::UnsupportedQuery);
            if (!fallback) return fallback.error();
            auto fallback_registration = register_cursor(state_->dataset);
            if (!fallback_registration) return fallback_registration.error();
            return verify_cursor(open_gdal_cursor(
                fallback.value(), query,
                std::move(fallback_registration).value()));
        }
#endif
        return verify_cursor(std::move(result));
    }
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
    return verify_cursor(
        open_gdal_cursor(state_, query, std::move(registration)));
#else
    return make_error(ErrorCode::BackendUnavailable,
                      "GDAL adapter is unavailable");
#endif
}

Result<Feature> Layer::read_by_fid(Fid fid) const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Layer is closed");
    if (dataset_expired(state_->dataset)) return expired_dataset_error();
    if (!source_is_current(state_->dataset)) return changed_layer_error(state_);
    explorgdb::RowSlot row_slot = 0;
    if (fid <= 0 || !explorgdb::object_id_to_row_slot(
            static_cast<explorgdb::ObjectId>(fid), row_slot)) {
        return make_error(ErrorCode::Unsupported,
                          "FID must be a positive 64-bit value");
    }

    auto registration_result = register_cursor(state_->dataset);
    if (!registration_result) return registration_result.error();
    auto operation_registration =
        std::move(registration_result).value();
    (void)operation_registration;

#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
    if (state_ &&
        state_->report.selected == Backend::GdalOpenFileGDB) {
        auto feature = read_gdal_feature_by_fid(state_, fid);
        if (!feature) return feature.error();
        if (!source_is_current(state_->dataset)) return changed_layer_error(state_);
        return std::move(feature).value();
    }
#endif
    auto feature = read_fast_feature_by_fid(state_, fid);
    if (!feature) return feature.error();
    if (!source_is_current(state_->dataset)) return changed_layer_error(state_);
    return std::move(feature).value();
}

Result<BoundedBinaryValue> Layer::read_binary_field_by_fid(
        Fid fid, std::string_view field_name, std::uint64_t max_bytes) const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Layer is closed");
    if (dataset_expired(state_->dataset)) return expired_dataset_error();
    if (state_->report.selected != Backend::FastGdb) {
        return make_error(
            ErrorCode::Unsupported,
            "bounded Binary reads require the FastGdb backend");
    }
    explorgdb::RowSlot row_slot = 0;
    if (fid <= 0 || !explorgdb::object_id_to_row_slot(
            static_cast<explorgdb::ObjectId>(fid), row_slot)) {
        return make_error(ErrorCode::InvalidRequest,
                          "FID cannot be mapped to a fast row slot");
    }
    if (!source_is_current(state_->dataset)) return changed_layer_error(state_);

    auto lease_result = acquire_fast_reader_lease(
        state_->dataset->source.normalized_uri);
    if (!lease_result) return lease_result.error();
    auto lease = std::move(lease_result).value();
    if (lease.generation() != state_->dataset->consistency.generation) {
        return make_error(
            ErrorCode::SourceChanged,
            "Dataset generation expired; refresh the Dataset and reopen the Layer");
    }

    if (!state_->dataset->fast_reader) {
        return make_error(ErrorCode::OpenFailed,
                          "cached FastGDB Reader snapshot is unavailable");
    }
    explorgdb::ReaderError reader_status;
    auto native_layer = state_->dataset->fast_reader->open_layer(
        state_->name, &reader_status);
    if (!native_layer) return reader_error(reader_status);
    if (!same_fast_field_schema(state_->source_fields,
                                native_layer->fields())) {
        return make_error(
            ErrorCode::SchemaMismatch,
            "Reader field schema differs from the frozen Layer schema");
    }

    const auto size_limit = static_cast<std::size_t>(std::min<std::uint64_t>(
        max_bytes, std::numeric_limits<std::size_t>::max()));
    const auto value = native_layer->read_binary_field_by_fid(
        row_slot, std::string(field_name),
        size_limit);
    if (!source_is_current(state_->dataset)) return changed_layer_error(state_);

    switch (value.status) {
        case explorgdb::BinaryValueReadStatus::Available: {
            BoundedBinaryValue result;
            result.byte_size = value.byte_size;
            result.bytes = value.bytes;
            return result;
        }
        case explorgdb::BinaryValueReadStatus::NullValue: {
            BoundedBinaryValue result;
            result.is_null = true;
            return result;
        }
        case explorgdb::BinaryValueReadStatus::FieldNotFound:
        case explorgdb::BinaryValueReadStatus::NotBinaryField:
            return make_error(ErrorCode::InvalidRequest, value.diagnostic);
        case explorgdb::BinaryValueReadStatus::FeatureNotFound:
            return make_error(ErrorCode::FeatureNotFound, value.diagnostic);
        case explorgdb::BinaryValueReadStatus::LimitExceeded:
            return make_error(ErrorCode::ResultLimitExceeded,
                value.diagnostic + " (" + std::to_string(value.byte_size) +
                " bytes requested, " + std::to_string(max_bytes) +
                " byte limit)");
        case explorgdb::BinaryValueReadStatus::SourceChanged:
            return changed_layer_error(state_);
        case explorgdb::BinaryValueReadStatus::ReadFailed:
            return make_error(ErrorCode::ReadFailed, value.diagnostic);
    }
    return make_error(ErrorCode::ReadFailed,
                      "unknown bounded Binary read status");
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
    if (!source_is_current(state_->dataset)) return changed_layer_error(state_);
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
    if (dataset_expired(state_->dataset)) return expired_dataset_error();
    if (!source_is_current(state_->dataset)) return changed_layer_error(state_);
    CapabilityReport report;
    report.native_extensions =
        state_->report.selected == Backend::FastGdb;
    return report;
}

Result<LayerMetadataSnapshot> Layer::metadata() const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Layer is closed");
    if (dataset_expired(state_->dataset)) return expired_dataset_error();
    if (!source_is_current(state_->dataset)) return changed_layer_error(state_);
    if (state_->report.selected == Backend::FastGdb) {
        auto lease = acquire_fast_reader_lease(
            state_->dataset->source.normalized_uri);
        if (!lease) return lease.error();
        if (lease.value().generation() !=
            state_->dataset->consistency.generation) {
            return make_error(
                ErrorCode::SourceChanged,
                "Dataset generation expired; refresh before reading metadata");
        }
        if (!state_->metadata) {
            return make_error(ErrorCode::ReadFailed,
                              "FastGdb layer metadata snapshot is unavailable");
        }
        return *state_->metadata;
    }
#if defined(FAST_GDB_UNIFIED_WITH_GDAL)
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(
        open_openfilegdb(
            state_->dataset->source.normalized_uri,
            state_->dataset->options.include_system_tables),
        GDALClose);
    if (!dataset) {
        return gdal_error(ErrorCode::ReadFailed,
                          "OpenFileGDB metadata open failed");
    }
    OGRLayer* layer = open_gdal_layer_by_path(
        *dataset, state_->path, state_->name);
    if (layer == nullptr) {
        return gdal_error(ErrorCode::LayerNotFound,
                          "OpenFileGDB metadata Layer open failed");
    }
    auto metadata = public_gdal_layer_metadata(*dataset, *layer, *state_);
    if (!source_is_current(state_->dataset)) return changed_layer_error(state_);
    return metadata;
#else
    return make_error(ErrorCode::BackendUnavailable,
                      "GDAL metadata adapter is unavailable");
#endif
}

Result<QueryPlan> Layer::explain(const Query& query) const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Layer is closed");
    if (dataset_expired(state_->dataset)) return expired_dataset_error();
    if (!source_is_current(state_->dataset)) return changed_layer_error(state_);
    if (!state_->schema) {
        return make_error(ErrorCode::SchemaMismatch,
                          "frozen Layer schema is unavailable");
    }
    if (const auto error = validate_public_query(query, *state_->schema)) {
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
    if (dataset_expired(state_->dataset)) return expired_dataset_error();
    if (!source_is_current(state_->dataset)) return changed_layer_error(state_);
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
    explorgdb::RowSlot row_slot = 0;
    if (!state_ || fid <= 0 || !explorgdb::object_id_to_row_slot(
            static_cast<explorgdb::ObjectId>(fid), row_slot)) {
        return make_error(ErrorCode::Unsupported,
                          "FID cannot be mapped to a fast row slot");
    }
    if (dataset_expired(state_->dataset)) return expired_dataset_error();
    if (!source_is_current(state_->dataset)) return changed_layer_error(state_);
    auto lease_result = acquire_fast_reader_lease(
        state_->dataset->source.normalized_uri);
    if (!lease_result) return lease_result.error();
    auto lease = std::move(lease_result).value();
    if (!state_->dataset->fast_reader) {
        return make_error(ErrorCode::OpenFailed,
                          "cached FastGDB Reader snapshot is unavailable");
    }
    explorgdb::ReaderError status;
    auto layer = state_->dataset->fast_reader->open_layer(
        state_->name, &status);
    if (!layer) return reader_error(status);
    if (lease.generation() != state_->dataset->consistency.generation) {
        return make_error(
            ErrorCode::SourceChanged,
            "Dataset generation expired; refresh the Dataset and reopen the Layer");
    }
    if (!same_fast_field_schema(state_->source_fields, layer->fields())) {
        return make_error(
            ErrorCode::SchemaMismatch,
            "Reader field schema differs from the frozen Layer schema");
    }
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
    feature.row_slot = row_slot;
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
    if (!source_is_current(state_->dataset)) return changed_layer_error(state_);
    return feature;
}

Dataset::Dataset(std::shared_ptr<detail::DatasetState> state)
    : state_(std::move(state)) {}

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
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
    if (state->source.kind == SourceKind::LocalFileGdb) {
        std::error_code directory_error;
        if (!std::filesystem::is_directory(
                std::filesystem::u8path(state->source.normalized_uri),
                directory_error)) {
            return make_error(
                ErrorCode::SourceNotFound,
                "FileGDB directory does not exist or is inaccessible");
        }
        state->has_source_snapshot = capture_source_snapshot(
            state->source.normalized_uri, state->source_snapshot);
        if (!state->has_source_snapshot) {
            return make_error(
                ErrorCode::OpenFailed,
                "failed to capture the initial GDB file identity snapshot");
        }
    }
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
        state->fast_reader = std::make_shared<explorgdb::Reader>(
            std::move(*reader));
        state->layer_names = state->fast_reader->layer_names();
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
        state->metadata = public_gdal_workspace_metadata(*dataset, *state);
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
        if (!state->fast_reader) {
            return make_error(ErrorCode::OpenFailed,
                              "FastGDB Reader snapshot is unavailable");
        }
        if (auto error = populate_fast_groups(*state, *state->fast_reader)) {
            return error;
        }
        if (!state->fast_reader->source_is_current()) {
            return make_error(
                ErrorCode::SourceChanged,
                "GDB source changed while building Dataset metadata");
        }
    } else if (state->paths_by_name.empty()) {
        for (const auto& name : state->layer_names) {
            index_group_layer(*state, name, "/" + name);
        }
    }
    if (!source_is_current(state)) return changed_source_error();
    return Dataset(std::move(state));
}

Result<Dataset> Dataset::refresh() const {
    if (!state_ || !state_->lifetime) {
        return make_error(ErrorCode::OpenFailed, "Dataset is closed");
    }
    {
        std::lock_guard<std::mutex> lock(state_->lifetime->mutex);
        if (state_->lifetime->active_cursors != 0) {
            return make_error(
                ErrorCode::SourceBusy,
                "Dataset refresh is blocked while a feature cursor is active");
        }
        state_->lifetime->expired = true;
    }
    // Invalidate old and current source identities without evicting other GDBs.
    invalidate_tablx_cache(state_->source_snapshot);
    std::unordered_map<std::string, explorgdb::TablxCacheKey> current_snapshot;
    if (capture_source_snapshot(
            state_->source.normalized_uri, current_snapshot)) {
        invalidate_tablx_cache(current_snapshot);
    }
    return Dataset::open(
        state_->source.normalized_uri, state_->options);
}

Result<std::vector<std::string>> Dataset::layer_names() const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Dataset is closed");
    if (dataset_expired(state_)) return expired_dataset_error();
    if (!source_is_current(state_)) return changed_source_error();
    return state_->layer_names;
}

Result<WorkspaceMetadataSnapshot> Dataset::metadata() const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Dataset is closed");
    if (dataset_expired(state_)) return expired_dataset_error();
    if (!source_is_current(state_)) return changed_source_error();
    if (state_->report.selected == Backend::FastGdb) {
        auto lease = acquire_fast_reader_lease(
            state_->source.normalized_uri);
        if (!lease) return lease.error();
        if (lease.value().generation() != state_->consistency.generation) {
            return make_error(
                ErrorCode::SourceChanged,
                "Dataset generation expired; refresh before reading metadata");
        }
    }
    return state_->metadata;
}

CatalogObjectLookupResult Dataset::catalog_object_by_uuid(
        std::string_view uuid) const {
    CatalogObjectLookupResult result;
    if (!state_) {
        result.status = CatalogObjectLookupStatus::Unavailable;
        result.diagnostic = "Dataset is closed";
        return result;
    }
    if (dataset_expired(state_)) {
        result.status = CatalogObjectLookupStatus::SourceChanged;
        result.diagnostic = "Dataset generation expired; reopen after refresh";
        return result;
    }
    if (!source_is_current(state_)) {
        result.status = CatalogObjectLookupStatus::SourceChanged;
        result.diagnostic = "GDB source changed; reopen the Dataset";
        return result;
    }
    if (state_->report.selected != Backend::FastGdb) {
        result.status = CatalogObjectLookupStatus::Unsupported;
        result.diagnostic =
            "catalog object attributes and raw definitions require the FastGdb backend";
        return result;
    }

    auto lease_result = acquire_fast_reader_lease(
        state_->source.normalized_uri);
    if (!lease_result) {
        result.status = lease_result.error().code == ErrorCode::SourceBusy
            ? CatalogObjectLookupStatus::SourceBusy
            : CatalogObjectLookupStatus::SourceChanged;
        result.diagnostic = lease_result.error().message;
        return result;
    }
    auto lease = std::move(lease_result).value();
    if (lease.generation() != state_->consistency.generation) {
        result.status = CatalogObjectLookupStatus::SourceChanged;
        result.diagnostic =
            "Dataset generation expired; reopen before reading catalog metadata";
        return result;
    }

    if (!state_->fast_reader) {
        result.status = CatalogObjectLookupStatus::Unavailable;
        result.diagnostic = "cached FastGDB Reader snapshot is unavailable";
        return result;
    }
    auto source_result = state_->fast_reader->read_catalog_object_by_uuid(
        std::string(uuid));
    result.status = catalog_object_status(source_result.status);
    result.diagnostic = std::move(source_result.diagnostic);
    result.integrity_issues.reserve(source_result.integrity_issues.size());
    for (const auto& issue : source_result.integrity_issues) {
        result.integrity_issues.push_back(
            public_catalog_integrity_issue(issue));
    }
    if (source_result.object) {
        result.object = public_catalog_object(*source_result.object);
    }
    if (!source_is_current(state_)) {
        result.status = CatalogObjectLookupStatus::SourceChanged;
        result.object.reset();
        result.diagnostic = "GDB source changed while reading catalog metadata";
    }
    return result;
}

Result<Layer> Dataset::open_layer(std::string_view name) const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Dataset is closed");
    if (dataset_expired(state_)) return expired_dataset_error();
    if (!source_is_current(state_)) return changed_source_error();
    std::optional<explorgdb::FastReaderLease> metadata_lease;
    if (state_->report.selected == Backend::FastGdb) {
        auto lease =
            acquire_fast_reader_lease(state_->source.normalized_uri);
        if (!lease) return lease.error();
        metadata_lease.emplace(std::move(lease).value());
        if (metadata_lease->generation() !=
            state_->consistency.generation) {
            return make_error(
                ErrorCode::SourceChanged,
                "Dataset generation expired; refresh before opening a Layer");
        }
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
    const std::string selected_path = matches.front().second->front();
    std::string layer_cache_key = selected_name;
    layer_cache_key.push_back('\0');
    layer_cache_key += selected_path;
    {
        auto& cache = *state_->layer_state_cache;
        std::lock_guard<std::mutex> lock(cache.mutex);
        const auto cached = cache.entries.find(layer_cache_key);
        if (cached != cache.entries.end()) {
            cache.lru.splice(cache.lru.end(), cache.lru,
                             cached->second.lru_position);
            if (auto layer_state = cached->second.live_state.lock()) {
                return Layer(std::move(layer_state));
            }
            if (cached->second.snapshot) {
                const auto& snapshot = *cached->second.snapshot;
                auto layer_state =
                    std::make_shared<detail::LayerState>();
                layer_state->dataset = state_;
                layer_state->name = snapshot.name;
                layer_state->path = snapshot.path;
                layer_state->schema = snapshot.schema;
                layer_state->source_field_indices =
                    snapshot.source_field_indices;
                layer_state->source_fields = snapshot.source_fields;
                layer_state->metadata = snapshot.metadata;
                layer_state->report = snapshot.report;
                cached->second.live_state = layer_state;
                return Layer(std::move(layer_state));
            }
            cache.lru.erase(cached->second.lru_position);
            cache.entries.erase(cached);
        }
    }
    auto layer_state = std::make_shared<detail::LayerState>();
    layer_state->dataset = state_;
    layer_state->name = selected_name;
    layer_state->path = selected_path;
    layer_state->report = state_->report;
    if (state_->report.selected == Backend::FastGdb) {
        if (!state_->fast_reader) {
            return make_error(ErrorCode::OpenFailed,
                              "cached FastGDB Reader snapshot is unavailable");
        }
        explorgdb::ReaderError status;
        auto source_layer = state_->fast_reader->open_layer(
            selected_name, &status);
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
            layer_state->source_fields = source_layer->fields();
            explorgdb::LayerMetadataSnapshot native_metadata;
            auto schema = freeze_schema(
                *source_layer, layer_state->source_field_indices,
                &native_metadata);
            if (schema) {
                layer_state->schema = std::make_shared<const LayerSchema>(
                    std::move(schema).value());
                layer_state->metadata = public_layer_metadata(
                    native_metadata, layer_state->path);
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
    if (!source_is_current(state_)) return changed_source_error();
    if (!layer_state->schema) {
        return make_error(ErrorCode::SchemaMismatch,
                          "frozen Layer schema is unavailable");
    }
    auto snapshot = std::make_shared<detail::LayerStateSnapshot>();
    snapshot->name = layer_state->name;
    snapshot->path = layer_state->path;
    snapshot->schema = layer_state->schema;
    snapshot->source_field_indices = layer_state->source_field_indices;
    snapshot->source_fields = layer_state->source_fields;
    snapshot->metadata = layer_state->metadata;
    snapshot->report = layer_state->report;
    {
        auto& cache = *state_->layer_state_cache;
        std::lock_guard<std::mutex> lock(cache.mutex);
        const auto existing = cache.entries.find(layer_cache_key);
        if (existing != cache.entries.end()) {
            if (auto live_state = existing->second.live_state.lock()) {
                cache.lru.splice(cache.lru.end(), cache.lru,
                                 existing->second.lru_position);
                return Layer(std::move(live_state));
            }
            cache.lru.erase(existing->second.lru_position);
            cache.entries.erase(existing);
        }
        cache.lru.push_back(layer_cache_key);
        auto lru_position = std::prev(cache.lru.end());
        cache.entries.emplace(
            layer_cache_key,
            detail::LayerStateCacheEntry{
                layer_state, std::move(snapshot), lru_position});
        while (cache.entries.size() >
               detail::kLayerStateSnapshotCacheCapacity) {
            const auto oldest = cache.lru.begin();
            cache.entries.erase(*oldest);
            cache.lru.erase(oldest);
        }
    }
    return Layer(std::move(layer_state));
}

Result<Layer> Dataset::open_layer_by_path(std::string_view path) const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Dataset is closed");
    if (dataset_expired(state_)) return expired_dataset_error();
    if (!source_is_current(state_)) return changed_source_error();
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
    if (dataset_expired(state_)) return expired_dataset_error();
    if (!source_is_current(state_)) return changed_source_error();
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
    if (dataset_expired(state_->dataset)) return expired_dataset_error();
    if (!source_is_current(state_->dataset)) return changed_source_error();
    return state_->groups;
}

Result<std::vector<LayerInfo>> Group::layers() const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Group is closed");
    if (dataset_expired(state_->dataset)) return expired_dataset_error();
    if (!source_is_current(state_->dataset)) return changed_source_error();
    return state_->layers;
}

Result<Group> Group::open_group(std::string_view name) const {
    if (!state_) return make_error(ErrorCode::OpenFailed, "Group is closed");
    if (dataset_expired(state_->dataset)) return expired_dataset_error();
    if (!source_is_current(state_->dataset)) return changed_source_error();
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
