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
    FidRangeUnsupported,
    MetadataReadFailed
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
    std::vector<DomainInfo> workspace_domains;
    std::vector<RelationshipSummary> relationships;
    std::vector<RelationshipClassDefinition> relationship_definitions;
    std::vector<DatasetGroupSummary> dataset_groups;
    std::vector<MetadataTableAudit> system_table_audit;
    std::vector<SubtypeInfo> subtypes;
    CapabilityReport capabilities;
};

struct MetadataReadResult {
    LayerMetadataSnapshot snapshot;
    ReaderError error;

    bool ok() const noexcept { return error.ok(); }
};

namespace detail {
struct ReaderState;
}

class Layer {
public:
    /** 移动构造图层句柄并转移查询资源。
     * @param other 被移动的图层；移动后不再拥有资源。
     */
    Layer(Layer&&) noexcept;
    Layer& operator=(Layer&&) noexcept = delete;
    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;
    ~Layer();

    /** 获取图层名称。
     * @return 图层名称的只读引用。
     */
    const std::string& name() const noexcept { return resolved_.name; }
    /** 获取图层字段描述。
     * @return 字段描述数组的只读引用。
     */
    const std::vector<FieldDescriptor>& fields() const;
    /** 获取图层能力报告。
     * @return 能力报告的只读引用。
     */
    const CapabilityReport& capabilities() const;
    /** 读取图层元数据。
     * @return 包含元数据快照或错误状态的结果。
     */
    MetadataReadResult read_metadata() const;
    /** 获取已缓存的图层元数据快照。
     * @return 图层元数据快照。
     */
    LayerMetadataSnapshot metadata_snapshot() const;

    /** 执行一次物化查询。
     * @param request 查询类型、过滤条件和结果限制。
     * @return 包含查询状态和结果记录的对象。
     */
    QueryResult query(const QueryRequest& request);
    /** 创建图层查询游标。
     * @param request 查询类型、过滤条件和结果限制。
     * @return 流式查询游标。
     */
    FeatureCursor open_cursor(const QueryRequest& request);
    /** 按 FID 读取一条记录。
     * @param fid 要读取的零基 FID。
     * @param record 接收字段值的输出对象。
     * @return 读取成功时返回 true。
     */
    bool read_by_fid(uint32_t fid, FeatureRecord& record);
    /** 获取当前有效要素数量。
     * @return 当前快照中的有效要素数。
     */
    std::size_t active_feature_count() const noexcept;
    /** 读取原始记录及其字节内容。
     * @param fid 要读取的零基 FID。
     * @param record 接收解析后字段值的输出对象。
     * @param raw_record 接收原始记录字节的输出数组。
     * @return 读取成功时返回 true。
     */
    bool read_raw_by_fid(uint32_t fid,
                         FeatureRecord& record,
                         std::vector<uint8_t>& raw_record);
    /** 按最大字节数限制读取原始记录。
     * @param fid 要读取的零基 FID。
     * @param raw_record 接收原始记录字节的输出数组。
     * @param max_bytes 允许返回的最大字节数。
     * @param limit_exceeded 可选输出，记录是否因大小限制截断/拒绝。
     * @return 读取成功且未超过限制时返回 true。
     */
    bool read_raw_by_fid(uint32_t fid,
                         std::vector<uint8_t>& raw_record,
                         std::size_t max_bytes,
                         bool* limit_exceeded = nullptr);
    /** 检查图层对应的数据源是否仍是打开时的快照。
     * @return 数据源未发生变化时返回 true。
     */
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

    /** 打开一个 FileGDB 数据源。
     * @param gdb_path FileGDB 目录路径。
     * @param options FID 范围等读取约束。
     * @param error 可选输出，接收失败原因。
     * @return 打开成功时返回 Reader，否则返回空值。
     */
    static std::optional<Reader> open(
        const std::string& gdb_path,
        ReaderOptions options = {},
        ReaderError* error = nullptr);

    /** 获取数据源路径。
     * @return 路径的只读引用。
     */
    const std::string& path() const noexcept { return path_; }
    /** 获取打开选项。
     * @return 选项的只读引用。
     */
    const ReaderOptions& options() const noexcept { return options_; }
    /** 获取最近一次错误状态。
     * @return 错误对象的只读引用。
     */
    const ReaderError& error() const noexcept { return error_; }
    /** 检查数据源是否仍保持打开时的快照。
     * @return 数据源未变化时返回 true。
     */
    bool source_is_current() const noexcept;
    /** 列出数据源中的图层名称。
     * @return 图层名称列表。
     */
    std::vector<std::string> layer_names() const;
    /** 按名称打开一个图层。
     * @param layer_name 目标图层名称。
     * @param error 可选输出，接收失败原因。
     * @return 打开成功时返回图层句柄，否则返回空值。
     */
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
