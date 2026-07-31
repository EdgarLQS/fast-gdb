// src/feature.h — GdbFeature 要素抽象（单条要素：FID + 几何 + 字段集合）
//
// GdbFeature 是要素值对象，封装单条要素的完整数据：FID、几何对象、命名字段集合。
// 设计参考 SuperMap iObjects 的 Feature 抽象——将 Recordset 游标中的瞬时要素快照为
// 独立拥有的值类型，便于长期持有、跨 Recordset 传递、存入 STL 容器。
//
// 核心语义：
// 1. 独立值类型：深拷贝几何（OGRGeometry::clone），按名称拷贝字段，
//    与原始 OGRFeature / Recordset 完全解耦，不受游标推进影响。
// 2. 可长期持有：可保存为局部变量、类成员、vector 元素，生命周期自主管理。
// 3. 深拷贝语义：拷贝构造/赋值对几何执行 clone()，字段 vector 按值拷贝。
// 4. OGRFeature 互转：fromNative 从 OGRFeature 深拷贝为 GdbFeature，
//    toNative 从 GdbFeature 重建 OGRFeature（调用方负责 delete）。

#ifndef GDB_FEATURE_H
#define GDB_FEATURE_H

#include "field.h"
#include "ogrsf_frmts.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

/**
 * GdbFeature — 要素值对象，封装单条要素：FID + 几何 + 命名字段集合。
 *
 * 核心语义：
 * 1. 独立值类型：持有深拷贝的 OGRGeometry（unique_ptr 管理）和按名称
 *    排列的字段集合（m_fieldNames / m_fields 平行 vector）。
 * 2. 可长期持有：与 Recordset 解耦，游标推进不影响已保存的 GdbFeature。
 * 3. 可跨 Recordset 传递：可从 A Recordset 提取，用于 B Recordset 操作。
 * 4. 可存入容器：支持拷贝（深拷贝几何）、移动（默认），适配 std::vector 等。
 *
 * 与 GdbRecordset 的关系：
 *   GdbRecordset::getFeature() 通过 GdbFeature::fromNative() 将当前游标
 *   指向的 OGRFeature 快照为独立的 GdbFeature 值对象。
 */
class GdbFeature {
public:
    // ========== 构造/析构 ==========

    /** 默认构造要素。
     * @return FID 初始化为 OGRNullFID 的空要素。
     */
    GdbFeature() = default;

    /** 使用指定 FID 构造要素。
     * @param fid 要素 ID。
     */
    explicit GdbFeature(int64_t fid) : m_fid(fid) {}

    /**
     * 拷贝构造函数。深拷贝语义：
     * - m_fid：按值拷贝
     * - m_geometry：通过 OGRGeometry::clone() 深拷贝，创建独立拥有的副本
     * - m_fieldNames / m_fields：vector 按值拷贝
     */
    GdbFeature(const GdbFeature& other);

    /** 拷贝赋值运算符。深拷贝语义同拷贝构造。 */
    GdbFeature& operator=(const GdbFeature& other);

    /** 移动构造函数（默认）。转移 unique_ptr 和 vector 所有权。 */
    GdbFeature(GdbFeature&& other) noexcept = default;

    /** 移动赋值运算符（默认）。转移所有权。 */
    GdbFeature& operator=(GdbFeature&& other) noexcept = default;

    // ========== FID 访问 ==========

    /** 获取要素 FID。
     * @return 要素 ID；未设置时返回 OGRNullFID（通常为 -1）。
     */
    int64_t getFid() const { return m_fid; }

    /** 设置要素 FID。
     * @param fid 新的要素 ID。
     */
    void setFid(int64_t fid) { m_fid = fid; }

    // ========== 几何访问 ==========

    /**
     * 设置几何对象。通过 unique_ptr 语义转移所有权，
     * 自动释放旧的几何对象（如有），接管传入的 geom。
     * @param geom 新几何对象的独占指针。
     */
    void setGeometry(std::unique_ptr<OGRGeometry> geom) { m_geometry = std::move(geom); }

    /**
     * 获取几何对象指针。返回裸指针，由 GdbFeature 管理生命周期。
     * 如需独立副本，调用 getGeometry()->clone()。
     * @return 非拥有的几何指针；没有几何时返回 nullptr。
     */
    const OGRGeometry* getGeometry() const { return m_geometry.get(); }

    // ========== 字段访问 ==========

    /** 获取字段数量。
     * @return 当前字段集合中的字段数。
     */
    int getFieldCount() const { return static_cast<int>(m_fieldNames.size()); }

    /**
     * 按索引获取字段名。
     * @param index 字段索引。越界时返回空字符串。
     */
    std::string getFieldName(int index) const;

    /**
     * 按索引获取字段值。
     * @param index 字段索引。越界时返回空的 GdbField（Null 类型）。
     */
    GdbField getField(int index) const;

    /**
     * 按名称获取字段值。线性扫描 m_fieldNames 匹配名称。
     * @param name 字段名。不存在时返回空的 GdbField（Null 类型）。
     */
    GdbField getField(const std::string& name) const;

    /**
     * 设置字段值。
     * 查找策略：线性扫描 m_fieldNames，找到同名则覆盖对应 m_fields 元素；
     * 未找到则在末尾追加新字段名和值（自动扩展字段集合）。
     * @param name  字段名
     * @param value 字段值（GdbField 值对象）
     * @return 无返回值；未找到的字段会被追加。
     */
    void setField(const std::string& name, const GdbField& value);

    // ========== 序列化 ==========

    /**
     * 序列化为 JSON 字符串。
     * 格式：{ "fid": <fid>, "geometry": "<WKT>", "fields": { "name": value, ... } }
     * - geometry 通过 OGRGeometry::exportToWkt() 导出为 WKT 字符串
     * - 字段值按类型序列化：Integer/Integer64/Real 直接输出数字，
     *   String 加双引号，Null 输出 null
     * @return JSON 格式的要素文本。
     */
    std::string toJson() const;

    // ========== 与 GDAL 原生 OGRFeature 互转 ==========

    /**
     * 从 OGRFeature 深拷贝创建 GdbFeature。
     * - 几何：clone() 深拷贝为独立拥有的 unique_ptr
     * - 字段：遍历 OGRFeature 的字段定义，通过 GdbField::fromOgrField 转换
     * @param feat 原生 OGRFeature 指针。空指针时返回默认构造的 GdbFeature。
     * @return 深拷贝得到的 GdbFeature 值对象。
     */
    static GdbFeature fromNative(const OGRFeature* feat);

    /**
     * 将 GdbFeature 转为 OGRFeature。
     * - 根据 OGRFeatureDefn 创建新 OGRFeature，调用方负责 delete 释放
     * - 设置 FID、几何（SetGeometry 不转移所有权，仅引用）
     * - 遍历字段，按 GdbField 类型分发调用 SetField 系列方法
     * @param defn 要素定义。空指针时返回 nullptr。
     * @return 新建的 OGRFeature 指针，调用方负责释放。
     */
    OGRFeature* toNative(const OGRFeatureDefn* defn) const;

private:
    int64_t m_fid = OGRNullFID;
    std::unique_ptr<OGRGeometry> m_geometry;
    std::vector<std::string> m_fieldNames;
    std::vector<GdbField> m_fields;
};

#endif // GDB_FEATURE_H
