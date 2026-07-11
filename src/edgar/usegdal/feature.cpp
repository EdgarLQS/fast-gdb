// src/feature.cpp — GdbFeature 实现

#include "feature.h"
#include "gdal_priv.h"
#include <sstream>

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
    for (size_t index = 0; index < m_fieldNames.size(); ++index) {
        if (m_fieldNames[index] == name) {
            m_fields[index] = value;
            return;
        }
    }
    m_fieldNames.push_back(name);
    m_fields.push_back(value);
}

std::string GdbFeature::toJson() const {
    std::ostringstream output;
    output << "{ \"fid\": " << m_fid;
    if (m_geometry) {
        char* wkt = nullptr;
        m_geometry->exportToWkt(&wkt);
        if (wkt != nullptr) {
            output << ", \"geometry\": \"" << wkt << "\"";
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

GdbFeature GdbFeature::fromNative(const OGRFeature* feature) {
    if (feature == nullptr) return GdbFeature();

    GdbFeature result(feature->GetFID());
    const OGRGeometry* geometry = feature->GetGeometryRef();
    if (geometry != nullptr) {
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
