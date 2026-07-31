// src/edgar/explorgdb/reader/geometry/polygon_topology.h
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

/** 计算三点方向的有符号叉积。
 * @param a 第一个点。
 * @param b 第二个点。
 * @param c 第三个点。
 * @return 大于 0 为逆时针，小于 0 为顺时针，0 为共线。
 */
int orientation(const GridPoint& a, const GridPoint& b, const GridPoint& c);
/** 判断点是否位于线段上。
 * @param p 待判断点。
 * @param a 线段起点。
 * @param b 线段终点。
 * @return 点在线段闭区间内时返回 true。
 */
bool point_on_segment(const GridPoint& p, const GridPoint& a, const GridPoint& b);
/** 计算两条线段的空间关系。
 * @param a 第一条线段起点。
 * @param b 第一条线段终点。
 * @param c 第二条线段起点。
 * @param d 第二条线段终点。
 * @return 相离、相切、交叉或重叠关系。
 */
SegmentRelation segment_relation(const GridPoint& a, const GridPoint& b,
                                 const GridPoint& c, const GridPoint& d);
/** 判断点在环中的位置。
 * @param point 待判断点。
 * @param ring 已规范化的环。
 * @return 外部、内部或边界位置。
 */
PointRingLocation point_in_ring(const GridPoint& point, const RingModel& ring);

/** 多边形拓扑构造器：输入原始环序列，输出带孔洞归属的 MultiPolygonModel。 */
class PolygonTopologyBuilder {
public:
    /** 创建多边形拓扑构造器。
     * @param options 自交、相切环和方向规范化选项。
     */
    explicit PolygonTopologyBuilder(PolygonTopologyOptions options = {});
    /** 组织原始环序列并构造多边形拓扑。
     * @param rings 输入的开放或闭合环序列，构造器会复制/规范化数据。
     * @return 带外环、内环归属和诊断状态的拓扑结果。
     */
    PolygonTopologyResult build(std::vector<PointSequence> rings) const;

private:
    PolygonTopologyOptions options_;
};

} // namespace explorgdb
#endif
