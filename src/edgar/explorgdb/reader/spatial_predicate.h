#ifndef EXPLORGDB_SPATIAL_PREDICATE_H
#define EXPLORGDB_SPATIAL_PREDICATE_H

#include "geometry_model.h"
#include "polygon_topology.h"

namespace explorgdb {

// Query coordinates remain continuous in FileGDB grid space. Rounding these
// values to int64 would miss a segment that crosses a sub-cell query window
// even when no stored vertex lies inside that window.
struct QueryGridBbox {
    long double xmin = 0.0L;
    long double ymin = 0.0L;
    long double xmax = 0.0L;
    long double ymax = 0.0L;
};

enum class PointGeometryLocation : uint8_t {
    Outside = 0,
    Inside = 1,
    Boundary = 2
};

class SpatialPredicate {
public:
    static PointGeometryLocation locate_point(
        const MultiPolygonModel& polygon,
        const GridPoint& point);
    static bool intersects_bbox(const GeometryModel& geometry,
                                const QueryGridBbox& bbox);
};

} // namespace explorgdb
#endif
