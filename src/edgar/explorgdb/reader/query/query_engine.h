// src/edgar/explorgdb/reader/query/query_engine.h
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
    /** 创建空游标。
     * @return 未绑定查询结果的游标。
     */
    FeatureCursor();
    /** 创建失败状态的游标。
     * @param result 查询失败状态。
     * @param error 面向调用方的错误信息。
     * @return 已保存失败状态的游标。
     */
    static FeatureCursor failed(QueryResult result, std::string error);
    /** 移动构造游标并转移底层租约。
     * @param other 被移动的游标；移动后不再拥有底层资源。
     */
    FeatureCursor(FeatureCursor&& other) noexcept;
    /** 移动赋值并释放当前游标资源。
     * @param other 被移动的游标。
     * @return 当前游标对象的引用。
     */
    FeatureCursor& operator=(FeatureCursor&& other) noexcept;
    FeatureCursor(const FeatureCursor&) = delete;
    FeatureCursor& operator=(const FeatureCursor&) = delete;
    ~FeatureCursor();

    /** 读取下一条查询结果。
     * @param feature 接收要素 ID、属性和几何的输出对象。
     * @return 成功读取一条记录时返回 true，游标结束或失败时返回 false。
     */
    bool next(QueryFeature& feature);

    /**
     * 按零基 FID 重定位；下一次成功 next() 返回 FID >= 请求值的首条结果。
     * 支持前进、后退和任意跳转，move_to(0) 等价于回绕。
     */
    /** 将游标定位到不小于指定 FID 的下一条结果。
     * @param fid 目标零基 FID。
     * @return 定位成功时返回 true，否则返回 false。
     */
    bool move_to(uint32_t fid);

    /** 判断游标是否已经结束或失效。
     * @return 游标不可再读取时返回 true。
     */
    bool done() const noexcept;
    /** 获取查询结果状态。
     * @return 查询状态的只读引用。
     */
    const QueryResult& query_result() const noexcept;
    /** 获取游标错误信息。
     * @return 错误文本的只读引用；无错误时为空。
     */
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
    /** 创建指定 FileGDB 表的查询引擎。
     * @param catalog 已完成扫描的目录对象。
     * @param table 解析得到的目标表描述。
     */
    QueryEngine(const GdbCatalog& catalog, const ResolvedTable& table);
    QueryEngine(const QueryEngine&) = delete;
    QueryEngine& operator=(const QueryEngine&) = delete;
    QueryEngine(QueryEngine&&) noexcept = default;
    QueryEngine& operator=(QueryEngine&&) noexcept = delete;

    /** 打开目标表及其可用索引。
     * @return 打开成功时返回 true。
     */
    bool open();
    /** 按请求类型执行查询并返回物化结果。
     * @param request 查询类型、过滤条件和结果限制。
     * @return 包含状态、记录和诊断指标的查询结果。
     */
    QueryResult query(const QueryRequest& request);
    /** 创建单活动游标以流式读取查询结果。
     * @param request 查询类型、过滤条件和结果限制。
     * @return 查询游标；请求无效或引擎未打开时返回失败游标。
     */
    FeatureCursor open_cursor(const QueryRequest& request);
    /** 按 FID 读取单条记录。
     * @param fid 要读取的零基 FID。
     * @param record 接收属性字段的输出对象。
     * @return 找到并成功解析时返回 true。
     */
    bool read_by_fid(uint32_t fid, FeatureRecord& record);
    /** 顺序扫描表中的记录。
     * @param callback 每读取一条记录时调用的回调函数。
     * @return 实际扫描的记录数量。
     */
    uint64_t scan(GdbTableParser::ScanCallback callback);

    /** 使用空间索引或几何回退路径查询包围盒。
     * @param xmin 查询框最小 X。
     * @param ymin 查询框最小 Y。
     * @param xmax 查询框最大 X。
     * @param ymax 查询框最大 Y。
     * @param skipped_unsupported_curve 可选输出，记录是否跳过不支持曲线。
     * @return 命中的 FID 列表。
     */
    std::vector<uint32_t> query_bbox(
        double xmin, double ymin, double xmax, double ymax,
        bool* skipped_unsupported_curve = nullptr);

    /** 以统一 QueryResult 形式执行包围盒查询。
     * @param xmin 查询框最小 X。
     * @param ymin 查询框最小 Y。
     * @param xmax 查询框最大 X。
     * @param ymax 查询框最大 Y。
     * @return 包含状态、FID 和空间查询指标的结果。
     */
    QueryResult query_bbox_unified(double xmin, double ymin,
                                   double xmax, double ymax);

    /** 使用数值属性索引查询 FID。
     * @param index_name 属性索引名称。
     * @param value 比较值。
     * @param op 比较操作符。
     * @return 满足条件的 FID 列表。
     */
    std::vector<uint32_t> query_attribute_double(
        const std::string& index_name, double value, AttrOp op);
    /** 使用字符串属性索引查询 FID。
     * @param index_name 属性索引名称。
     * @param value 比较值。
     * @param op 比较操作符。
     * @return 满足条件的 FID 列表。
     */
    std::vector<uint32_t> query_attribute_string(
        const std::string& index_name,
        const std::string& value, AttrOp op);

    /** 获取指定 FID 的原始几何 blob，不复制数据。
     * @param fid 要读取的零基 FID。
     * @param blob 输出原始 blob 首地址，由引擎管理生命周期。
     * @param size 输出 blob 长度，单位为字节。
     * @return 成功定位几何数据时返回 true。
     */
    bool peek_bbox_source(uint32_t fid,
                          const uint8_t*& blob, size_t& size);

    static bool should_fallback_spatial_index(bool spx_exists,
                                               bool spx_parse_ok) {
        return !spx_exists || !spx_parse_ok;
    }

    /** 获取当前引擎能力报告。
     * @return 能力报告的只读引用。
     */
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
