// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

#include "reader.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "gdb_tablx_cache.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace explorgdb {
namespace detail {

struct ReaderState {
    std::string source_path;
    std::shared_ptr<GdbCatalog> catalog;
    std::shared_ptr<CatalogResolver> resolver;
    std::shared_ptr<QueryPreparationCache> query_preparation_cache =
        make_query_preparation_cache();
    std::unordered_map<std::string, TablxCacheKey> source_snapshot;
    std::mutex spatial_index_mutex;
    std::unordered_map<std::string, std::optional<bool>> spatial_index_flags;

    std::optional<bool> spatial_index_declared(
            const std::string& layer_name) {
        {
            std::lock_guard<std::mutex> lock(spatial_index_mutex);
            const auto found = spatial_index_flags.find(layer_name);
            if (found != spatial_index_flags.end()) return found->second;
        }

        std::optional<bool> declared;
        if (resolver != nullptr) {
            MetadataReader metadata(*resolver);
            const auto layer = metadata.read_layer_metadata(layer_name);
            if (layer) declared = layer->has_spatial_index;
        }

        std::lock_guard<std::mutex> lock(spatial_index_mutex);
        return spatial_index_flags.emplace(layer_name, declared)
            .first->second;
    }

    bool capture_source_snapshot() {
        source_snapshot.clear();
        std::error_code error;
        std::filesystem::directory_iterator iterator(
            std::filesystem::u8path(source_path), error);
        if (error) return false;
        for (const auto& entry : iterator) {
            if (!entry.is_regular_file(error)) {
                if (error) return false;
                continue;
            }
            TablxCacheKey identity;
            if (!TablxCache::make_key(entry.path().u8string(), identity))
                return false;
            source_snapshot.emplace(
                entry.path().filename().u8string(), identity);
        }
        return true;
    }

    bool source_is_current() const noexcept {
        try {
            std::unordered_map<std::string, TablxCacheKey> current_snapshot;
            std::error_code error;
            std::filesystem::directory_iterator iterator(
                std::filesystem::u8path(source_path), error);
            if (error) return false;
            for (const auto& entry : iterator) {
                if (!entry.is_regular_file(error)) {
                    if (error) return false;
                    continue;
                }
                TablxCacheKey identity;
                if (!TablxCache::make_key(entry.path().u8string(), identity))
                    return false;
                current_snapshot.emplace(
                    entry.path().filename().u8string(), identity);
            }
            return current_snapshot == source_snapshot;
        } catch (...) {
            return false;
        }
    }
};

}  // namespace detail

namespace {

void set_error(ReaderError* error, ReaderStatus status, std::string message) {
    if (error == nullptr) return;
    error->status = status;
    error->message = std::move(message);
}

}  // namespace

const char* reader_status_name(ReaderStatus status) noexcept {
    switch (status) {
        case ReaderStatus::Ok: return "ok";
        case ReaderStatus::InvalidArgument: return "invalid_argument";
        case ReaderStatus::SourceNotFound: return "source_not_found";
        case ReaderStatus::CatalogScanFailed: return "catalog_scan_failed";
        case ReaderStatus::CatalogResolveFailed: return "catalog_resolve_failed";
        case ReaderStatus::SourceSnapshotFailed: return "source_snapshot_failed";
        case ReaderStatus::SourceChanged: return "source_changed";
        case ReaderStatus::MetadataReadFailed: return "metadata_read_failed";
        case ReaderStatus::LayerNotFound: return "layer_not_found";
        case ReaderStatus::LayerOpenFailed: return "layer_open_failed";
        case ReaderStatus::FidRangeUnsupported: return "fid_range_unsupported";
    }
    return "unknown";
}

Layer::Layer(std::shared_ptr<detail::ReaderState> state,
             ResolvedTable resolved,
             std::unique_ptr<QueryEngine> engine)
    : state_(std::move(state)),
      resolved_(std::move(resolved)),
      engine_(std::move(engine)),
      metadata_(*state_->resolver) {}

Layer::Layer(Layer&&) noexcept = default;
Layer::~Layer() = default;

const std::vector<FieldDescriptor>& Layer::fields() const {
    static const std::vector<FieldDescriptor> empty;
    return engine_ != nullptr && engine_->table() != nullptr
        ? engine_->table()->fields()
        : empty;
}

const CapabilityReport& Layer::capabilities() const {
    static const CapabilityReport empty;
    return engine_ != nullptr ? engine_->capabilities() : empty;
}

MetadataReadResult Layer::read_metadata() const {
    MetadataReadResult result;
    if (!source_is_current()) {
        set_error(&result.error, ReaderStatus::SourceChanged,
                  "GDB source changed while reading metadata; reopen the Reader");
        return result;
    }
    try {
        result.snapshot.name = name();
        result.snapshot.fields = fields();
        result.snapshot.capabilities = capabilities();
        result.snapshot.layer = metadata_.read_layer_metadata(name());
        result.snapshot.workspace_domains = metadata_.read_workspace_domains();
        result.snapshot.field_domains =
            metadata_.read_field_domain_bindings(name());
        result.snapshot.relationships = metadata_.read_relationship_summaries();
        result.snapshot.relationship_definitions =
            metadata_.read_relationship_class_definitions();
        auto relationship_classes = metadata_.read_relationship_classes_result();
        result.snapshot.relationship_classes =
            std::move(relationship_classes.relationships);
        result.snapshot.relationship_classes_xml =
            std::move(relationship_classes.inspection);
        result.snapshot.attachments =
            std::move(relationship_classes.attachments);
        result.snapshot.attachments_xml =
            std::move(relationship_classes.attachments_inspection);
        auto dataset_groups = metadata_.read_dataset_group_summaries_result();
        result.snapshot.dataset_groups = std::move(dataset_groups.groups);
        result.snapshot.feature_datasets_xml =
            std::move(dataset_groups.inspection);
        result.snapshot.system_table_audit = metadata_.audit_system_tables();
        result.snapshot.subtypes = metadata_.read_subtypes(name());
        if (!result.snapshot.layer.has_value()) {
            result.snapshot = {};
            set_error(&result.error, ReaderStatus::MetadataReadFailed,
                      "layer metadata is unavailable; inspect the GDB system tables");
            return result;
        }
        const auto& layer = *result.snapshot.layer;
        const std::optional<std::string_view> definition =
            layer.definition_present
                ? std::optional<std::string_view>(layer.definition)
                : std::nullopt;
        result.snapshot.definition_xml = MetadataReader::inspect_metadata_xml(
            definition, MetadataXmlKind::LayerDefinition);
        result.snapshot.field_domains_xml =
            MetadataReader::inspect_metadata_xml(
                definition, MetadataXmlKind::FieldDomainBindings);
        result.snapshot.subtypes_xml = MetadataReader::inspect_metadata_xml(
            definition, MetadataXmlKind::Subtypes);
    } catch (const std::exception& exception) {
        result.snapshot = {};
        set_error(&result.error, ReaderStatus::MetadataReadFailed,
                  std::string("metadata read failed: ") + exception.what());
        return result;
    } catch (...) {
        result.snapshot = {};
        set_error(&result.error, ReaderStatus::MetadataReadFailed,
                  "metadata read failed with an unknown exception");
        return result;
    }
    if (!source_is_current()) {
        result.snapshot = {};
        set_error(&result.error, ReaderStatus::SourceChanged,
                  "GDB source changed while reading metadata; reopen the Reader");
    }
    return result;
}

LayerMetadataSnapshot Layer::metadata_snapshot() const {
    return read_metadata().snapshot;
}

bool Layer::read_raw_by_fid(RowSlot fid,
                            FeatureRecord& record,
                            std::vector<uint8_t>& raw_record) {
    record = {};
    raw_record.clear();
    if (!source_is_current() || engine_ == nullptr ||
        engine_->table() == nullptr) {
        return false;
    }
    FeatureRecord value;
    std::vector<uint8_t> raw_value;
    if (!engine_->read_by_fid(fid, value) ||
        !engine_->table()->read_raw_record_by_fid(fid, raw_value) ||
        !source_is_current()) {
        return false;
    }
    record = std::move(value);
    raw_record = std::move(raw_value);
    return true;
}

bool Layer::read_raw_by_fid(RowSlot fid,
                            std::vector<uint8_t>& raw_record,
                            std::size_t max_bytes,
                            bool* limit_exceeded) {
    raw_record.clear();
    if (limit_exceeded != nullptr) *limit_exceeded = false;
    if (!source_is_current() || engine_ == nullptr ||
        engine_->table() == nullptr) {
        return false;
    }
    std::vector<uint8_t> raw_value;
    bool exceeded = false;
    const bool read = engine_->table()->read_raw_record_by_fid(
        fid, raw_value, max_bytes, &exceeded);
    if (limit_exceeded != nullptr) *limit_exceeded = exceeded;
    if (!read || !source_is_current()) {
        return false;
    }
    raw_record = std::move(raw_value);
    return true;
}

std::size_t Layer::active_feature_count() const noexcept {
    return engine_ != nullptr && engine_->table() != nullptr
        ? engine_->table()->active_feature_count() : 0;
}

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
QueryResult Layer::query(const QueryRequest& request) {
    if (!source_is_current()) {
        QueryResult result;
        result.status = QueryStatus::SourceChanged;
        result.error = "GDB source changed after Layer open; reopen the Reader";
        result.fallback_reason = result.error;
        return result;
    }
    QueryResult result = engine_ != nullptr ? engine_->query(request) : QueryResult{};
    if (!source_is_current()) {
        result.matched_fids.clear();
        result.record.reset();
        result.status = QueryStatus::SourceChanged;
        result.error = "GDB source changed while executing the query; reopen the Reader";
        result.fallback_reason = result.error;
    }
    return result;
}

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
FeatureCursor Layer::open_cursor(const QueryRequest& request) {
    if (!source_is_current()) {
        QueryResult result;
        result.status = QueryStatus::SourceChanged;
        result.error = "GDB source changed after Layer open; reopen the Reader";
        result.fallback_reason = result.error;
        return FeatureCursor::failed(result, result.error);
    }
    if (engine_ != nullptr) {
        FeatureCursor cursor = engine_->open_cursor(request);
        if (!source_is_current()) {
            QueryResult result;
            result.status = QueryStatus::SourceChanged;
            result.error = "GDB source changed while opening the cursor; reopen the Reader";
            result.fallback_reason = result.error;
            return FeatureCursor::failed(result, result.error);
        }
        return cursor;
    }
    return FeatureCursor{};
}

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool Layer::read_by_fid(RowSlot fid, FeatureRecord& record) {
    if (!source_is_current() || engine_ == nullptr) return false;
    FeatureRecord value;
    if (!engine_->read_by_fid(fid, value) || !source_is_current()) return false;
    record = std::move(value);
    return true;
}

BinaryValueReadResult Layer::read_binary_field_by_fid(
        RowSlot fid,
        const std::string& field_name,
        std::size_t max_bytes) {
    BinaryValueReadResult result;
    const auto fail = [&result](BinaryValueReadStatus status,
                                std::string diagnostic) {
        result.status = status;
        result.diagnostic = std::move(diagnostic);
        return result;
    };
    if (!source_is_current()) {
        return fail(BinaryValueReadStatus::SourceChanged,
                    "GDB source changed before reading the Binary field");
    }
    if (engine_ == nullptr || engine_->table() == nullptr) {
        return fail(BinaryValueReadStatus::ReadFailed,
                    "layer has no open table parser");
    }

    const auto equals_ignore_case = [](const std::string& lhs,
                                       const std::string& rhs) {
        return lhs.size() == rhs.size() &&
            std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                [](unsigned char left, unsigned char right) {
                    return std::tolower(left) == std::tolower(right);
                });
    };
    const auto& fields = engine_->table()->fields();
    const auto field = std::find_if(
        fields.begin(), fields.end(), [&field_name, &equals_ignore_case](
                const FieldDescriptor& candidate) {
            return equals_ignore_case(candidate.name, field_name);
        });
    if (field == fields.end()) {
        return fail(BinaryValueReadStatus::FieldNotFound,
                    "Binary field does not exist: " + field_name);
    }
    if (field->type != FieldType::Binary) {
        return fail(BinaryValueReadStatus::NotBinaryField,
                    "field is not Binary: " + field->name);
    }
    if (!engine_->table()->has_feature(fid)) {
        return fail(BinaryValueReadStatus::FeatureNotFound,
                    "feature FID does not exist");
    }
    if (!source_is_current()) {
        return fail(BinaryValueReadStatus::SourceChanged,
                    "GDB source changed before reading the Binary field");
    }

    const auto field_index = static_cast<std::size_t>(field - fields.begin());
    bool is_null = false;
    bool limit_exceeded = false;
    if (!engine_->table()->read_binary_field_by_fid(
            fid, field_index, max_bytes, result.bytes, result.byte_size,
            is_null, limit_exceeded)) {
        return fail(BinaryValueReadStatus::ReadFailed,
                    "feature row could not be decoded");
    }
    if (is_null) {
        result.status = BinaryValueReadStatus::NullValue;
        result.byte_size = 0;
    } else if (limit_exceeded) {
        result.status = BinaryValueReadStatus::LimitExceeded;
        result.diagnostic =
            "Binary payload exceeds the configured byte limit";
    } else {
        result.status = BinaryValueReadStatus::Available;
    }
    if (!source_is_current()) {
        result.bytes.clear();
        result.status = BinaryValueReadStatus::SourceChanged;
        result.diagnostic =
            "GDB source changed while reading the Binary field";
    }
    return result;
}

bool Layer::source_is_current() const noexcept {
    return state_ != nullptr && state_->source_is_current();
}

std::optional<Reader> Reader::open(const std::string& gdb_path,
                                   ReaderOptions options,
                                   ReaderError* error) {
    if (gdb_path.empty()) {
        set_error(error, ReaderStatus::InvalidArgument,
                  "GDB path must not be empty; provide an existing .gdb directory");
        return std::nullopt;
    }
    const std::filesystem::path path = std::filesystem::u8path(gdb_path);
    if (!std::filesystem::is_directory(path)) {
        set_error(error, ReaderStatus::SourceNotFound,
                  "GDB path is not a directory: " + gdb_path +
                      "; check the source path and retry");
        return std::nullopt;
    }

    auto state = std::make_shared<detail::ReaderState>();
    state->source_path = gdb_path;
    if (!state->capture_source_snapshot()) {
        set_error(error, ReaderStatus::SourceSnapshotFailed,
                  "failed to capture the initial GDB source snapshot; verify file access and retry");
        return std::nullopt;
    }
    state->catalog = std::make_shared<GdbCatalog>();
    if (!state->catalog->scan(gdb_path)) {
        set_error(error, ReaderStatus::CatalogScanFailed,
                  "failed to scan GDB catalog: " + gdb_path +
                      "; verify directory permissions and FileGDB contents");
        return std::nullopt;
    }
    state->resolver = std::make_shared<CatalogResolver>(*state->catalog);
    if (!state->resolver->load()) {
        set_error(error, ReaderStatus::CatalogResolveFailed,
                  "failed to resolve GDB system catalog: " + gdb_path +
                      "; verify the system catalog files and retry");
        return std::nullopt;
    }
    if (!state->source_is_current()) {
        set_error(error, ReaderStatus::SourceChanged,
                  "GDB source changed while opening; retry with a stable source directory");
        return std::nullopt;
    }

    Reader reader;
    reader.state_ = std::move(state);
    reader.path_ = gdb_path;
    reader.options_ = options;
    reader.error_ = {};
    set_error(error, ReaderStatus::Ok, {});
    return reader;
}

std::vector<std::string> Reader::layer_names() const {
    return state_ != nullptr && state_->resolver != nullptr
        ? state_->resolver->table_names()
        : std::vector<std::string>{};
}

WorkspaceMetadataReadResult Reader::read_workspace_metadata() const {
    WorkspaceMetadataReadResult result;
    if (state_ == nullptr || state_->resolver == nullptr) {
        set_error(&result.error, ReaderStatus::CatalogResolveFailed,
                  "Reader is not open; call Reader::open with a valid .gdb directory");
        return result;
    }
    if (!source_is_current()) {
        set_error(&result.error, ReaderStatus::SourceChanged,
                  "GDB source changed before reading workspace metadata");
        return result;
    }
    try {
        MetadataReader metadata(*state_->resolver);
        auto domains = metadata.read_workspace_domains_result();
        result.snapshot.domains = std::move(domains.domains);
        result.snapshot.domains_xml = std::move(domains.inspection);
        result.snapshot.relationships = metadata.read_relationship_summaries();
        result.snapshot.relationship_definitions =
            metadata.read_relationship_class_definitions();
        auto relationship_classes = metadata.read_relationship_classes_result();
        result.snapshot.relationship_classes =
            std::move(relationship_classes.relationships);
        result.snapshot.relationship_classes_xml =
            std::move(relationship_classes.inspection);
        result.snapshot.attachments =
            std::move(relationship_classes.attachments);
        result.snapshot.attachments_xml =
            std::move(relationship_classes.attachments_inspection);
        auto dataset_groups = metadata.read_dataset_group_summaries_result();
        result.snapshot.dataset_groups = std::move(dataset_groups.groups);
        result.snapshot.feature_datasets_xml =
            std::move(dataset_groups.inspection);
        result.snapshot.system_table_audit = metadata.audit_system_tables();
        result.snapshot.catalog_objects =
            metadata.read_catalog_object_inventory();
    } catch (const std::exception& exception) {
        set_error(&result.error, ReaderStatus::MetadataReadFailed,
                  std::string("workspace metadata read failed: ") +
                      exception.what());
        return result;
    } catch (...) {
        set_error(&result.error, ReaderStatus::MetadataReadFailed,
                  "workspace metadata read failed with an unknown exception");
        return result;
    }
    if (!source_is_current()) {
        result.snapshot = {};
        set_error(&result.error, ReaderStatus::SourceChanged,
                  "GDB source changed while reading workspace metadata");
        return result;
    }
    set_error(&result.error, ReaderStatus::Ok, {});
    return result;
}

CatalogObjectReadResult Reader::read_catalog_object_by_uuid(
        const std::string& uuid) const {
    CatalogObjectReadResult result;
    if (state_ == nullptr || state_->resolver == nullptr) {
        result.status = CatalogObjectReadStatus::Unavailable;
        result.diagnostic =
            "Reader is not open; call Reader::open with a valid .gdb directory";
        return result;
    }
    if (!source_is_current()) {
        result.status = CatalogObjectReadStatus::Unavailable;
        result.diagnostic =
            "GDB source changed before reading the catalog object";
        return result;
    }
    try {
        MetadataReader metadata(*state_->resolver);
        result = metadata.read_catalog_object_by_uuid(uuid);
    } catch (const std::exception& exception) {
        result.status = CatalogObjectReadStatus::Unavailable;
        result.object.reset();
        result.diagnostic = std::string("catalog object read failed: ") +
            exception.what();
        return result;
    } catch (...) {
        result.status = CatalogObjectReadStatus::Unavailable;
        result.object.reset();
        result.diagnostic =
            "catalog object read failed with an unknown exception";
        return result;
    }
    if (!source_is_current()) {
        result.status = CatalogObjectReadStatus::Unavailable;
        result.object.reset();
        result.diagnostic =
            "GDB source changed while reading the catalog object";
    }
    return result;
}

bool Reader::source_is_current() const noexcept {
    return state_ != nullptr && state_->source_is_current();
}

std::optional<Layer> Reader::open_layer(const std::string& layer_name,
                                        ReaderError* error) const {
    if (state_ == nullptr || state_->resolver == nullptr) {
        set_error(error, ReaderStatus::CatalogResolveFailed,
                  "Reader is not open; call Reader::open with a valid .gdb directory");
        return std::nullopt;
    }
    if (!source_is_current()) {
        set_error(error, ReaderStatus::SourceChanged,
                  "GDB source changed after Reader::open; discard this Reader and reopen it");
        return std::nullopt;
    }
    const auto resolved = state_->resolver->resolve(layer_name);
    if (!resolved) {
        set_error(error, ReaderStatus::LayerNotFound,
                  "layer not found: " + layer_name +
                      "; use Reader::layer_names() to select an available layer");
        return std::nullopt;
    }

    ResolvedTable resolved_layer = *resolved;
    resolved_layer.has_spatial_index =
        state_->spatial_index_declared(layer_name);

    auto engine = std::make_unique<QueryEngine>(
        *state_->catalog, resolved_layer, state_->query_preparation_cache);
    if (!engine->open()) {
        set_error(error, ReaderStatus::LayerOpenFailed,
                  "failed to open layer: " + layer_name +
                      "; verify table, tablx and geometry metadata");
        return std::nullopt;
    }
    if (!source_is_current()) {
        set_error(error, ReaderStatus::SourceChanged,
                  "GDB source changed while opening the layer; reopen the Reader");
        return std::nullopt;
    }
    if (engine->table() == nullptr ||
        engine->table()->feature_count() > options_.max_fid_slots) {
        set_error(error, ReaderStatus::FidRangeUnsupported,
                  "layer exceeds the configured logical FID slot limit: " +
                      layer_name);
        return std::nullopt;
    }

    set_error(error, ReaderStatus::Ok, {});
    return Layer(state_, std::move(resolved_layer), std::move(engine));
}

}  // namespace explorgdb
