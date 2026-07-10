#ifndef EXPLORGDB_QUERY_ENGINE_H
#define EXPLORGDB_QUERY_ENGINE_H

#include "capability_report.h"
#include "gdb_attribute_index.h"
#include "gdb_table.h"
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
    WhereClause
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
};

struct QueryResult {
    std::vector<uint32_t> matched_fids;
    std::optional<FeatureRecord> record;
    std::string execution_path;
    std::string fallback_reason;
};

class QueryEngine {
public:
    QueryEngine(const GdbCatalog& catalog, const ResolvedTable& table);

    bool open();
    QueryResult query(const QueryRequest& request);
    bool read_by_fid(uint32_t fid, FeatureRecord& record);
    uint64_t scan(GdbTableParser::ScanCallback callback);

    // Spatial query: .spx candidate lookup followed by exact geometry/bbox filtering.
    // Missing or unparseable .spx falls back to sequential probing; a valid empty
    // index result remains an empty result and must not trigger a full scan.
    std::vector<uint32_t> query_bbox(double xmin, double ymin, double xmax, double ymax);

    std::vector<uint32_t> query_attribute_double(const std::string& index_name,
                                                 double value, AttrOp op);
    std::vector<uint32_t> query_attribute_string(const std::string& index_name,
                                                 const std::string& value, AttrOp op);

    bool peek_bbox_source(uint32_t fid, const uint8_t*& blob, size_t& size);

    static bool should_fallback_spatial_index(bool spx_exists, bool spx_parse_ok) {
        return !spx_exists || !spx_parse_ok;
    }

    const CapabilityReport& capabilities() const { return capabilities_; }
    const GdbTableParser* table() const { return parser_.get(); }

private:
    const FieldDescriptor* geometry_field() const;
    bool feature_intersects(uint32_t fid, double xmin, double ymin,
                            double xmax, double ymax);
    QueryResult query_sequential_scan() const;
    QueryResult query_spatial(const QueryRequest& request);
    QueryResult query_attribute(const QueryRequest& request);
    QueryResult query_where(const QueryRequest& request);

    const GdbCatalog& catalog_;
    ResolvedTable resolved_;
    std::unique_ptr<GdbTableParser> parser_;
    CapabilityReport capabilities_;
};

} // namespace explorgdb

#endif
