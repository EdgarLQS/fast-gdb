// src/query_parameter.cpp — GdbQueryParameter 实现
//
// setSpatialFilterRect 是唯一有逻辑的方法：内部构造 OGRPolygon 矩形，
// 顺时针环绕（左下→右下→右上→左上→闭合），由 unique_ptr 管理。

#include "query_parameter.h"

/**
 * setSpatialFilterRect() — 使用矩形范围设置空间过滤。
 *
 * 实现策略：
 * 1. 创建 OGRPolygon 和 OGRLinearRing
 * 2. 按顺时针添加 4 个顶点（左下→右下→右上→左上）
 * 3. closeRings() 闭合环
 * 4. 将环添加到多边形
 * 5. m_rectGeom 持有所有权，m_spatialFilter 指向它
 *
 * 注意：后续调用 setSpatialFilter() 会覆盖 m_spatialFilter 指针，
 * 但 m_rectGeom 仍保留，直到下次调用 setSpatialFilterRect() 被替换。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
void GdbQueryParameter::setSpatialFilterRect(double minX, double minY, double maxX, double maxY) {
    // 内部构造矩形多边形作为空间过滤几何
    m_rectGeom = std::make_unique<OGRPolygon>();
    auto* ring = new OGRLinearRing();
    ring->addPoint(minX, minY);
    ring->addPoint(maxX, minY);
    ring->addPoint(maxX, maxY);
    ring->addPoint(minX, maxY);
    ring->closeRings();
    static_cast<OGRPolygon*>(m_rectGeom.get())->addRing(ring);
    m_spatialFilter = m_rectGeom.get();
}

std::string GdbQueryParameter::toString() const {
    std::string result = "GdbQueryParameter{";
    if (!m_attrFilter.empty()) {
        result += "filter=\"" + m_attrFilter + "\"";
    }
    if (m_spatialFilter) {
        if (!m_attrFilter.empty()) result += ", ";
        result += "spatial=" + std::string(m_spatialFilter->getGeometryName());
    }
    if (result == "GdbQueryParameter{") {
        result += "empty";
    }
    result += "}";
    return result;
}
