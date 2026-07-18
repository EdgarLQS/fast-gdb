// src/edgar/usegdal/feature.cpp
// GdbFeature 值对象实现 — 管理几何深拷贝、字段集合和 OGRFeature 双向转换。
//
// 设计约束：
// - GdbFeature 必须与来源游标解耦，因此拷贝时 clone() 几何而不是共享指针。
// - 字段名与字段值使用平行 vector，保持 OGR 定义顺序并支持按名称覆盖。
// - OGRFeature 转换只复制本对象认识的字段；定义中不存在的名称被安全忽略。

#include "feature.h"
#include "gdal_priv.h"
#include <sstream>

// ========== 值语义与资源所有权 ==========

GdbFeature::GdbFeature(const GdbFeature& other)
    : m_fid(other.m_fid),
      m_geometry(other.m_geometry
          ? std::unique_ptr<OGRGeometry>(other.m_geometry->clone())
          : nullptr),
      m_fieldNames(other.m_fieldNames),
      m_fields(other.m_fields) {}

GdbFeature& GdbFeature::operator=(const GdbFeature& other) {
    if (this != &other) {
        m_fid = other.m_fid;
        m_geometry = other.m_geometry
            ? std::unique_ptr<OGRGeometry>(other.m_geometry->clone())
            : nullptr;
        m_fieldNames = other.m_fieldNames;
        m_fields = other.m_fields;
    }
    return *this;
}

// ========== 字段访问 ==========

std::string GdbFeature::getFieldName(int index) const {
    if (index < 0 || index >= static_cast<int>(m_fieldNames.size()))
        return "";
    return m_fieldNames[static_cast<size_t>(index)];
}

GdbField GdbFeature::getField(int index) const {
    if (index < 0 || index >= static_cast<int>(m_fields.size()))
        return GdbField();
    return m_fields[static_cast<size_t>(index)];
}

GdbField GdbFeature::getField(const std::string& name) const {
    for (size_t index = 0; index < m_fieldNames.size(); ++index) {
        if (m_fieldNames[index] == name) return m_fields[index];
    }
    return GdbField();
}

void GdbFeature::setField(const std::string& name,
                          const GdbField& value) {
    // 覆盖时保持原字段顺序；新字段追加到末尾，确保两个 vector 始终同长。
    for (size_t index = 0; index < m_fieldNames.size(); ++index) {
        if (m_fieldNames[index] == name) {
            m_fields[index] = value;
            return;
        }
    }
    m_fieldNames.push_back(name);
    m_fields.push_back(value);
}

// ========== 轻量 JSON 表示 ==========

std::string GdbFeature::toJson() const {
    std::ostringstream output;
    output << "{ \"fid\": " << m_fid;
    if (m_geometry) {
        char* wkt = nullptr;
        m_geometry->exportToWkt(&wkt);
        if (wkt != nullptr) {
            output << ", \"geometry\": \"" << wkt << "\"";
            // exportToWkt() 返回 CPL 分配的缓冲区，必须使用 CPLFree 释放。
            CPLFree(wkt);
        }
    }
    output << ", \"fields\": {";
    for (size_t index = 0; index < m_fieldNames.size(); ++index) {
        if (index != 0) output << ", ";
        output << "\"" << m_fieldNames[index] << "\": ";
        const auto& field = m_fields[index];
        switch (field.getType()) {
            case GdbField::Type::Null:
                output << "null";
                break;
            case GdbField::Type::Integer:
                output << field.asInteger();
                break;
            case GdbField::Type::Integer64:
                output << field.asInteger64();
                break;
            case GdbField::Type::Real:
                output << field.asDouble();
                break;
            case GdbField::Type::String:
                output << "\"" << field.asString() << "\"";
                break;
        }
    }
    output << "} }";
    return output.str();
}

// ========== OGRFeature 双向转换 ==========

GdbFeature GdbFeature::fromNative(const OGRFeature* feature) {
    if (feature == nullptr) return GdbFeature();

    GdbFeature result(feature->GetFID());
    const OGRGeometry* geometry = feature->GetGeometryRef();
    if (geometry != nullptr) {
        // OGRFeature 拥有原几何；clone 后结果可独立于游标长期保存。
        result.setGeometry(
            std::unique_ptr<OGRGeometry>(geometry->clone()));
    }

    const int count = feature->GetFieldCount();
    for (int index = 0; index < count; ++index) {
        const char* name =
            feature->GetFieldDefnRef(index)->GetNameRef();
        result.setField(
            name, GdbField::fromOgrField(feature, index));
    }
    return result;
}

OGRFeature* GdbFeature::toNative(
    const OGRFeatureDefn* definition) const {
    if (definition == nullptr) return nullptr;

    OGRFeature* feature = new OGRFeature(
        const_cast<OGRFeatureDefn*>(definition));
    feature->SetFID(m_fid);
    if (m_geometry) {
        // SetGeometry() 执行深拷贝，本对象继续保持几何所有权。
        feature->SetGeometry(
            const_cast<OGRGeometry*>(m_geometry.get()));
    }

    for (size_t index = 0; index < m_fieldNames.size(); ++index) {
        const int field_index = definition->GetFieldIndex(
            m_fieldNames[index].c_str());
        if (field_index < 0) continue;

        const auto& field = m_fields[index];
        switch (field.getType()) {
            case GdbField::Type::Null:
                feature->SetFieldNull(field_index);
                break;
            case GdbField::Type::Integer:
                feature->SetField(field_index, field.asInteger());
                break;
            case GdbField::Type::Integer64:
                // int64_t is long on LP64 Linux while GDAL's GIntBig may be
                // long long. Make the intended overload explicit.
                feature->SetField(
                    field_index,
                    static_cast<GIntBig>(field.asInteger64()));
                break;
            case GdbField::Type::Real:
                feature->SetField(field_index, field.asDouble());
                break;
            case GdbField::Type::String:
                feature->SetField(
                    field_index, field.asString().c_str());
                break;
        }
    }
    return feature;
}
