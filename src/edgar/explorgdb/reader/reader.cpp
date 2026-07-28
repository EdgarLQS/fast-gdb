#include "reader.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"

#include <filesystem>
#include <utility>

namespace explorgdb {
namespace detail {

struct ReaderState {
    std::shared_ptr<GdbCatalog> catalog;
    std::shared_ptr<CatalogResolver> resolver;
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

QueryResult Layer::query(const QueryRequest& request) {
    return engine_ != nullptr ? engine_->query(request) : QueryResult{};
}

FeatureCursor Layer::open_cursor(const QueryRequest& request) {
    if (engine_ != nullptr) return engine_->open_cursor(request);
    return FeatureCursor{};
}

bool Layer::read_by_fid(uint32_t fid, FeatureRecord& record) {
    return engine_ != nullptr && engine_->read_by_fid(fid, record);
}

std::optional<Reader> Reader::open(const std::string& gdb_path,
                                   ReaderOptions options,
                                   ReaderError* error) {
    if (gdb_path.empty()) {
        set_error(error, ReaderStatus::InvalidArgument,
                  "GDB path must not be empty; provide an existing .gdb directory");
        return std::nullopt;
    }
    const std::filesystem::path path(gdb_path);
    if (!std::filesystem::is_directory(path)) {
        set_error(error, ReaderStatus::SourceNotFound,
                  "GDB path is not a directory: " + gdb_path +
                      "; check the source path and retry");
        return std::nullopt;
    }

    auto state = std::make_shared<detail::ReaderState>();
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

std::optional<Layer> Reader::open_layer(const std::string& layer_name,
                                        ReaderError* error) const {
    if (state_ == nullptr || state_->resolver == nullptr) {
        set_error(error, ReaderStatus::CatalogResolveFailed,
                  "Reader is not open; call Reader::open with a valid .gdb directory");
        return std::nullopt;
    }
    const auto resolved = state_->resolver->resolve(layer_name);
    if (!resolved) {
        set_error(error, ReaderStatus::LayerNotFound,
                  "layer not found: " + layer_name +
                      "; use Reader::layer_names() to select an available layer");
        return std::nullopt;
    }

    auto engine = std::make_unique<QueryEngine>(*state_->catalog, *resolved);
    if (!engine->open()) {
        set_error(error, ReaderStatus::LayerOpenFailed,
                  "failed to open layer: " + layer_name +
                      "; verify table, tablx and geometry metadata");
        return std::nullopt;
    }
    if (engine->table() == nullptr ||
        engine->table()->feature_count() > options_.max_fid_slots) {
        set_error(error, ReaderStatus::FidRangeUnsupported,
                  "layer exceeds the supported uint32_t FID slot range: " +
                      layer_name + "; use a source with no more than UINT32_MAX slots");
        return std::nullopt;
    }

    set_error(error, ReaderStatus::Ok, {});
    return Layer(state_, *resolved, std::move(engine));
}

}  // namespace explorgdb
