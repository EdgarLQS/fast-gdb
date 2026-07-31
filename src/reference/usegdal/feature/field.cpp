// src/reference/usegdal/feature/field.cpp
// GdbField 实现 — 在 std::variant 值模型与 GDAL/OGR 字段类型之间转换。
//
// 转换策略刻意保持轻量：数值读取允许窄化/拓宽，字符串不隐式解析为数值；
// 不支持的 OGR 类型和空字段统一映射为 Null，使调用方能够稳定处理降级情况。

#include "field.h"
#include "ogr_feature.h"

// ========== 运行时类型与空值 ==========

GdbField::Type GdbField::getType() const {
    if (std::holds_alternative<std::nullptr_t>(m_value)) return Type::Null;
    if (std::holds_alternative<int32_t>(m_value)) return Type::Integer;
    if (std::holds_alternative<int64_t>(m_value)) return Type::Integer64;
    if (std::holds_alternative<double>(m_value)) return Type::Real;
    if (std::holds_alternative<std::string>(m_value)) return Type::String;
    return Type::Null;
}

bool GdbField::isNull() const {
    return std::holds_alternative<std::nullptr_t>(m_value);
}

// ========== 标量访问 ==========

int32_t GdbField::asInteger() const {
    if (std::holds_alternative<int32_t>(m_value))
        return std::get<int32_t>(m_value);
    if (std::holds_alternative<int64_t>(m_value))
        return static_cast<int32_t>(std::get<int64_t>(m_value));
    if (std::holds_alternative<double>(m_value))
        return static_cast<int32_t>(std::get<double>(m_value));
    return 0;
}

int64_t GdbField::asInteger64() const {
    if (std::holds_alternative<int64_t>(m_value))
        return std::get<int64_t>(m_value);
    if (std::holds_alternative<int32_t>(m_value))
        return std::get<int32_t>(m_value);
    if (std::holds_alternative<double>(m_value))
        return static_cast<int64_t>(std::get<double>(m_value));
    return 0;
}

double GdbField::asDouble() const {
    if (std::holds_alternative<double>(m_value))
        return std::get<double>(m_value);
    if (std::holds_alternative<int32_t>(m_value))
        return std::get<int32_t>(m_value);
    if (std::holds_alternative<int64_t>(m_value))
        return static_cast<double>(std::get<int64_t>(m_value));
    return 0.0;
}

const std::string& GdbField::asString() const {
    if (std::holds_alternative<std::string>(m_value))
        return std::get<std::string>(m_value);
    // 返回静态对象避免为非字符串访问重复分配临时字符串。
    static const std::string empty;
    return empty;
}

std::string GdbField::typeName() const {
    switch (getType()) {
        case Type::Null: return "Null";
        case Type::Integer: return "Integer";
        case Type::Integer64: return "Integer64";
        case Type::Real: return "Real";
        case Type::String: return "String";
    }
    return "Unknown";
}

// ========== OGR 字段适配 ==========

GdbField GdbField::fromOgrField(
    const OGRFeature* feature, int index) {
    if (feature == nullptr || index < 0 ||
        feature->IsFieldNull(index))
        return GdbField();

    const OGRFieldType type =
        feature->GetFieldDefnRef(index)->GetType();
    switch (type) {
        case OFTInteger:
        case OFTIntegerList:
            return GdbField(
                static_cast<int32_t>(
                    feature->GetFieldAsInteger(index)));
        case OFTInteger64:
            // GDAL's GIntBig may be long long while int64_t is long on LP64.
            // Cast explicitly to select the intended variant constructor.
            return GdbField(
                static_cast<int64_t>(
                    feature->GetFieldAsInteger64(index)));
        case OFTReal:
        case OFTRealList:
            return GdbField(feature->GetFieldAsDouble(index));
        case OFTString:
        case OFTStringList:
        case OFTWideString:
        case OFTWideStringList:
        case OFTDate:
        case OFTTime:
        case OFTDateTime:
            // 日期时间保持 GDAL 的标准文本表示，避免在值层引入时区策略。
            return GdbField(std::string(
                feature->GetFieldAsString(index)));
        default:
            return GdbField();
    }
}

GdbField GdbField::fromOgrField(
    const OGRFeature* feature, const std::string& name) {
    if (feature == nullptr) return GdbField();
    return fromOgrField(
        feature, feature->GetFieldIndex(name.c_str()));
}
