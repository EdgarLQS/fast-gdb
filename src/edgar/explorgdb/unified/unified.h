// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

#ifndef FAST_GDB_UNIFIED_H
#define FAST_GDB_UNIFIED_H

#include "routing.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fast_gdb {
namespace unified {

using Fid = std::int64_t;

template <typename T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : error_(std::move(error)) {}

    explicit operator bool() const noexcept { return value_.has_value(); }
    T& value() & { return *value_; }
    const T& value() const& { return *value_; }
    T&& value() && { return std::move(*value_); }
    const Error& error() const noexcept { return error_; }

private:
    std::optional<T> value_;
    Error error_;
};

enum class ResultOrder {
    Native,
    FidAscending,
};

enum class RemoteSourcePolicy {
    ImmutablePrefixRequired,
    AllowMutableUnverified,
};

enum class ConcurrentReadPolicy {
    SourceBusy,
    GdalUnverified,
};

enum class Consistency {
    LocalSnapshot,
    UnverifiedConcurrentRead,
    ImmutablePrefixAssumed,
    RemoteUnverified,
};

enum class FieldType {
    Integer,
    Integer64,
    Real,
    String,
    Binary,
    Date,
    Time,
    DateTime,
};

enum class ValueState {
    Unset,
    Null,
    Value,
};

enum class GeometryState {
    Unset,
    Null,
    Empty,
    Value,
};

struct DateValue {
    std::string iso8601;
};

struct TimeValue {
    std::string iso8601;
};

struct DateTimeValue {
    std::string iso8601;
    std::optional<std::int16_t> utc_offset_minutes;
};

using Value = std::variant<std::int32_t, std::int64_t, double, std::string,
                           std::vector<std::uint8_t>, DateValue, TimeValue,
                           DateTimeValue>;

struct Field {
    ValueState state = ValueState::Unset;
    std::optional<Value> value;
};

struct FieldDefinition {
    std::string name;
    std::string alias;
    FieldType type = FieldType::String;
    bool nullable = false;
    std::optional<std::string> default_value;
    std::optional<std::string> domain_name;
};

struct Geometry {
    GeometryState state = GeometryState::Unset;
    std::vector<std::uint8_t> wkb;
    std::uint32_t type = 0;
    std::int32_t srid = 0;
    std::string srs_wkt;
    bool has_z = false;
    bool has_m = false;
};

struct Feature {
    Fid fid = -1;
    std::vector<Field> fields;
    Geometry geometry;
};

struct LayerSchema {
    std::string name;
    std::vector<FieldDefinition> fields;
    std::optional<std::size_t> geometry_field;
    std::uint32_t geometry_type = 0;
    std::string srs_wkt;
};

struct Envelope {
    double xmin = 0;
    double ymin = 0;
    double xmax = 0;
    double ymax = 0;
};

struct OpenOptions {
    BackendPreference backend = BackendPreference::Auto;
    ConcurrentReadPolicy concurrent_read =
        ConcurrentReadPolicy::SourceBusy;
    RemoteSourcePolicy remote_source =
        RemoteSourcePolicy::AllowMutableUnverified;
    bool include_system_tables = false;
};

struct ConsistencyReport {
    Consistency consistency = Consistency::LocalSnapshot;
    std::uint64_t generation = 0;
};

struct QueryReport {
    std::string execution_path;
    std::uint64_t feature_count = 0;
    std::uint64_t materialized_bytes = 0;
};

struct CapabilityReport {
    bool streaming = true;
    bool attribute_filter = true;
    bool spatial_bbox = true;
    bool field_projection = true;
    bool fid_64 = true;
    bool native_extensions = false;
};

struct QueryPlan {
    Backend backend = Backend::None;
    std::string execution_path;
    bool materializes_fids = false;
    bool fallback_possible_before_publication = false;
};

struct Query {
    std::vector<std::string> projected_fields;
    std::string attribute_filter;
    std::optional<Envelope> spatial_filter;
    std::uint64_t offset = 0;
    std::uint64_t limit = 0;
    ResultOrder order = ResultOrder::Native;
    std::uint64_t max_ordered_fid_bytes = 64ULL * 1024 * 1024;
    std::function<bool()> cancel_requested;
    std::optional<std::chrono::steady_clock::time_point> deadline;
};

struct ReadAllOptions {
    std::uint64_t max_features = 1'000'000;
    std::uint64_t max_materialized_bytes = 512ULL * 1024 * 1024;
    std::uint64_t max_feature_bytes = 64ULL * 1024 * 1024;
    bool unlimited = false;
};

struct ReadBatch {
    std::vector<Feature> features;
    BackendReport backend_report;
    std::uint64_t materialized_bytes = 0;
};

struct NativeReadLimits {
    std::uint64_t max_raw_bytes = 64ULL * 1024 * 1024;
    std::uint64_t max_materialized_bytes = 64ULL * 1024 * 1024;
};

struct NativeFieldDescriptor {
    std::string name;
    std::string physical_type;
    std::uint32_t width = 0;
    std::uint8_t flags = 0;
    std::vector<std::uint8_t> default_value_raw;
};

struct FastNativeFeature {
    Fid fid = -1;
    std::uint32_t row_slot = 0;
    std::vector<NativeFieldDescriptor> descriptors;
    std::vector<std::uint8_t> raw_record;
};

struct LayerInfo {
    std::string name;
    std::string path;
};

struct GroupInfo {
    std::string name;
    std::string path;
};

namespace detail {
struct DatasetState;
struct LayerState;
struct GroupState;
struct CursorState;
}

class FAST_GDB_RUNTIME_API FeatureCursor {
public:
    /** 创建空游标。
     * @return 未绑定数据源的游标对象。
     */
    FeatureCursor();
    FeatureCursor(FeatureCursor&&) noexcept;
    FeatureCursor& operator=(FeatureCursor&&) noexcept;
    FeatureCursor(const FeatureCursor&) = delete;
    FeatureCursor& operator=(const FeatureCursor&) = delete;
    ~FeatureCursor();

    /** 读取下一条要素。
     * @return 有下一条记录时返回 Feature，正常结束时返回空 optional，失败时返回 Error。
     */
    Result<std::optional<Feature>> next();
    /** 获取后端选择和回退信息。
     * @return 后端报告的只读引用。
     */
    const BackendReport& backend_report() const noexcept;
    /** 获取读取一致性报告。
     * @return 一致性报告的只读引用。
     */
    const ConsistencyReport& consistency_report() const noexcept;
    /** 获取游标查询统计。
     * @return 查询报告的只读引用。
     */
    const QueryReport& query_report() const noexcept;
    /** 关闭游标并释放底层资源。
     * @return 无返回值；重复调用安全。
     */
    void close() noexcept;

    // Runtime factory seam; detail::CursorState is intentionally incomplete
    // to public consumers.
    explicit FeatureCursor(std::unique_ptr<detail::CursorState> state);

private:
    std::unique_ptr<detail::CursorState> state_;
    friend class Layer;
};

class FAST_GDB_RUNTIME_API FastLayerExtensions {
public:
    /** 按 FID 读取 fast-gdb 原生扩展数据。
     * @param fid 要读取的 FID。
     * @param limits 原始记录和物化结果大小限制。
     * @return 原生字段描述和原始记录，失败时返回 Error。
     */
    Result<FastNativeFeature> read_native_by_fid(
        Fid fid, NativeReadLimits limits = {}) const;

private:
    explicit FastLayerExtensions(std::shared_ptr<detail::LayerState> state);
    std::shared_ptr<detail::LayerState> state_;
    friend class Layer;
};

class FAST_GDB_RUNTIME_API Layer {
public:
    Layer() = default;

    /** 获取图层 Schema 快照。
     * @return Schema 的只读引用。
     */
    const LayerSchema& schema() const noexcept;
    /** 按 FID 读取一条要素。
     * @param fid 要读取的 FID。
     * @return 要素对象；找不到或读取失败时返回 Error。
     */
    Result<Feature> read_by_fid(Fid fid) const;
    /** 按查询条件一次性读取要素。
     * @param query 属性、空间、分页和排序条件。
     * @param options 结果数量和内存限制。
     * @return 物化批次及后端统计。
     */
    Result<ReadBatch> read_all(const Query& query = {},
                               ReadAllOptions options = {}) const;
    /** 创建流式要素游标。
     * @param query 查询条件。
     * @return 游标对象；打开失败时返回 Error。
     */
    Result<FeatureCursor> open_cursor(const Query& query = {}) const;
    /** 获取当前图层能力。
     * @return 能力报告；失败时返回 Error。
     */
    Result<CapabilityReport> capabilities() const;
    /** 解释查询计划而不执行查询。
     * @param query 待解释的查询条件。
     * @return 后端、执行路径和物化策略。
     */
    Result<QueryPlan> explain(const Query& query) const;
    /** 获取 fast-gdb 原生扩展接口。
     * @return 原生扩展句柄；不支持时返回 Error。
     */
    Result<FastLayerExtensions> fast_extensions() const;
    /** 获取后端选择报告。
     * @return 后端报告的只读引用。
     */
    const BackendReport& backend_report() const noexcept;
    /** 获取读取一致性报告。
     * @return 一致性报告的只读引用。
     */
    const ConsistencyReport& consistency_report() const noexcept;

private:
    explicit Layer(std::shared_ptr<detail::LayerState> state);
    std::shared_ptr<detail::LayerState> state_;
    friend class Dataset;
    friend class Group;
};

class FAST_GDB_RUNTIME_API Group {
public:
    /** 枚举子分组。
     * @return 子分组信息列表；失败时返回 Error。
     */
    Result<std::vector<GroupInfo>> groups() const;
    /** 枚举当前分组下的图层。
     * @return 图层信息列表；失败时返回 Error。
     */
    Result<std::vector<LayerInfo>> layers() const;
    /** 按名称打开子分组。
     * @param name 子分组名称。
     * @return 分组句柄；不存在或打开失败时返回 Error。
     */
    Result<Group> open_group(std::string_view name) const;

private:
    explicit Group(std::shared_ptr<detail::GroupState> state);
    std::shared_ptr<detail::GroupState> state_;
    friend class Dataset;
};

class FAST_GDB_RUNTIME_API Dataset {
public:
    /** 按 GDAL 风格 URI 打开数据源并选择后端。
     * @param uri 本地 FileGDB、S3 或 GDAL 风格路径。
     * @param options 后端、远程源和并发读取选项。
     * @return 数据集句柄；打开失败时返回 Error。
     */
    static Result<Dataset> open(std::string uri,
                                OpenOptions options = {});

    /** 获取根分组。
     * @return 根分组句柄；失败时返回 Error。
     */
    Result<Group> root_group() const;
    /** 获取数据源中的图层名称。
     * @return 图层名称列表；失败时返回 Error。
     */
    Result<std::vector<std::string>> layer_names() const;
    /** 按名称打开图层。
     * @param name 图层名称。
     * @return 图层句柄；不存在时返回 Error。
     */
    Result<Layer> open_layer(std::string_view name) const;
    /** 按层级路径打开图层。
     * @param path 分组/图层路径。
     * @return 图层句柄；路径不存在时返回 Error。
     */
    Result<Layer> open_layer_by_path(std::string_view path) const;
    /** 获取数据集后端报告。
     * @return 后端报告的只读引用。
     */
    const BackendReport& backend_report() const noexcept;
    /** 获取数据集一致性报告。
     * @return 一致性报告的只读引用。
     */
    const ConsistencyReport& consistency_report() const noexcept;

private:
    explicit Dataset(std::shared_ptr<detail::DatasetState> state);
    std::shared_ptr<detail::DatasetState> state_;
};

}  // namespace unified
}  // namespace fast_gdb

#endif
