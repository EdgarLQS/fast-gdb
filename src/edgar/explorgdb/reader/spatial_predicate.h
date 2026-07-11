#ifndef EXPLORGDB_SPATIAL_PREDICATE_H
#define EXPLORGDB_SPATIAL_PREDICATE_H

#include "geometry_model.h"
#include "polygon_topology.h"

namespace explorgdb {

struct QueryGridBbox {
    int64_t xmin = 0;
    int64_t ymin = 0;
    int64_t xmax = 0;
    int64_t ymax = 0;
};

enum class PointGeometryLocation : uint8_t { Outside = 0, Inside = 1, Boundary = 2 };

class SpatialPredicate {
public:
    static PointGeometryLocation locate_point(const MultiPolygonModel& polygon,
                                              const GridPoint& point);
    static bool intersects_bbox(const GeometryModel& geometry,
                                const QueryGridBbox& bbox);
};

} // namespace explorgdb
#endif
