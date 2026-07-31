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
        RemoteSourcePolicy::ImmutablePrefixRequired;
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

class FeatureCursor {
public:
    FeatureCursor();
    FeatureCursor(FeatureCursor&&) noexcept;
    FeatureCursor& operator=(FeatureCursor&&) noexcept;
    FeatureCursor(const FeatureCursor&) = delete;
    FeatureCursor& operator=(const FeatureCursor&) = delete;
    ~FeatureCursor();

    Result<std::optional<Feature>> next();
    const BackendReport& backend_report() const noexcept;
    const QueryReport& query_report() const noexcept;
    void close() noexcept;

    // Runtime factory seam; detail::CursorState is intentionally incomplete
    // to public consumers.
    explicit FeatureCursor(std::unique_ptr<detail::CursorState> state);

private:
    std::unique_ptr<detail::CursorState> state_;
    friend class Layer;
};

class FastLayerExtensions {
public:
    Result<FastNativeFeature> read_native_by_fid(
        Fid fid, NativeReadLimits limits = {}) const;

private:
    explicit FastLayerExtensions(std::shared_ptr<detail::LayerState> state);
    std::shared_ptr<detail::LayerState> state_;
    friend class Layer;
};

class Layer {
public:
    Layer() = default;

    const LayerSchema& schema() const noexcept;
    Result<Feature> read_by_fid(Fid fid) const;
    Result<ReadBatch> read_all(const Query& query = {},
                               ReadAllOptions options = {}) const;
    Result<FeatureCursor> open_cursor(const Query& query = {}) const;
    Result<CapabilityReport> capabilities() const;
    Result<QueryPlan> explain(const Query& query) const;
    Result<FastLayerExtensions> fast_extensions() const;
    const BackendReport& backend_report() const noexcept;
    const ConsistencyReport& consistency_report() const noexcept;

private:
    explicit Layer(std::shared_ptr<detail::LayerState> state);
    std::shared_ptr<detail::LayerState> state_;
    friend class Dataset;
    friend class Group;
};

class Group {
public:
    Result<std::vector<GroupInfo>> groups() const;
    Result<std::vector<LayerInfo>> layers() const;
    Result<Group> open_group(std::string_view name) const;

private:
    explicit Group(std::shared_ptr<detail::GroupState> state);
    std::shared_ptr<detail::GroupState> state_;
    friend class Dataset;
};

class Dataset {
public:
    static Result<Dataset> open(std::string uri,
                                OpenOptions options = {});

    Result<Group> root_group() const;
    Result<std::vector<std::string>> layer_names() const;
    Result<Layer> open_layer(std::string_view name) const;
    Result<Layer> open_layer_by_path(std::string_view path) const;
    const BackendReport& backend_report() const noexcept;
    const ConsistencyReport& consistency_report() const noexcept;

private:
    explicit Dataset(std::shared_ptr<detail::DatasetState> state);
    std::shared_ptr<detail::DatasetState> state_;
};

}  // namespace unified
}  // namespace fast_gdb

#endif
