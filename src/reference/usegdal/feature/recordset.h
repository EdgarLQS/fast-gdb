// src/recordset.h — GdbRecordset 记录集（OGRLayer 的 RAII 封装）

#ifndef GDB_RECORDSET_H
#define GDB_RECORDSET_H

#include "error_context.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "spatial_relation.h"
#include <memory>
#include <string>

class GdbFeature;

/**
 * GdbRecordset — 记录集视图，封装 OGRLayer 的顺序游标。
 *
 * 核心语义：
 * 1. 顺序游标：OGRLayer 的 GetNextFeature() 维护内部游标，moveNext() 前进。
 *    Phase 1 不提供 movePrev/moveTo，只提供 moveFirst/moveNext。
 * 2. 非拥有视图：不拥有 OGRLayer 指针，生命周期不得超过 GdbDatasource。
 * 3. 不可拷贝：同一个 Recordset 持有唯一的 OGRFeature* 游标位置，
 *    拷贝会导致两个实例共享游标、互相覆盖。
 * 4. 可移动：支持所有权转移（如从 query() 返回时）。
 *
 * 与 OGRLayer 的关系：
 *   OGRLayer 维护三类共享状态：
 *   - 游标位置：GetNextFeature() 推进，ResetReading() 重置
 *   - 属性滤镜：SetAttributeFilter() 持久化，不因新 Recordset 而清除
 *   - 空间滤镜：SetSpatialFilter() 同上
 *   因此 GdbDataset::getRecordset() 每次调用 ResetReading() 确保新游标从头开始。
 *
 * 线程安全：
 *   GdbRecordset 不线程安全。OGRLayer 的 GetNextFeature 推进共享游标，
 *   多线程不可共享同一 Recordset。应通过 GdbConnectionPool 获取独立连接。
 */
class GdbRecordset {
public:
    GdbRecordset();
    ~GdbRecordset();

    // 不可拷贝：OGRLayer 游标是共享的，拷贝会导致游标位置互相覆盖
    GdbRecordset(const GdbRecordset&) = delete;
    GdbRecordset& operator=(const GdbRecordset&) = delete;

    // 可移动：支持所有权转移（如从 query() 返回）
    GdbRecordset(GdbRecordset&& other) noexcept;
    GdbRecordset& operator=(GdbRecordset&& other) noexcept;

    // ========== 顺序游标操作 ==========

    /**
     * 移动游标到第一条要素并返回该要素。
     * 等价于 ResetReading() + GetNextFeature()。
     * @return 成功定位第一条要素时返回 true。
     */
    bool moveFirst();

    /**
     * 移动游标到下一条要素。
     *
     * 空间关系后过滤逻辑（详见 recordset.cpp 实现）：
     * - Intersects：OGRLayer SetSpatialFilter 已处理，直接返回
     * - Contains/Within/Disjoint：逐要素检查 OGRGeometry 空间关系
     *
     * @return true 有下一条要素，false 已到达末尾
     */
    bool moveNext();

    /** 判断游标是否已到达末尾。
     * @return 没有更多要素时返回 true。
     */
    bool isEOF() const;

    /** 判断记录集是否有效。
     * @return 底层 OGRLayer 指针有效时返回 true。
     */
    bool isValid() const;

    // ========== 当前要素读取 ==========

    /** 获取当前要素的 FID。
     * @return 当前 FID；没有当前要素时返回 -1。
     */
    int64_t getFid() const;

    // ========== 字段访问 ==========

    /** 获取字段总数。
     * @return 图层定义中的字段数量。
     */
    int getFieldCount() const;

    /** 按索引获取字段名。
     * @param index 字段索引。
     * @return 字段名；索引越界时返回空字符串。
     */
    std::string getFieldName(int index) const;

    /** 按名称获取字段索引。
     * @param name 字段名称。
     * @return 字段索引；字段不存在时返回 -1。
     */
    int getFieldIndex(const std::string& name) const;

    // ========== 类型化读取 ==========
    // 按名称或索引读取当前要素的字段值，类型不匹配时返回 GDAL 默认转换结果

    /** 按名称读取当前字段的 32 位整数值。
     * @param name 字段名称。
     * @return GDAL 转换后的整数值。
     */
    int32_t getFieldAsInteger(const std::string& name) const;
    /** 按索引读取当前字段的 32 位整数值。
     * @param index 字段索引。
     * @return GDAL 转换后的整数值。
     */
    int32_t getFieldAsInteger(int index) const;
    /** 按名称读取当前字段的 64 位整数值。
     * @param name 字段名称。
     * @return GDAL 转换后的整数值。
     */
    int64_t getFieldAsInteger64(const std::string& name) const;
    /** 按索引读取当前字段的 64 位整数值。
     * @param index 字段索引。
     * @return GDAL 转换后的整数值。
     */
    int64_t getFieldAsInteger64(int index) const;
    /** 按名称读取当前字段的浮点值。
     * @param name 字段名称。
     * @return GDAL 转换后的双精度值。
     */
    double getFieldAsDouble(const std::string& name) const;
    /** 按索引读取当前字段的浮点值。
     * @param index 字段索引。
     * @return GDAL 转换后的双精度值。
     */
    double getFieldAsDouble(int index) const;
    /** 按名称读取当前字段的字符串值。
     * @param name 字段名称。
     * @return GDAL 转换后的字符串。
     */
    std::string getFieldAsString(const std::string& name) const;
    /** 按索引读取当前字段的字符串值。
     * @param index 字段索引。
     * @return GDAL 转换后的字符串。
     */
    std::string getFieldAsString(int index) const;

    // ========== 几何访问 ==========

    /**
     * 获取当前要素的几何对象指针。
     * 指针由 OGRLayer 管理，生命周期与当前要素绑定。
     * 如需长期持有，使用 cloneGeometry()。
     */
    const OGRGeometry* getGeometry() const;

    /**
     * 克隆当前要素的几何对象。
     * 返回独立拥有的副本，调用方负责通过 unique_ptr 管理生命周期。
     */
    std::unique_ptr<OGRGeometry> cloneGeometry() const;

    // ========== 要素抽象 ==========

    /**
     * 将当前要素转为 GdbFeature 值对象。
     * 自动深拷贝几何和字段，可长期持有、跨 Recordset 传递。
     */
    GdbFeature getFeature() const;

    // ========== 写入操作 ==========

    /**
     * 准备编辑当前要素。
     * GDAL 中 GetNextFeature() 返回的要素可直接 SetField/SetGeometry
     * 后 SetFeature() 更新，不需要显式 "edit" 标记。
     * 此方法仅验证当前有可编辑的要素。
     * @return 当前要素可编辑时返回 true。
     */
    bool edit();

    /** 按名称修改当前要素的 32 位整数字段。
     * @param name 字段名称。
     * @param value 新字段值。
     * @return 修改成功时返回 true。
     */
    bool setField(const std::string& name, int32_t value);
    /** 按名称修改当前要素的浮点字段。
     * @param name 字段名称。
     * @param value 新字段值。
     * @return 修改成功时返回 true。
     */
    bool setField(const std::string& name, double value);
    /** 按名称修改当前要素的字符串字段。
     * @param name 字段名称。
     * @param value 新字段值。
     * @return 修改成功时返回 true。
     */
    bool setField(const std::string& name, const std::string& value);
    /** 按名称修改当前要素的 64 位整数字段。
     * @param name 字段名称。
     * @param value 新字段值。
     * @return 修改成功时返回 true。
     */
    bool setField(const std::string& name, int64_t value);

    /** 修改当前要素的几何对象。
     * @param geom 新几何对象；记录集不接管指针所有权。
     * @return 设置成功时返回 true。
    */
    bool setGeometry(const OGRGeometry* geom);

    /**
     * 提交当前要素的修改到 OGRLayer。
     * 调用 SetFeature() 后 ResetReading() 防止游标越界。
     */
    bool update();

    /** 删除当前要素。
     * @return 删除成功时返回 true；删除后当前要素指针失效。
     */
    bool deleteCurrent();

    /** 获取最近一次操作的错误信息。
     * @return 错误文本；没有错误或未绑定上下文时返回空字符串。
     */
    std::string getLastError() const { return m_errorCtx ? m_errorCtx->getLastError() : ""; }

    /**
     * 关闭记录集，释放当前要素。
     * 析构函数自动调用。调用后 Recordset 仍有效，可复用（但不会自动重新打开）。
     * @return 无返回值。
     */
    void close();

private:
    friend class GdbDataset;
    friend class GdbDatasets;

    /** 构造函数，由 GdbDataset::getRecordset() / query() 调用。 */
    GdbRecordset(OGRLayer* layer, GdbErrorContext* errCtx);

    // ========== 成员变量 ==========

    /** 底层 OGRLayer 指针（非拥有，由 GdbDatasource 管理生命周期）。 */
    OGRLayer* m_layer = nullptr;

    /** 当前要素指针。由 GetNextFeature() 获取，DestroyFeature() 释放。 */
    OGRFeature* m_currentFeature = nullptr;

    /** 错误上下文指针（转发到 GdbDatasource）。 */
    GdbErrorContext* m_errorCtx = nullptr;

    /** 游标是否已到达末尾（GetNextFeature 返回 nullptr）。 */
    bool m_eof = false;

    // ========== 空间关系后过滤（仅内部使用） ==========
    //
    // 背景：GDAL SetSpatialFilter() 只做 bbox 相交预过滤，
    // 不支持 Contains/Within/Disjoint。这些关系必须在 moveNext() 中后过滤。
    //
    // 设置时机：GdbDataset::query() 通过 friend 访问设置。
    // 使用方式：moveNext() 检查 m_spatialRelation，非 Intersects 时逐要素验证。

    /** 空间关系类型。默认 Intersects（SetSpatialFilter 已处理，无需后过滤）。 */
    GdbSpatialRelation m_spatialRelation = GdbSpatialRelation::Intersects;

    /** 空间过滤几何对象指针（const，仅用于 Contains/Within/Disjoint 后过滤）。 */
    const OGRGeometry* m_spatialFilter = nullptr;

    /** 设置空间过滤几何（friend 专用）。
     * @param geom 非拥有的空间过滤几何指针。
     */
    void setSpatialFilter(const OGRGeometry* geom) { m_spatialFilter = geom; }

    /** 设置空间关系类型（friend 专用）。
     * @param rel 要应用的空间关系。
     */
    void setSpatialRelation(GdbSpatialRelation rel) { m_spatialRelation = rel; }
};

#endif // GDB_RECORDSET_H
