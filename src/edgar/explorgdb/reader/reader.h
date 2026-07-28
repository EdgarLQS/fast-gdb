#ifndef EXPLORGDB_READER_H
#define EXPLORGDB_READER_H

#include "metadata_reader.h"
#include "query_engine.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace explorgdb {

enum class ReaderStatus {
    Ok,
    InvalidArgument,
    SourceNotFound,
    CatalogScanFailed,
    CatalogResolveFailed,
    SourceSnapshotFailed,
    SourceChanged,
    LayerNotFound,
    LayerOpenFailed,
    FidRangeUnsupported
};

struct ReaderError {
    ReaderStatus status = ReaderStatus::Ok;
    std::string message;

    bool ok() const noexcept { return status == ReaderStatus::Ok; }
};

struct ReaderOptions {
    // fast-gdb currently exposes zero-based uint32_t FIDs. A source with more
    // slots is rejected instead of silently truncating the FID domain.
    size_t max_fid_slots = static_cast<size_t>(std::numeric_limits<uint32_t>::max());
};

/** Layer 对外可消费的只读元数据快照；不暴露 catalog 物理表路径细节。 */
struct LayerMetadataSnapshot {
    std::string name;
    std::vector<FieldDescriptor> fields;
    std::optional<LayerMetadata> layer;
    std::vector<FieldDomainBinding> field_domains;
    std::vector<RelationshipSummary> relationships;
    std::vector<DatasetGroupSummary> dataset_groups;
    CapabilityReport capabilities;
};

namespace detail {
struct ReaderState;
}

class Layer {
public:
    Layer(Layer&&) noexcept;
    Layer& operator=(Layer&&) noexcept = delete;
    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;
    ~Layer();

    const std::string& name() const noexcept { return resolved_.name; }
    const std::vector<FieldDescriptor>& fields() const;
    const CapabilityReport& capabilities() const;
    LayerMetadataSnapshot metadata_snapshot() const;

    QueryResult query(const QueryRequest& request);
    FeatureCursor open_cursor(const QueryRequest& request);
    bool read_by_fid(uint32_t fid, FeatureRecord& record);
    bool source_is_current() const noexcept;
    const MetadataReader& metadata() const noexcept { return metadata_; }

private:
    Layer(std::shared_ptr<detail::ReaderState> state,
          ResolvedTable resolved,
          std::unique_ptr<QueryEngine> engine);

    std::shared_ptr<detail::ReaderState> state_;
    ResolvedTable resolved_;
    std::unique_ptr<QueryEngine> engine_;
    MetadataReader metadata_;

    friend class Reader;
};

class Reader {
public:
    Reader() = default;
    Reader(Reader&&) noexcept = default;
    Reader& operator=(Reader&&) noexcept = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    static std::optional<Reader> open(
        const std::string& gdb_path,
        ReaderOptions options = {},
        ReaderError* error = nullptr);

    const std::string& path() const noexcept { return path_; }
    const ReaderOptions& options() const noexcept { return options_; }
    const ReaderError& error() const noexcept { return error_; }
    bool source_is_current() const noexcept;
    std::vector<std::string> layer_names() const;
    std::optional<Layer> open_layer(
        const std::string& layer_name,
        ReaderError* error = nullptr) const;

private:
    std::shared_ptr<detail::ReaderState> state_;
    std::string path_;
    ReaderOptions options_;
    ReaderError error_;
};

const char* reader_status_name(ReaderStatus status) noexcept;

}  // namespace explorgdb

#endif  // EXPLORGDB_READER_H
