#ifndef EXPLORGDB_GDAL_CURVE_BACKEND_H
#define EXPLORGDB_GDAL_CURVE_BACKEND_H

#include "geometry_model.h"

#include <cstdint>
#include <string>

namespace explorgdb {

struct GdalCurveRequest {
    std::string gdb_path;
    std::string layer_name;

    // This is the FID expected by GDAL/OGR. HybridGeometryReader performs the
    // configurable fast-gdb row-index -> GDAL FID mapping before calling the
    // bridge (the default mapping is row index + 1 for FileGDB ObjectID).
    int64_t fid = -1;

    bool source_was_curve = true;
    bool native_curve_wkb = false;
    double max_angle_step_degrees = 0.0;
};

struct GdalSpatialResult {
    bool matched = false;
    GeometryBackend backend = GeometryBackend::Gdal;
    GeometryStatus status = GeometryStatus::Valid;
    std::string diagnostic;

    bool valid() const {
        return status == GeometryStatus::Valid ||
               status == GeometryStatus::Empty;
    }
};

// The implementation owns a thread-local dataset/layer cache. Consequently
// mutable OGRLayer cursor state is never shared between worker threads and a
// dataset is not reopened for each fallback feature.
class GdalCurveBackendBridge {
public:
    GeometryValue read_geometry(const GdalCurveRequest& request) const;
    GdalSpatialResult intersects_bbox(const GdalCurveRequest& request,
                                       double xmin, double ymin,
                                       double xmax, double ymax) const;

    static void clear_thread_cache();
};

} // namespace explorgdb
#endif
