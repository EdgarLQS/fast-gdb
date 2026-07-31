// src/query_builder.h — GdbQuery 链式查询构建器
//
// GdbQuery 提供流畅接口（fluent interface）构建查询条件：
//   GdbQuery q;
//   q.where("population > 1000000")
//    .spatial(&bbox, GdbSpatialRelation::Within)
//    .limit(100);
//
// 与 GdbQueryParameter 的区别：
// - GdbQuery：链式构建，支持 where/spatial/limit/offset 组合，用于 GdbDataset::query()
// - GdbQueryParameter：setter 风格，支持 bbox/rect 快捷空间过滤，用于 GdbDataset::query()
// 两者功能重叠，GdbQuery 更适合复杂查询，GdbQueryParameter 更适合简单场景。
//
// 空间关系支持：Intersects/Contains/Within/Disjoint
// Intersects 为默认值，通过 OGRLayer::SetSpatialFilter 做 bbox 预过滤；
// 其他关系在 Recordset::moveNext() 中逐要素后过滤。

#ifndef GDB_QUERY_BUILDER_H
#define GDB_QUERY_BUILDER_H

#include "ogrsf_frmts.h"
#include "spatial_relation.h"
#include <cstdint>
#include <memory>
#include <string>

/**
 * GdbQuery — 链式查询构建器。
 *
 * 使用流畅接口组合查询条件（where + spatial + limit + offset），
 * 传递给 GdbDataset::query() 执行。
 *
 * 空间过滤约定：
 * - spatial() 传入的 OGRGeometry* 指针不被 GdbQuery 拷贝或拥有
 * - 调用方必须保证几何对象在 query() 执行期间有效
 * - 如需长期持有，使用 GdbQueryParameter（内部 unique_ptr 管理）
 */
class GdbQuery {
public:
    GdbQuery() = default;

    // ========== 链式构建 ==========

    /**
     * 设置属性过滤条件。
     * @param attributeFilter SQL-like 表达式（如 "name = 'Beijing'"）
     * @return *this，支持链式调用
     */
    GdbQuery& where(const std::string& attributeFilter);

    /**
     * 设置空间过滤条件和关系类型。
     *
     * @param geom 过滤几何对象（不拷贝，仅持有指针，调用方保证生命周期）
     * @param relation 空间关系（默认 Intersects）
     * @return *this，支持链式调用
     */
    GdbQuery& spatial(const OGRGeometry* geom, GdbSpatialRelation relation = GdbSpatialRelation::Intersects);

    /**
     * 设置结果数量限制。-1 表示无限制（默认）。
     * @return *this，支持链式调用
     */
    GdbQuery& limit(int64_t count);

    /**
     * 设置结果偏移量（跳过前 N 条）。默认 0。
     * @return *this，支持链式调用
     */
    GdbQuery& offset(int64_t start);

    // ========== 有效性检查 ==========

    /**
     * 查询是否为空（既无属性过滤也无空间过滤）。
     * "空" 意味着 query() 将返回全部要素。
     */
    bool isEmpty() const;

    // ========== 内部 getter（供 GdbDataset::query() 使用） ==========

    /** 获取属性过滤表达式。
     * @return 属性过滤表达式的只读引用。
     */
    const std::string& getWhere() const { return m_where; }
    /** 获取空间过滤几何。
     * @return 非拥有的几何指针，未设置时返回 nullptr。
     */
    const OGRGeometry* getSpatialFilter() const { return m_spatialFilter; }
    /** 获取空间关系类型。
     * @return 当前空间关系。
     */
    GdbSpatialRelation getSpatialRelation() const { return m_spatialRelation; }
    /** 获取结果数量限制。
     * @return 最大记录数，-1 表示不限制。
     */
    int64_t getLimit() const { return m_limit; }
    /** 获取结果偏移量。
     * @return 需要跳过的记录数。
     */
    int64_t getOffset() const { return m_offset; }

    // ========== 调试输出 ==========

    /** 生成可读的查询条件字符串，用于调试和日志。 */
    std::string toString() const;

private:
    /** 属性过滤表达式（SQL-like 字符串）。 */
    std::string m_where;

    /** 空间过滤几何指针（不拥有，仅持有）。 */
    const OGRGeometry* m_spatialFilter = nullptr;

    /** 空间关系类型。默认 Intersects。 */
    GdbSpatialRelation m_spatialRelation = GdbSpatialRelation::Intersects;

    /** 结果数量限制。-1 = 无限制。 */
    int64_t m_limit = -1;

    /** 结果偏移量。默认 0。 */
    int64_t m_offset = 0;
};

#endif // GDB_QUERY_BUILDER_H
