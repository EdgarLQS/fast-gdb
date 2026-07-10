#ifndef EXPLORGDB_QUERY_ENGINE_H
#define EXPLORGDB_QUERY_ENGINE_H

#include "capability_report.h"
#include "gdb_attribute_index.h"
#include "gdb_table.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace explorgdb {

class QueryEngine {
public:
    QueryEngine(const GdbCatalog& catalog, const ResolvedTable& table);

    bool open();
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

    const GdbCatalog& catalog_;
    ResolvedTable resolved_;
    std::unique_ptr<GdbTableParser> parser_;
    CapabilityReport capabilities_;
};

} // namespace explorgdb

#endif
