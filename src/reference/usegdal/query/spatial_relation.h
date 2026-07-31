// src/spatial_relation.h — GdbSpatialRelation 空间关系枚举

#ifndef GDB_SPATIAL_RELATION_H
#define GDB_SPATIAL_RELATION_H

enum class GdbSpatialRelation {
    Intersects,   // 相交（默认，驱动层 bbox 过滤）
    Contains,     // 包含（后过滤：filterGeom->Contains(featureGeom)）
    Within,       // 在...内（后过滤：featureGeom->Within(filterGeom)）
    Disjoint,     // 不相交（后过滤：filterGeom->Disjoint(featureGeom)）
};

#endif // GDB_SPATIAL_RELATION_H
