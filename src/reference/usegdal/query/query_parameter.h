// src/query_parameter.h — GdbQueryParameter 查询参数类（参考 SuperMap iObjects QueryParameter 设计）
//
// 与 GdbQuery 的区别：
// - GdbQueryParameter：setter 风格，支持 bbox 快捷矩形过滤（setSpatialFilterRect），
//   内部 unique_ptr 管理矩形几何，适合简单场景
// - GdbQuery：链式构建，支持 where/spatial/limit/offset 组合，空间关系可指定，
//   但不管理几何生命周期，适合复杂查询
//
// 两者都传递给 GdbDataset::query() 执行，功能重叠但使用风格不同。

#ifndef GDB_QUERY_PARAMETER_H
#define GDB_QUERY_PARAMETER_H

#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include <memory>
#include <string>
#include <vector>

/**
 * GdbQueryParameter — 查询参数数据传输对象。
 *
 * 封装查询条件（属性过滤、空间过滤、结果字段选择），
 * 通过 setter 方法逐个设置，传递给 GdbDataset::query() 执行。
 *
 * 空间过滤特点：
 * - setSpatialFilter()：传入外部几何指针（不拥有）
 * - setSpatialFilterRect()：内部构造矩形多边形（unique_ptr 管理）
 */
class GdbQueryParameter {
public:
    GdbQueryParameter() = default;

    // ========== 属性过滤 ==========

    /**
     * 设置属性过滤条件（SQL-like 表达式）。
     * 如 "population > 1000000 AND name LIKE 'B%'"
     */
    void setAttributeFilter(const std::string& where) { m_attrFilter = where; }

    /** 获取属性过滤表达式。
     * @return SQL-like 属性过滤表达式的只读引用。
     */
    const std::string& getAttributeFilter() const { return m_attrFilter; }

    // ========== 空间过滤 ==========

    /**
     * 设置空间过滤几何对象。
     * @param geom 几何指针（不拥有，调用方须保证生命周期）
     */
    void setSpatialFilter(const OGRGeometry* geom) { m_spatialFilter = geom; }

    /**
     * 使用矩形范围设置空间过滤。
     *
     * 内部构造 OGRPolygon 表示矩形（顺时针环绕：左下→右下→右上→左上→闭合），
     * 由 unique_ptr 管理生命周期。
     * 后续调用 setSpatialFilter() 会覆盖此矩形。
     *
     * @param minX 最小 X（左下角 X）
     * @param minY 最小 Y（左下角 Y）
     * @param maxX 最大 X（右上角 X）
     * @param maxY 最大 Y（右上角 Y）
     */
    void setSpatialFilterRect(double minX, double minY, double maxX, double maxY);

    /** 获取空间过滤几何指针。
     * @return 非拥有的过滤几何指针，未设置时返回 nullptr。
     */
    const OGRGeometry* getSpatialFilter() const { return m_spatialFilter; }

    // ========== 是否返回几何 ==========

    /** 设置是否在结果中包含几何对象。默认 true。 */
    void setHasGeometry(bool value) { m_hasGeometry = value; }

    /** 判断查询结果是否包含几何。
     * @return 包含几何时返回 true。
     */
    bool hasGeometry() const { return m_hasGeometry; }

    // ========== 结果字段 ==========

    /**
     * 设置需要返回的字段列表。
     * 空列表表示返回全部字段。设置后可提升查询性能（忽略不需要的字段）。
     */
    void setResultFields(std::vector<std::string> fields) { m_resultFields = std::move(fields); }

    /** 获取结果字段列表。
     * @return 字段名列表的只读引用；为空表示返回全部字段。
     */
    const std::vector<std::string>& getResultFields() const { return m_resultFields; }

    // ========== 有效性检查 ==========

    /**
     * 查询是否为空（既无属性过滤也无空间过滤）。
     * "空" 意味着 query() 将返回全部要素。
     */
    bool isEmpty() const { return m_attrFilter.empty() && m_spatialFilter == nullptr; }

    // ========== 调试输出 ==========

    /** 生成可读的查询条件字符串。
     * @return 用于调试和日志的查询条件文本。
     */
    std::string toString() const;

private:
    /** 属性过滤表达式（SQL-like 字符串）。 */
    std::string m_attrFilter;

    /** 空间过滤几何指针（不拥有，仅持有）。 */
    const OGRGeometry* m_spatialFilter = nullptr;

    /** setSpatialFilterRect 创建的矩形多边形（内部拥有，供 m_spatialFilter 引用）。 */
    std::unique_ptr<OGRGeometry> m_rectGeom;

    /** 是否返回几何对象。默认 true。 */
    bool m_hasGeometry = true;

    /** 需要返回的字段列表。空表示全部。 */
    std::vector<std::string> m_resultFields;
};

#endif // GDB_QUERY_PARAMETER_H
