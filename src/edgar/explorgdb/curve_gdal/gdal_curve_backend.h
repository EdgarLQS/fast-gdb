#ifndef EXPLORGDB_GDAL_CURVE_BACKEND_H
#define EXPLORGDB_GDAL_CURVE_BACKEND_H

#include "geometry_model.h"

#include <cstdint>
#include <string>

namespace explorgdb {

struct GdalCurveRequest {
    std::string gdb_path;
    std::string layer_name;
    int64_t fid = -1;
    bool native_curve_wkb = false;
    double max_angle_step_degrees = 0.0;
};

struct GdalSpatialResult {
    bool matched = false;
    GeometryStatus status = GeometryStatus::Valid;
    std::string diagnostic;
    bool valid() const {
        return status == GeometryStatus::Valid ||
               status == GeometryStatus::Empty;
    }
};

// The implementation owns a thread-local dataset/layer cache. Consequently
// mutable OGRLayer cursor state is never shared between worker threads and a
// dataset is not reopened for each curve feature.
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
