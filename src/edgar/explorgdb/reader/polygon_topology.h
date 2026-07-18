// src/edgar/explorgdb/reader/polygon_topology.h
// 多边形拓扑校验 — 环方向、自交检查、孔洞归属和面拓扑构造结果。

#ifndef EXPLORGDB_POLYGON_TOPOLOGY_H
#define EXPLORGDB_POLYGON_TOPOLOGY_H

#include "geometry_model.h"
#include <string>
#include <vector>

namespace explorgdb {

/** 点在环中的相对位置。 */
enum class PointRingLocation : uint8_t { Outside = 0, Inside = 1, Boundary = 2 };
/** 两条线段的空间关系。 */
enum class SegmentRelation : uint8_t { Disjoint = 0, Touch = 1, Cross = 2, Overlap = 3 };

/** 多边形拓扑校验选项。 */
struct PolygonTopologyOptions {
    bool validate_self_intersections = true;  // 检查自相交。
    bool reject_touching_rings = true;        // 拒绝仅相切的环（严格多边形规则）。
    bool normalize_orientation = true;        // 外环逆时针（CCW），内环顺时针（CW）。
};

/** 多边形拓扑构造结果。 */
struct PolygonTopologyResult {
    MultiPolygonModel model;
    TopologyStatus status = TopologyStatus::Valid;
    std::string diagnostic;
    bool valid() const { return status == TopologyStatus::Valid || status == TopologyStatus::Empty; }
};

/** 三点叉积方向判断（> 0 逆时针，< 0 顺时针，= 0 共线）。 */
int orientation(const GridPoint& a, const GridPoint& b, const GridPoint& c);
/** 点在线段上的投影判断。 */
bool point_on_segment(const GridPoint& p, const GridPoint& a, const GridPoint& b);
/** 两条线段的空间关系（相离/相切/交叉/重叠）。 */
SegmentRelation segment_relation(const GridPoint& a, const GridPoint& b,
                                 const GridPoint& c, const GridPoint& d);
/** 点在环中的位置（外部/内部/边界）。 */
PointRingLocation point_in_ring(const GridPoint& point, const RingModel& ring);

/** 多边形拓扑构造器：输入原始环序列，输出带孔洞归属的 MultiPolygonModel。 */
class PolygonTopologyBuilder {
public:
    explicit PolygonTopologyBuilder(PolygonTopologyOptions options = {});
    PolygonTopologyResult build(std::vector<PointSequence> rings) const;

private:
    PolygonTopologyOptions options_;
};

} // namespace explorgdb
#endif