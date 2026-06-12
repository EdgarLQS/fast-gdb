// src/feature.cpp — GdbFeature 实现
//
// GdbFeature 是要素值对象的实现，将 OGRFeature 的瞬时数据快照为独立拥有的
// 值类型。核心关注点：
// 1. 深拷贝语义：通过 OGRGeometry::clone() 创建独立几何副本，
//    使 GdbFeature 脱离 Recordset/OGRLayer 生命周期约束。
// 2. 字段按名存储：使用平行 vector（m_fieldNames / m_fields）管理命名字段，
//    支持按索引和按名称双向访问。
// 3. OGRFeature 互转：fromNative 深拷贝、toNative 反向重建，
//    注意所有权转移——toNative 返回的新 OGRFeature 由调用方负责 delete。

#include "feature.h"
#include "gdal_priv.h"
#include <sstream>

// ========== 拷贝构造/赋值：深拷贝几何 ==========

/**
 * 拷贝构造函数。
 *
 * 实现策略：
 * - m_fid：直接按值拷贝
 * - m_geometry：通过 OGRGeometry::clone() 创建深拷贝副本，
 *   用 unique_ptr 接管所有权。若源对象无几何，则置为 nullptr。
 * - m_fieldNames / m_fields：vector 按值拷贝（元素为 trivially copyable）
 *
 * 内存模型：拷贝后两个 GdbFeature 各自拥有独立的 OGRGeometry 副本，
 * 修改任一对象的几何不影响另一个。
 */
GdbFeature::GdbFeature(const GdbFeature& other)
    : m_fid(other.m_fid),
      m_geometry(other.m_geometry ? std::unique_ptr<OGRGeometry>(other.m_geometry->clone()) : nullptr),
      m_fieldNames(other.m_fieldNames),
      m_fields(other.m_fields) {}

/**
 * 拷贝赋值运算符。
 *
 * 实现策略同拷贝构造函数，增加自赋值保护（this != &other）。
 * unique_ptr 赋值自动释放旧几何、接管新克隆的副本。
 */
GdbFeature& GdbFeature::operator=(const GdbFeature& other) {
    if (this != &other) {
        m_fid = other.m_fid;
        m_geometry = other.m_geometry ? std::unique_ptr<OGRGeometry>(other.m_geometry->clone()) : nullptr;
        m_fieldNames = other.m_fieldNames;
        m_fields = other.m_fields;
    }
    return *this;
}

// ========== 字段访问 ==========

/**
 * 按索引获取字段名。
 * @param index 字段索引。越界（< 0 或 >= m_fieldNames.size()）时返回空字符串。
 */
std::string GdbFeature::getFieldName(int index) const {
    if (index < 0 || index >= static_cast<int>(m_fieldNames.size())) return "";
    return m_fieldNames[index];
}

/**
 * 按索引获取字段值。
 * @param index 字段索引。越界时返回空的 GdbField（Null 类型）。
 */
GdbField GdbFeature::getField(int index) const {
    if (index < 0 || index >= static_cast<int>(m_fields.size())) return GdbField();
    return m_fields[index];
}

/**
 * 按名称获取字段值。
 * 查找策略：线性扫描 m_fieldNames，找到第一个匹配的字段名后返回对应 m_fields 元素。
 * @param name 字段名。不存在时返回空的 GdbField（Null 类型）。
 */
GdbField GdbFeature::getField(const std::string& name) const {
    for (size_t i = 0; i < m_fieldNames.size(); i++) {
        if (m_fieldNames[i] == name) return m_fields[i];
    }
    return GdbField();
}

/**
 * 设置字段值。
 *
 * 查找策略：
 * 1. 线性扫描 m_fieldNames，找到同名则覆盖 m_fields 中对应位置的元素。
 * 2. 未找到同名字段时，在 m_fieldNames 和 m_fields 末尾追加新条目。
 *
 * 注意：此方法允许自由添加新字段（不设字段定义表限制），
 * 与 Recordset 的 setField 不同（后者需字段在 LayerDefn 中存在）。
 */
void GdbFeature::setField(const std::string& name, const GdbField& value) {
    // 查找已存在的字段
    for (size_t i = 0; i < m_fieldNames.size(); i++) {
        if (m_fieldNames[i] == name) {
            m_fields[i] = value;
            return;
        }
    }
    m_fieldNames.push_back(name);
    m_fields.push_back(value);
}

// ========== 序列化 ==========

/**
 * 将要素序列化为 JSON 字符串。
 *
 * 序列化格式：
 *   { "fid": <int64>, "geometry": "<WKT 字符串>", "fields": { "name": value, ... } }
 *
 * 实现细节：
 * - fid：直接输出整数
 * - geometry：通过 OGRGeometry::exportToWkt() 导出为 WKT 格式字符串。
 *   exportToWkt 分配的内存由 GDAL 管理，必须通过 CPLFree() 释放，
 *   否则会造成内存泄漏。此处使用栈上 char* 接收，写入 stringstream
 *   后立即调用 CPLFree(wkt)。
 * - fields：按 m_fieldNames/m_fields 顺序遍历，字段名加双引号，
 *   字段值按类型序列化：Integer/Integer64/Real 输出数字字面量，
 *   String 加双引号，Null 输出 null 字面量。
 *
 * 注意：当前实现未对 WKT 和 String 中的特殊字符（如双引号、反斜杠）
 * 做 JSON 转义，包含此类字符时可能生成非法 JSON。
 */
std::string GdbFeature::toJson() const {
    std::ostringstream ss;
    ss << "{ \"fid\": " << m_fid;
    if (m_geometry) {
        char* wkt = nullptr;
        m_geometry->exportToWkt(&wkt);
        if (wkt) {
            ss << ", \"geometry\": \"" << wkt << "\"";
            CPLFree(wkt);
        }
    }
    ss << ", \"fields\": {";
    for (size_t i = 0; i < m_fieldNames.size(); i++) {
        if (i > 0) ss << ", ";
        ss << "\"" << m_fieldNames[i] << "\": ";
        const auto& f = m_fields[i];
        switch (f.getType()) {
            case GdbField::Type::Null:       ss << "null"; break;
            case GdbField::Type::Integer:    ss << f.asInteger(); break;
            case GdbField::Type::Integer64:  ss << f.asInteger64(); break;
            case GdbField::Type::Real:       ss << f.asDouble(); break;
            case GdbField::Type::String:     ss << "\"" << f.asString() << "\""; break;
        }
    }
    ss << "} }";
    return ss.str();
}

// ========== OGRFeature 互转 ==========

/**
 * 从 OGRFeature 深拷贝创建 GdbFeature。
 *
 * 实现策略：
 * 1. 提取 FID：feat->GetFID()
 * 2. 深拷贝几何：
 *    - 通过 feat->GetGeometryRef() 获取几何引用
 *    - clone() 创建独立副本，unique_ptr 接管所有权
 *    - 无几何时跳过（m_geometry 保持 nullptr）
 * 3. 填充字段：
 *    - 遍历 feat->GetFieldCount()，逐个提取字段
 *    - 通过 feat->GetFieldDefnRef(i)->GetNameRef() 获取字段名
 *    - 通过 GdbField::fromOgrField(feat, i) 将 OGR 字段值转为 GdbField
 *    - setField() 添加到结果中
 *
 * @param feat 原生 OGRFeature 指针。空指针时返回默认构造的 GdbFeature。
 * @return 独立拥有的 GdbFeature 值对象，与 feat 完全解耦。
 */
GdbFeature GdbFeature::fromNative(const OGRFeature* feat) {
    if (!feat) return GdbFeature();

    GdbFeature result(feat->GetFID());

    // 几何：深拷贝为独立拥有的 unique_ptr
    const OGRGeometry* geom = feat->GetGeometryRef();
    if (geom) {
        result.setGeometry(std::unique_ptr<OGRGeometry>(geom->clone()));
    }

    // 字段：遍历 OGRFeature 的字段定义，通过 GdbField::fromOgrField 转换
    int count = feat->GetFieldCount();
    for (int i = 0; i < count; i++) {
        const char* name = feat->GetFieldDefnRef(i)->GetNameRef();
        result.setField(name, GdbField::fromOgrField(feat, i));
    }

    return result;
}

/**
 * 将 GdbFeature 转为 OGRFeature。
 *
 * 实现策略：
 * 1. 根据 OGRFeatureDefn 创建新 OGRFeature（new 分配，调用方负责 delete）
 * 2. 设置 FID：feat->SetFID(m_fid)
 * 3. 设置几何：通过 SetGeometry() 引用 m_geometry（不转移所有权，
 *    OGRFeature 内部持有引用，几何生命周期仍由 GdbFeature 管理）
 * 4. 遍历字段：
 *    - 通过 defn->GetFieldIndex() 查找字段索引
 *    - 按 GdbField::getType() 类型分发调用 SetField 系列方法：
 *      Null -> SetFieldNull(), Integer -> SetField(int32),
 *      Integer64 -> SetField(int64), Real -> SetField(double),
 *      String -> SetField(const char*)
 *    - 字段名在 defn 中不存在时跳过（idx < 0）
 *
 * 所有权说明：
 * - 返回的 OGRFeature* 由调用方负责 delete
 * - 几何不转移所有权，OGRFeature 仅持有引用
 *
 * @param defn 要素定义。空指针时返回 nullptr。
 * @return 新建的 OGRFeature 指针，调用方负责释放。
 */
OGRFeature* GdbFeature::toNative(const OGRFeatureDefn* defn) const {
    if (!defn) return nullptr;

    OGRFeature* feat = new OGRFeature(const_cast<OGRFeatureDefn*>(defn));
    feat->SetFID(m_fid);

    if (m_geometry) {
        feat->SetGeometry(const_cast<OGRGeometry*>(m_geometry.get()));
    }

    for (size_t i = 0; i < m_fieldNames.size(); i++) {
        int idx = defn->GetFieldIndex(m_fieldNames[i].c_str());
        if (idx >= 0) {
            const auto& f = m_fields[i];
            switch (f.getType()) {
                case GdbField::Type::Null:       feat->SetFieldNull(idx); break;
                case GdbField::Type::Integer:    feat->SetField(idx, f.asInteger()); break;
                case GdbField::Type::Integer64:  feat->SetField(idx, f.asInteger64()); break;
                case GdbField::Type::Real:       feat->SetField(idx, f.asDouble()); break;
                case GdbField::Type::String:     feat->SetField(idx, f.asString().c_str()); break;
            }
        }
    }

    return feat;
}
