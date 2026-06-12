// src/field.cpp — GdbField 实现
//
// GdbField 是 OGR 字段值类型到 C++ 类型安全的 std::variant 的映射层。
// 核心关注点：
// 1. 类型映射：fromOgrField() 将 OGRFieldType（OFTInteger/OFTReal 等）
//    映射到 variant 分支（int32_t/int64_t/double/std::string）
// 2. 跨类型转换：asInteger/asDouble 等方法提供隐式类型转换
//    （截断、拓宽），调用方需留意数据丢失风险
// 3. 空值处理：std::nullptr_t 对应 Null，与 OGR IsFieldNull 对齐
// 4. List 类型处理：OFTIntegerList/OFTStringList 等与标量归为一组，
//    取 OGR GetFieldAsXxx() 的标量返回值（首元素），面向单值场景

#include "field.h"
#include "ogr_feature.h"

/**
 * 获取字段类型。
 *
 * 通过 std::holds_alternative 逐一检查 variant 的活跃分支，
 * 返回对应的 Type 枚举值。未匹配的任何状态返回 Type::Null（防御性默认值）。
 */
GdbField::Type GdbField::getType() const {
    if (std::holds_alternative<std::nullptr_t>(m_value)) return Type::Null;
    if (std::holds_alternative<int32_t>(m_value)) return Type::Integer;
    if (std::holds_alternative<int64_t>(m_value)) return Type::Integer64;
    if (std::holds_alternative<double>(m_value)) return Type::Real;
    if (std::holds_alternative<std::string>(m_value)) return Type::String;
    return Type::Null;
}

/** 是否为空值。检查 variant 是否持有 nullptr_t。 */
bool GdbField::isNull() const {
    return std::holds_alternative<std::nullptr_t>(m_value);
}

/**
 * 转为 32 位整数。
 *
 * 实现策略（按 variant 分支逐一尝试）：
 * 1. int32_t：精确匹配，直接返回
 * 2. int64_t：static_cast 截断为 32 位，高位数据丢失
 *    例：0x1_0000_0000LL -> 0
 * 3. double：static_cast 向零截断，小数部分丢失
 *    例：3.14 -> 3, -2.9 -> -2
 * 4. 其他（string/nullptr）：返回 0
 *
 * 注意：不做范围检查，int64_t 超出 int32_t 范围时结果为截断值。
 */
int32_t GdbField::asInteger() const {
    if (std::holds_alternative<int32_t>(m_value)) return std::get<int32_t>(m_value);
    if (std::holds_alternative<int64_t>(m_value)) return static_cast<int32_t>(std::get<int64_t>(m_value));
    if (std::holds_alternative<double>(m_value)) return static_cast<int32_t>(std::get<double>(m_value));
    return 0;
}

/**
 * 转为 64 位整数。
 *
 * 实现策略：
 * 1. int64_t：精确匹配，直接返回
 * 2. int32_t：隐式拓宽为 int64_t（无损转换）
 * 3. double：static_cast 向零截断，小数部分丢失
 *    例：3.14 -> 3
 * 4. 其他（string/nullptr）：返回 0
 *
 * 注意：double -> int64_t 不检查范围，超出 int64_t 可表示范围时
 * 结果为实现定义的值（通常为 INT64_MIN 或 INT64_MAX）。
 */
int64_t GdbField::asInteger64() const {
    if (std::holds_alternative<int64_t>(m_value)) return std::get<int64_t>(m_value);
    if (std::holds_alternative<int32_t>(m_value)) return std::get<int32_t>(m_value);
    if (std::holds_alternative<double>(m_value)) return static_cast<int64_t>(std::get<double>(m_value));
    return 0;
}

/**
 * 转为双精度浮点数。
 *
 * 实现策略：
 * 1. double：精确匹配，直接返回
 * 2. int32_t：隐式拓宽为 double（小整数无损）
 * 3. int64_t：static_cast<double>，> 2^53 的整数可能丢失低位精度
 *    （IEEE 754 double 只有 53 位尾数）
 * 4. 其他（string/nullptr）：返回 0.0
 *
 * 注意：64 位大整数转 double 时，低位有效数字可能被舍入。
 * 例：9223372036854775807 -> 9223372036854775808（差 1）
 */
double GdbField::asDouble() const {
    if (std::holds_alternative<double>(m_value)) return std::get<double>(m_value);
    if (std::holds_alternative<int32_t>(m_value)) return std::get<int32_t>(m_value);
    if (std::holds_alternative<int64_t>(m_value)) return static_cast<double>(std::get<int64_t>(m_value));
    return 0.0;
}

/**
 * 转为字符串引用。
 *
 * 实现策略：
 * 1. std::string：直接返回内部 const 引用（零拷贝）
 * 2. 其他类型：返回静态空字符串
 *
 * 注意：与 OGR 的 GetFieldAsString 不同，GdbField::asString() 不执行
 * 数值到字符串的格式化转换。如果需要 "123" 这样的字符串表示，
 * 调用方应自行使用 std::to_string 或 sprintf。
 */
const std::string& GdbField::asString() const {
    if (std::holds_alternative<std::string>(m_value)) return std::get<std::string>(m_value);
    static const std::string empty;
    return empty;
}

/** 返回类型的可读名称，用于日志和错误信息。 */
std::string GdbField::typeName() const {
    switch (getType()) {
        case Type::Null:       return "Null";
        case Type::Integer:    return "Integer";
        case Type::Integer64:  return "Integer64";
        case Type::Real:       return "Real";
        case Type::String:     return "String";
    }
    return "Unknown";
}

/**
 * 从 OGRFeature 按索引读取字段值（核心类型映射函数）。
 *
 * 类型映射策略总览：
 * ┌─────────────────────────┬──────────────┬──────────────────────────────┐
 * │ OGR FieldType           │ GdbField     │ 说明                         │
 * ├─────────────────────────┼──────────────┼──────────────────────────────┤
 * │ OFTInteger              │ Integer      │ 32 位整数                    │
 * │ OFTIntegerList          │ Integer      │ 取首元素（单值场景）          │
 * │ OFTInteger64            │ Integer64    │ 64 位整数                    │
 * │ OFTReal                 │ Real         │ 双精度浮点                   │
 * │ OFTRealList             │ Real         │ 取首元素                     │
 * │ OFTString               │ String       │ UTF-8 字符串                 │
 * │ OFTStringList           │ String       │ OGR 已拼接为逗号分隔字符串    │
 * │ OFTWideString           │ String       │ OGR 已转为 UTF-8             │
 * │ OFTWideStringList       │ String       │ 同上                         │
 * │ OFTDate                 │ String       │ OGR 格式化为 "YYYY/MM/DD"    │
 * │ OFTTime                 │ String       │ OGR 格式化为 "HH:MM:SS"      │
 * │ OFTDateTime             │ String       │ OGR 格式化为 "YYYY/MM/DD HH:MM:SS" │
 * │ 其他（Binary/Geometry） │ Null         │ 不支持，返回空值              │
 * └─────────────────────────┴──────────────┴──────────────────────────────┘
 *
 * 设计决策说明：
 *
 * 1. List 类型归入标量分支：
 *    本设计面向单值字段场景（GDB 的常见字段都是标量类型）。
 *    OGR 的 GetFieldAsInteger/GetFieldAsDouble 对 List 类型返回首元素值，
 *    GetFieldAsString 对 StringList 返回逗号分隔的拼接字符串。
 *    这种简化满足大多数 GDB 字段读取需求，如需完整 List 支持可扩展 variant。
 *
 * 2. Date/Time/DateTime 映射为 String：
 *    OGR 的 GetFieldAsString 内部已将这些类型格式化为人类可读的字符串
 *    （如 "2024/01/15"、"14:30:00"）。GdbField 不额外存储 struct tm 或
 *    std::chrono 类型，保持 variant 分支简洁。
 *    如需解析日期，调用方可使用日期解析库从字符串提取。
 *
 * 3. 前置条件检查：
 *    - feat 为空指针：返回默认构造的空值
 *    - index < 0：返回空值（防御性检查）
 *    - IsFieldNull(index)：返回空值，与 OGR 的 null 语义对齐
 *
 * @param feat  OGRFeature 指针
 * @param index 字段在 Feature 中的索引（0-based）
 */
GdbField GdbField::fromOgrField(const OGRFeature* feat, int index) {
    if (!feat || index < 0) return GdbField();
    if (feat->IsFieldNull(index)) return GdbField();

    OGRFieldType type = feat->GetFieldDefnRef(index)->GetType();
    switch (type) {
        case OFTInteger:
        case OFTIntegerList:
            return GdbField(feat->GetFieldAsInteger(index));
        case OFTInteger64:
            return GdbField(feat->GetFieldAsInteger64(index));
        case OFTReal:
        case OFTRealList:
            return GdbField(feat->GetFieldAsDouble(index));
        case OFTString:
        case OFTStringList:
        case OFTWideString:
        case OFTWideStringList:
            return GdbField(std::string(feat->GetFieldAsString(index)));
        case OFTDate:
        case OFTTime:
        case OFTDateTime:
            return GdbField(std::string(feat->GetFieldAsString(index)));
        default:
            return GdbField();
    }
}

/**
 * 从 OGRFeature 按名称读取字段值。
 *
 * 实现流程：
 * 1. 空指针检查：feat 为空时直接返回空值
 * 2. 名称查找：调用 OGRFeature::GetFieldIndex() 将名称转为索引
 *    （内部遍历字段定义表，O(n) 复杂度）
 * 3. 转发到索引版本：fromOgrField(feat, idx)，复用类型映射逻辑
 *
 * 注意：频繁按名称查找时建议在调用方缓存字段索引，
 * 避免重复的字符串比较开销。
 */
GdbField GdbField::fromOgrField(const OGRFeature* feat, const std::string& name) {
    if (!feat) return GdbField();
    int idx = feat->GetFieldIndex(name.c_str());
    return fromOgrField(feat, idx);
}
