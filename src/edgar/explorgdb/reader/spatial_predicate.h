#ifndef EXPLORGDB_SPATIAL_PREDICATE_H
#define EXPLORGDB_SPATIAL_PREDICATE_H

#include "geometry_model.h"
#include "polygon_topology.h"

namespace explorgdb {

struct QueryGridBbox {
    long double xmin = 0;
    long double ymin = 0;
    long double xmax = 0;
    long double ymax = 0;
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
