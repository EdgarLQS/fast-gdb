// src/datasets.h — GdbDatasets（图层集合视图）和 GdbDataset（单个图层视图）
//
// GdbDatasets 和 GdbDataset 是 GdbDatasource 的视图层：
// - GdbDatasets：持有 GDALDataset*，提供图层枚举、创建、删除
// - GdbDataset：持有 OGRLayer*，提供记录集访问、查询、要素操作
//
// 两者均不拥有底层 GDAL 指针，生命周期受 GdbDatasource 约束。
// 非线程安全：多线程并发时，每个线程通过独立连接获取各自的视图。

#ifndef GDB_DATASETS_H
#define GDB_DATASETS_H

#include "error_context.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include <map>
#include <string>
#include <vector>

class GdbRecordset;
class GdbQueryParameter;
class GdbQuery;

/**
 * GdbDataset — 单个图层的视图，提供记录集访问和查询。
 *
 * 核心职责：
 * 1. 图层信息获取（名称、几何类型、字段定义、要素计数）
 * 2. 记录集创建（getRecordset / query）
 * 3. 要素写入（addNew / deleteAll）
 * 4. 能力检测（属性过滤、空间索引、快速计数）
 *
 * 非拥有视图：持有 OGRLayer*，生命周期受 GdbDatasource 约束。
 * 共享状态：OGRLayer 的游标位置、属性滤镜、空间滤镜在多次查询间持久化，
 * 因此 query() 内部会手动重置滤镜防止条件叠加。
 */
class GdbDataset {
public:
    GdbDataset();

    /** 获取图层名称。
     * @return 图层名称；图层无效时返回空字符串。
     */
    std::string getName() const;

    /** 获取图层几何类型。
     * @return OGR 几何类型；图层无效时返回未知类型。
     */
    OGRwkbGeometryType getGeometryType() const;

    /**
     * 获取要素总数。基于 OGRLayer::GetFeatureCount()。
     * @return 要素数量，-1 表示无法获取（如驱动不支持快速计数）
     */
    int getFeatureCount();

    /** 获取字段总数。
     * @return 图层定义中的字段数量。
     */
    int getFieldCount() const;

    /** 按索引获取字段名。
     * @param index 字段索引。
     * @return 字段名；索引越界时返回空字符串。
     */
    std::string getFieldName(int index) const;

    /** 按索引获取字段类型。
     * @param index 字段索引。
     * @return OGR 字段类型；索引越界时返回默认类型。
     */
    OGRFieldType getFieldType(int index) const;

    // ========== 记录集 ==========

    /**
     * 获取遍历全部要素的记录集。
     * 内部调用 ResetReading() 确保游标从头开始。
     */
    GdbRecordset getRecordset() const;

    /**
     * 获取带属性过滤的记录集。
     * 在 getRecordset() 基础上设置属性滤镜。
     *
     * @param attributeFilter SQL-like 过滤表达式（如 "population > 1000000"）
     * @return 按过滤条件创建的记录集。
     */
    GdbRecordset getRecordsetFiltered(const std::string& attributeFilter) const;

    // ========== 统一查询接口 ==========

    /**
     * 使用 GdbQueryParameter 执行查询。
     * 支持属性过滤、空间过滤（bbox/rect）、结果字段选择。
     * 每次调用前清空旧滤镜，防止条件叠加。
     * @param param 查询参数。
     * @return 查询结果记录集。
     */
    GdbRecordset query(const GdbQueryParameter& param) const;

    /**
     * 使用 GdbQuery（链式构建器）执行查询。
     * 支持 where/spatial/limit/offset 链式组合。
     * 空间关系支持 Intersects/Contains/Within/Disjoint（两层过滤架构）。
     * @param q 链式查询条件。
     * @return 查询结果记录集。
     */
    GdbRecordset query(const GdbQuery& q) const;

    // ========== 能力检测 ==========

    /** 判断是否支持属性过滤。
     * @return 支持时返回 true。
     */
    bool supportsAttributeFilter() const;

    /** 判断是否支持快速空间过滤。
     * @return 存在可用空间索引时返回 true。
     */
    bool supportsFastSpatialFilter() const;

    /** 判断是否支持快速要素计数。
     * @return 驱动提供快速计数时返回 true。
     */
    bool supportsFastFeatureCount() const;

    // ========== 要素计数 ==========

    /**
     * 使用 GdbQueryParameter 条件统计要素数量。
     * 内部遍历 recordset 计数，不使用 GDAL 快速计数。
     *
     * @return 符合条件的要素数量，-1 表示查询失败
     */
    int getFeatureCountFiltered(const GdbQueryParameter& param) const;

    /**
     * 使用 GdbQuery 条件统计要素数量。
     * 支持 limit 截断（count >= limit 时停止遍历）。
     *
     * @return 符合条件的要素数量，-1 表示查询失败
     */
    int count(const GdbQuery& q) const;

    // ========== 写入操作 ==========

    /**
     * 在当前图层创建新要素。
     *
     * 实现策略：
     * 1. 基于 LayerDefn 创建 OGRFeature 模板
     * 2. 设置几何对象（如果提供）
     * 3. 按名称查找字段索引，逐个 SetField
     * 4. CreateFeature 写入图层
     *
     * @param geom 几何对象（可为 nullptr）
     * @param fields 字段名到字符串值的映射（只支持字符串设置）
     * @return true 创建成功，false 失败
     */
    bool addNew(const OGRGeometry* geom, const std::map<std::string, std::string>& fields = {});

    /**
     * 删除图层中全部要素。
     *
     * 实现策略：ResetReading + GetNextFeature 遍历，
     * 逐个 DeleteFeature + DestroyFeature。
     * 注意：此操作不可逆，建议在事务中执行。
     *
     * @return true 全部删除成功，false 失败
     */
    bool deleteAll();

    // ========== 字段信息 ==========

    /** 获取字段类型的人类可读名称。
     * @param type OGR 字段类型。
     * @return 类型名称文本。
     */
    std::string getFieldTypeName(OGRFieldType type) const;

    // ========== 底层访问 ==========

    /** 获取底层 OGRLayer 指针（非拥有，仅供与 GDAL C API 互操作）。 */
    OGRLayer* getNative() const;

    /** 判断图层视图是否有效。
     * @return 底层 OGRLayer 指针非空时返回 true。
     */
    bool isValid() const;

    /** 获取最新错误信息。
     * @return 错误文本；没有错误时返回空字符串。
     */
    std::string getLastError() const { return m_errorCtx ? m_errorCtx->getLastError() : ""; }

private:
    friend class GdbDatasets;
    friend class GdbRecordset;

    GdbDataset(OGRLayer* layer, GdbErrorContext* errCtx);

    /** 底层 OGRLayer 指针（非拥有，由 GdbDatasource 管理生命周期）。 */
    OGRLayer* m_layer = nullptr;

    /** 错误上下文指针（转发到 GdbDatasource）。 */
    GdbErrorContext* m_errorCtx = nullptr;
};

/**
 * GdbDatasets — 图层集合视图，提供图层枚举、创建、删除。
 *
 * 核心职责：
 * 1. 图层枚举（getCount / get by index / get by name）
 * 2. 图层创建（create）
 * 3. 图层删除（remove）
 *
 * 非拥有视图：持有 GDALDataset*，生命周期受 GdbDatasource 约束。
 * 通过 GdbDatasource::getDatasets() 获取。
 */
class GdbDatasets {
public:
    GdbDatasets();

    /** 获取图层数量。
     * @return 当前数据源的图层数量。
     */
    int getCount() const;

    /** 按索引获取图层视图。
     * @param index 图层索引。
     * @return 图层视图；越界时返回无效视图。
     */
    GdbDataset get(int index) const;

    /** 按名称获取图层视图。
     * @param name 图层名称。
     * @return 图层视图；不存在时返回无效视图。
     */
    GdbDataset get(const std::string& name) const;

    // ========== 图层管理 ==========

    /**
     * 在当前数据源创建新图层。
     *
     * @param name 图层名称
     * @param type 几何类型（wkbPoint/wkbLineString/wkbPolygon 等）
     * @param srs 空间参考（可为 nullptr）
     * @return 新图层的 GdbDataset 视图，创建失败返回无效视图
     */
    GdbDataset create(const std::string& name, OGRwkbGeometryType type,
                      const OGRSpatialReference* srs = nullptr) const;

    /**
     * 删除指定名称的图层。
     *
     * 实现策略：遍历图层列表查找名称，获取索引后调用 GDALDataset::DeleteLayer。
     *
     * @param name 图层名称
     * @return true 删除成功，false 失败（不存在或驱动不支持删除）
     */
    bool remove(const std::string& name) const;

    /** 获取最新错误信息。转发到 GdbDatasource 的错误上下文。 */
    std::string getLastError() const { return m_errorCtx ? m_errorCtx->getLastError() : ""; }

private:
    friend class GdbDatasource;

    GdbDatasets(GDALDataset* ds, GdbErrorContext* errCtx);

    /** 底层 GDALDataset 指针（非拥有，由 GdbDatasource 管理生命周期）。 */
    GDALDataset* m_ds = nullptr;

    /** 错误上下文指针（转发到 GdbDatasource）。 */
    GdbErrorContext* m_errorCtx = nullptr;
};

#endif // GDB_DATASETS_H
