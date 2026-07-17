#ifndef EXPLORGDB_QUERY_ENGINE_H
#define EXPLORGDB_QUERY_ENGINE_H

#include "capability_report.h"
#include "gdb_attribute_index.h"
#include "gdb_spatial_index.h"
#include "gdb_table.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace explorgdb {

enum class QueryKind {
    ReadByFid,
    SequentialScan,
    SpatialBbox,
    AttributeDouble,
    AttributeString,
    WhereClause,
    SpatialWhere
};

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
    // Opt-in cursor stage timings. Default false keeps normal reads free of
    // steady_clock calls and avoids process-global profiling state.
    bool profile_feature_reads = false;
};

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

struct CombinedQueryMetrics {
    size_t spatial_candidate_count = 0;
    size_t spatial_match_count = 0;
    size_t attribute_candidate_count = 0;
    size_t attribute_tested = 0;
    size_t final_match_count = 0;
    double spatial_ms = 0.0;
    double attribute_ms = 0.0;
    double intersection_ms = 0.0;
    double total_ms = 0.0;
    bool used_spatial_index = false;
    bool used_attribute_index = false;
};

// Aggregated only when QueryRequest::profile_feature_reads is true.
struct FeatureCursorMetrics {
    size_t feature_count = 0;
    double row_lookup_ms = 0.0;
    double field_materialization_ms = 0.0;
    double geometry_decode_ms = 0.0;
    double wkt_write_ms = 0.0;
    double wkb_write_ms = 0.0;
};

struct QueryResult {
    std::vector<uint32_t> matched_fids;
    std::optional<FeatureRecord> record;
    std::string execution_path;
    std::string fallback_reason;
    SpatialQueryMetrics spatial_metrics;
    CombinedQueryMetrics combined_metrics;
    FeatureCursorMetrics feature_cursor_metrics;
};

struct QueryFeature {
    uint32_t fid = 0;
    FeatureRecord record;
    GeometryValue geometry;
};

class QueryEngine;

class FeatureCursor {
public:
    FeatureCursor(FeatureCursor&& other) noexcept;
    FeatureCursor& operator=(FeatureCursor&& other) noexcept;
    FeatureCursor(const FeatureCursor&) = delete;
    FeatureCursor& operator=(const FeatureCursor&) = delete;
    ~FeatureCursor();

    bool next(QueryFeature& feature);
    // Reposition by zero-based FID. The next successful next() returns the
    // first result whose FID is greater than or equal to the requested value.
    // This supports forward, backward and arbitrary jumps; move_to(0) rewinds.
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

class QueryEngine {
public:
    QueryEngine(const GdbCatalog& catalog, const ResolvedTable& table);
    QueryEngine(const QueryEngine&) = delete;
    QueryEngine& operator=(const QueryEngine&) = delete;
    QueryEngine(QueryEngine&&) = delete;
    QueryEngine& operator=(QueryEngine&&) = delete;

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
    QueryResult query_sequential_scan() const;
    QueryResult query_spatial(const QueryRequest& request);
    QueryResult query_attribute(const QueryRequest& request);
    QueryResult query_where(const QueryRequest& request);
    QueryResult query_spatial_where(const QueryRequest& request);

    uint64_t register_feature_cursor() noexcept;
    void release_feature_cursor(uint64_t generation) noexcept;
    bool feature_cursor_active() const noexcept {
        return active_cursor_generation_ != 0;
    }

    const GdbCatalog& catalog_;
    ResolvedTable resolved_;
    std::unique_ptr<GdbTableParser> parser_;
    std::unique_ptr<GdbSpatialIndexParser> spatial_index_;
    bool opened_ = false;
    bool spatial_index_initialized_ = false;
    bool spatial_index_present_ = false;
    CapabilityReport capabilities_;
    uint64_t open_generation_ = 0;
    uint64_t next_cursor_generation_ = 0;
    uint64_t active_cursor_generation_ = 0;
};

} // namespace explorgdb

#endif
