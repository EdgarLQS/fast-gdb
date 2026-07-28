// src/edgar/explorgdb/reader/query_engine.h
// 查询引擎公开接口 — 统一 FID 查询、联合查询与 WKB-first FeatureCursor。

#ifndef EXPLORGDB_QUERY_ENGINE_H
#define EXPLORGDB_QUERY_ENGINE_H

#include "capability_report.h"
#include "gdb_attribute_index.h"
#include "gdb_spatial_index.h"
#include "gdb_table.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace explorgdb {

/** QueryRequest 的执行类型。 */
enum class QueryKind {
    ReadByFid,
    SequentialScan,
    SpatialBbox,
    AttributeDouble,
    AttributeString,
    WhereClause,
    SpatialWhere
};

/** 查询执行的结构化结果状态；fallback_reason 仅保留兼容诊断文本。 */
enum class QueryStatus {
    Ok,
    InvalidRequest,
    EngineNotOpen,
    CursorActive,
    Unsupported,
    SourceChanged,
    Cancelled,
    ResultLimitExceeded
};

const char* query_status_name(QueryStatus status) noexcept;

/** 查询参数；不同 QueryKind 只读取与自身相关的字段。 */
struct QueryRequest {
    QueryKind kind = QueryKind::SequentialScan;
    uint32_t fid = 0;
    double xmin = 0;
    double ymin = 0;
    double xmax = 0;
    double ymax = 0;
    std::string index_name;
    double double_value = 0;
    std::string string_value;
    std::string where_clause;
    AttrOp attr_op = AttrOp::Eq;

    // 空值表示返回全部字段；非空时只保证列出字段有值，其余槽位为 NULL，
    // FeatureRecord::materialized_fields 标明哪些槽位实际物化。
    std::optional<std::vector<size_t>> field_projection;
    size_t offset = 0;
    size_t limit = 0;
    size_t max_result_features = 0;
    bool sort_fids = false;
    std::function<bool()> cancel_requested;

    // 显式启用 Cursor 阶段计时；默认关闭以避免 steady_clock 热路径开销。
    bool profile_feature_reads = false;
};

/** 空间查询候选、精确过滤和回退路径的诊断指标。 */
struct SpatialQueryMetrics {
    size_t feature_count = 0;
    size_t candidate_count = 0;
    size_t bbox_rejected = 0;
    size_t bbox_contained = 0;
    size_t exact_tested = 0;
    size_t invalid_geometries = 0;
    double estimated_coverage = 0.0;
    bool spx_bypassed = false;
    bool geometry_only_scan = false;
    double candidate_ratio = 0.0;
    double candidate_lookup_ms = 0.0;
    double geometry_scan_ms = 0.0;
    double blob_lookup_ms = 0.0;
    double bbox_filter_ms = 0.0;
    double exact_filter_ms = 0.0;
    double total_ms = 0.0;
};

/** SpatialWhere 规划、索引和融合扫描的诊断指标。 */
struct CombinedQueryMetrics {
    size_t spatial_candidate_count = 0;
    size_t spatial_match_count = 0;
    size_t attribute_candidate_count = 0;
    size_t attribute_tested = 0;
    size_t final_match_count = 0;
    size_t fused_candidate_count = 0;

    size_t attribute_index_page_count = 0;
    size_t attribute_index_pages_visited = 0;
    size_t attribute_index_entries_scanned = 0;

    double spatial_ms = 0.0;
    double attribute_ms = 0.0;
    double intersection_ms = 0.0;
    double total_ms = 0.0;
    double fused_candidate_scan_ms = 0.0;

    double attribute_metadata_ms = 0.0;
    double attribute_index_file_load_ms = 0.0;
    double attribute_index_navigation_ms = 0.0;
    double attribute_index_scan_ms = 0.0;
    double attribute_candidate_order_ms = 0.0;
    double attribute_recheck_ms = 0.0;

    bool used_spatial_index = false;
    bool used_attribute_index = false;
    bool attribute_index_bypassed = false;
    bool fused_spatial_attribute_scan = false;
};

/** 仅在 QueryRequest::profile_feature_reads=true 时累计。 */
struct FeatureCursorMetrics {
    size_t feature_count = 0;
    double row_lookup_ms = 0.0;
    double field_materialization_ms = 0.0;
    double geometry_decode_ms = 0.0;
    double wkb_write_ms = 0.0;
};

/** 查询结果、执行路径和可选完整记录。 */
struct QueryResult {
    QueryStatus status = QueryStatus::Ok;
    std::string error;
    std::vector<uint32_t> matched_fids;
    std::optional<FeatureRecord> record;
    std::string execution_path;
    std::string fallback_reason;
    SpatialQueryMetrics spatial_metrics;
    CombinedQueryMetrics combined_metrics;
    FeatureCursorMetrics feature_cursor_metrics;
};

/** FeatureCursor 返回的完整对象；几何独立由 WKB-first GeometryValue 承载。 */
struct QueryFeature {
    uint32_t fid = 0;
    FeatureRecord record;
    GeometryValue geometry;
};

class QueryEngine;

/**
 * QueryEngine 的单活动游标。
 *
 * 游标可移动但不可复制；其生命周期占用 QueryEngine 的独占游标租约，避免
 * reopen 或另一个游标改变底层映射状态。
 */
class FeatureCursor {
public:
    FeatureCursor();
    static FeatureCursor failed(QueryResult result, std::string error);
    FeatureCursor(FeatureCursor&& other) noexcept;
    FeatureCursor& operator=(FeatureCursor&& other) noexcept;
    FeatureCursor(const FeatureCursor&) = delete;
    FeatureCursor& operator=(const FeatureCursor&) = delete;
    ~FeatureCursor();

    bool next(QueryFeature& feature);

    /**
     * 按零基 FID 重定位；下一次成功 next() 返回 FID >= 请求值的首条结果。
     * 支持前进、后退和任意跳转，move_to(0) 等价于回绕。
     */
    bool move_to(uint32_t fid);

    bool done() const noexcept;
    const QueryResult& query_result() const noexcept;
    const std::string& error() const noexcept;

private:
    class Impl;
    explicit FeatureCursor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class QueryEngine;
};

/** FileGDB 表上的查询规划与执行入口。 */
class QueryEngine {
public:
    QueryEngine(const GdbCatalog& catalog, const ResolvedTable& table);
    QueryEngine(const QueryEngine&) = delete;
    QueryEngine& operator=(const QueryEngine&) = delete;
    QueryEngine(QueryEngine&&) noexcept = default;
    QueryEngine& operator=(QueryEngine&&) noexcept = delete;

    bool open();
    QueryResult query(const QueryRequest& request);
    FeatureCursor open_cursor(const QueryRequest& request);
    bool read_by_fid(uint32_t fid, FeatureRecord& record);
    uint64_t scan(GdbTableParser::ScanCallback callback);

    std::vector<uint32_t> query_bbox(
        double xmin, double ymin, double xmax, double ymax,
        bool* skipped_unsupported_curve = nullptr);

    QueryResult query_bbox_unified(double xmin, double ymin,
                                   double xmax, double ymax);

    std::vector<uint32_t> query_attribute_double(
        const std::string& index_name, double value, AttrOp op);
    std::vector<uint32_t> query_attribute_string(
        const std::string& index_name,
        const std::string& value, AttrOp op);

    bool peek_bbox_source(uint32_t fid,
                          const uint8_t*& blob, size_t& size);

    static bool should_fallback_spatial_index(bool spx_exists,
                                               bool spx_parse_ok) {
        return !spx_exists || !spx_parse_ok;
    }

    const CapabilityReport& capabilities() const { return capabilities_; }
    GdbTableParser* table() { return parser_.get(); }
    const GdbTableParser* table() const { return parser_.get(); }

private:
    const FieldDescriptor* geometry_field() const;
    bool feature_intersects(uint32_t fid,
                            double xmin, double ymin,
                            double xmax, double ymax,
                            bool* skipped_unsupported_curve = nullptr);
    QueryResult query_sequential_scan(const QueryRequest& request) const;
    QueryResult query_spatial(const QueryRequest& request);
    QueryResult query_attribute(const QueryRequest& request);
    QueryResult query_where(const QueryRequest& request);
    QueryResult query_spatial_where(const QueryRequest& request);

    uint64_t register_feature_cursor() noexcept;
    void release_feature_cursor(uint64_t generation) noexcept;
    bool feature_cursor_active() const noexcept;

    /** 引擎重开代次与单游标租约状态。 */
    struct CursorControl {
        uint64_t open_generation = 0;
        uint64_t next_cursor_generation = 0;
        uint64_t active_cursor_generation = 0;

        uint64_t register_feature_cursor() noexcept;
        void release_feature_cursor(uint64_t generation) noexcept;
        bool feature_cursor_active() const noexcept;
    };
    std::unique_ptr<CursorControl> cursor_control_;

    const GdbCatalog& catalog_;
    ResolvedTable resolved_;
    std::unique_ptr<GdbTableParser> parser_;
    std::unique_ptr<GdbSpatialIndexParser> spatial_index_;
    bool opened_ = false;
    bool spatial_index_initialized_ = false;
    bool spatial_index_present_ = false;
    CapabilityReport capabilities_;
};

} // namespace explorgdb

#endif // EXPLORGDB_QUERY_ENGINE_H
