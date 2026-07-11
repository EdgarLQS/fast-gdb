#ifndef EXPLORGDB_POLYGON_TOPOLOGY_H
#define EXPLORGDB_POLYGON_TOPOLOGY_H

#include "geometry_model.h"
#include <string>
#include <vector>

namespace explorgdb {

enum class PointRingLocation : uint8_t { Outside = 0, Inside = 1, Boundary = 2 };
enum class SegmentRelation : uint8_t { Disjoint = 0, Touch = 1, Cross = 2, Overlap = 3 };

struct PolygonTopologyOptions {
    bool validate_self_intersections = true;
    bool reject_touching_rings = true;
    bool normalize_orientation = true; // exterior CCW, holes CW
};

struct PolygonTopologyResult {
    MultiPolygonModel model;
    TopologyStatus status = TopologyStatus::Valid;
    std::string diagnostic;
    bool valid() const { return status == TopologyStatus::Valid || status == TopologyStatus::Empty; }
};

int orientation(const GridPoint& a, const GridPoint& b, const GridPoint& c);
bool point_on_segment(const GridPoint& p, const GridPoint& a, const GridPoint& b);
SegmentRelation segment_relation(const GridPoint& a, const GridPoint& b,
                                 const GridPoint& c, const GridPoint& d);
PointRingLocation point_in_ring(const GridPoint& point, const RingModel& ring);

class PolygonTopologyBuilder {
public:
    explicit PolygonTopologyBuilder(PolygonTopologyOptions options = {});
    PolygonTopologyResult build(std::vector<PointSequence> rings) const;

private:
    PolygonTopologyOptions options_;
};

} // namespace explorgdb
#endif
