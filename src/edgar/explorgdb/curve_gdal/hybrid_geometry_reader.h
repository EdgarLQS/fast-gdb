#ifndef EXPLORGDB_HYBRID_GEOMETRY_READER_H
#define EXPLORGDB_HYBRID_GEOMETRY_READER_H

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "gdb_table.h"
#include "gdal_curve_backend.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace explorgdb {

struct HybridGeometryOptions {
    // fast-gdb row IDs are zero-based. OpenFileGDB commonly exposes the
    // ObjectID as a one-based GDAL FID, hence the default +1 mapping.
    // Set this to 0 for datasets/drivers whose FIDs already match row IDs.
    int64_t gdal_fid_offset = 1;

    bool prefer_gdal_for_curves = false;
    bool fallback_on_topology_error = true;
    bool native_curve_wkb = false;
    double max_angle_step_degrees = 0.0;
};

class HybridGeometryReader {
public:
    HybridGeometryReader(GdbTableParser& parser,
                         std::string gdb_path,
                         std::string layer_name,
                         HybridGeometryOptions options = {});

    GeometryValue read_geometry(uint32_t fast_fid) const;
    GdalSpatialResult intersects_bbox(uint32_t fast_fid,
                                      double xmin, double ymin,
                                      double xmax, double ymax) const;

    static bool map_gdal_fid(uint32_t fast_fid, int64_t offset,
                             int64_t& gdal_fid);

private:
    bool should_fallback(const GeometryModel& model) const;
    bool make_request(uint32_t fast_fid, bool source_was_curve,
                      GdalCurveRequest& request,
                      std::string& diagnostic) const;

    GdbTableParser& parser_;
    std::string gdb_path_;
    std::string layer_name_;
    HybridGeometryOptions options_;
    GdalCurveBackendBridge bridge_;
};

struct HybridQueryResult {
    std::vector<uint32_t> matched_fids;
    std::string execution_path;
    std::string diagnostic;
    size_t gdal_fallback_count = 0;
    size_t invalid_geometry_count = 0;
};

// Complete hybrid spatial path:
//   .spx candidate lookup -> fast-gdb GeometryModel exact predicate
//   -> cached GDAL fallback only for curves/topology failures.
class HybridQueryEngine {
public:
    HybridQueryEngine(const GdbCatalog& catalog,
                      ResolvedTable table,
                      HybridGeometryOptions options = {});

    bool open();
    HybridQueryResult query_bbox(double xmin, double ymin,
                                 double xmax, double ymax);

private:
    const GdbCatalog& catalog_;
    ResolvedTable resolved_;
    HybridGeometryOptions options_;
    std::unique_ptr<GdbTableParser> parser_;
};

} // namespace explorgdb
#endif
