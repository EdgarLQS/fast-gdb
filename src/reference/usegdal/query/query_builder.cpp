// src/query_builder.cpp — GdbQuery 链式查询构建器实现
//
// 所有链式方法都是简单的 setter 返回 *this，无复杂逻辑。
// toString() 生成可读的调试输出，包含空间关系名称。

#include "query_builder.h"
#include <sstream>

/** 设置属性过滤。直接赋值，支持链式返回。 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
GdbQuery& GdbQuery::where(const std::string& attributeFilter) {
    m_where = attributeFilter;
    return *this;
}

/**
 * 设置空间过滤和关系类型。
 * 仅持有指针不拷贝，调用方须保证几何在 query() 执行期间有效。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
GdbQuery& GdbQuery::spatial(const OGRGeometry* geom, GdbSpatialRelation relation) {
    m_spatialFilter = geom;
    m_spatialRelation = relation;
    return *this;
}

/** 设置结果数量限制。-1 表示无限制。 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
GdbQuery& GdbQuery::limit(int64_t count) {
    m_limit = count;
    return *this;
}

/** 设置结果偏移量。 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
GdbQuery& GdbQuery::offset(int64_t start) {
    m_offset = start;
    return *this;
}

bool GdbQuery::isEmpty() const {
    return m_where.empty() && m_spatialFilter == nullptr;
}

std::string GdbQuery::toString() const {
    std::ostringstream ss;
    ss << "GdbQuery{";
    bool first = true;
    if (!m_where.empty()) {
        ss << "where=\"" << m_where << "\"";
        first = false;
    }
    if (m_spatialFilter) {
        if (!first) ss << ", ";
        ss << "spatial=" << m_spatialFilter->getGeometryName();
        switch (m_spatialRelation) {
            case GdbSpatialRelation::Intersects: ss << "(Intersects)"; break;
            case GdbSpatialRelation::Contains:   ss << "(Contains)"; break;
            case GdbSpatialRelation::Within:     ss << "(Within)"; break;
            case GdbSpatialRelation::Disjoint:   ss << "(Disjoint)"; break;
        }
        first = false;
    }
    if (m_limit >= 0) {
        if (!first) ss << ", ";
        ss << "limit=" << m_limit;
        first = false;
    }
    if (m_offset > 0) {
        if (!first) ss << ", ";
        ss << "offset=" << m_offset;
    }
    ss << "}";
    return ss.str();
}
