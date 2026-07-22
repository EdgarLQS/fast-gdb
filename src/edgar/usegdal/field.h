// src/field.h — GdbField 值类型（基于 std::variant 的类型安全字段值封装）
//
// 设计动机：
// GDB（ESRI FileGeodatabase）的字段值在运行时具有多种类型（整型、64 位整型、
// 浮点型、字符串、空值），传统方案使用 void* + 类型标记或联合体，容易引发
// 类型安全问题。GdbField 采用 std::variant 实现类型安全的变体值类型，
// 灵感来源于 SuperMap iObjects 的 Field 设计。
//
// 核心关注点：
// 1. 类型安全：std::variant 在编译期约束可容纳的类型集合，运行期通过
//    holds_alternative 检查，避免 reinterpret_cast 导致的未定义行为
// 2. 空值语义：Null 对应 variant 的 std::nullptr_t，与 OGR 的 IsFieldNull
//    语义对齐，区别于空字符串或数值 0
// 3. 跨类型转换：提供 asInteger/asDouble/asString 等方法，支持隐式类型转换
//    （如 Integer64 -> Integer 截断、Integer -> Double 拓宽），调用方需留意
//    精度丢失风险
// 4. OGR 适配：fromOgrField() 将 OGRFeature 的字段值映射为 GdbField，
//    处理 OGR 类型系统（OFTInteger/OFTReal/OFTString 等）到 variant 分支的映射

#ifndef GDB_FIELD_H
#define GDB_FIELD_H

#include <cstdint>
#include <string>
#include <variant>

class OGRFeature;

/**
 * GdbField — 类型安全的字段值变体类型。
 *
 * 封装 GDB 字段的可能取值（Null / Integer / Integer64 / Real / String），
 * 底层使用 std::variant<std::nullptr_t, int32_t, int64_t, double, std::string>
 * 存储。默认构造为空值（Null）。
 *
 * 与 OGR 的关系：
 *   fromOgrField() 负责将 OGRFeature 的字段读取为 GdbField，
 *   完成 OGR 类型系统到 variant 分支的映射。
 *
 * 类型转换：
 *   asInteger/asInteger64/asDouble 支持跨类型读取：
 *   - Integer64 -> Integer：static_cast 截断，可能丢失高位数据
 *   - Double -> Integer：向零截断，丢失小数部分
 *   - Integer -> Double：无损拓宽
 *   - 非字符串类型调用 asString()：返回空字符串（不自动格式化数值）
 *
 * 空值处理：
 *   isNull() 检查 variant 是否持有 nullptr_t。
 *   对空值调用 asXxx() 返回对应类型的零值（0 / 0.0 / ""）。
 */
class GdbField {
public:
    /**
     * 字段类型枚举，对应 std::variant 的各个分支。
     *
     * - Null:      空值，对应 OGR 的 IsFieldNull() 语义
     * - Integer:   32 位有符号整数（int32_t），对应 OGR OFTInteger
     * - Integer64: 64 位有符号整数（int64_t），对应 OGR OFTInteger64
     * - Real:      双精度浮点数（double），对应 OGR OFTReal
     * - String:    UTF-8 编码字符串（std::string），对应 OGR OFTString
     */
    enum class Type { Null, Integer, Integer64, Real, String };

    /**
     * 默认构造函数，创建空值（Null）字段。
     * 底层 variant 默认构造为 std::nullptr_t。
     */
    GdbField() = default;

    /** 从 32 位整数构造，对应 Type::Integer。 */
    explicit GdbField(int32_t v) : m_value(v) {}

    /** 从 64 位整数构造，对应 Type::Integer64。 */
    explicit GdbField(int64_t v) : m_value(v) {}

    /** 从双精度浮点数构造，对应 Type::Real。 */
    explicit GdbField(double v) : m_value(v) {}

    /** 从 std::string 构造，对应 Type::String。 */
    explicit GdbField(std::string v) : m_value(std::move(v)) {}

    /**
     * 便捷构造：C 字符串自动转为 std::string 存储。
     * 注意：不检查 nullptr，传入 null 指针会导致 std::string 构造异常。
     * 对应 Type::String。
     */
    explicit GdbField(const char* v) : m_value(std::string(v)) {}

    /**
     * 获取当前字段的类型。
     * 通过 std::holds_alternative 逐一检查 variant 的活跃分支。
     */
    Type getType() const;

    /** 是否为空值。等价于 getType() == Type::Null。 */
    bool isNull() const;

    /**
     * 转为 32 位整数。
     *
     * 转换规则：
     * - Integer：直接返回
     * - Integer64：static_cast<int32_t> 截断，可能丢失高位数据
     * - Real：static_cast<int32_t> 向零截断，丢失小数部分
     * - String / Null：返回 0
     *
     * 注意：不检查数值范围，大值截断后结果不可预期。
     */
    int32_t asInteger() const;

    /**
     * 转为 64 位整数。
     *
     * 转换规则：
     * - Integer64：直接返回
     * - Integer：隐式拓宽为 int64_t（无损）
     * - Real：static_cast<int64_t> 向零截断，丢失小数部分
     * - String / Null：返回 0
     */
    int64_t asInteger64() const;

    /**
     * 转为双精度浮点数。
     *
     * 转换规则：
     * - Real：直接返回
     * - Integer：隐式拓宽为 double（小整数无损，超大整数可能丢失精度）
     * - Integer64：static_cast<double>，> 2^53 的整数可能丢失精度
     * - String / Null：返回 0.0
     */
    double asDouble() const;

    /**
     * 转为字符串引用。
     *
     * 转换规则：
     * - String：返回内部 std::string 的 const 引用
     * - 其他类型：返回静态空字符串（不自动格式化数值）
     *
     * 注意：非字符串类型不会自动转换为字符串表示（如 "123"），
     * 如需格式化字符串，请在调用方自行处理。
     */
    const std::string& asString() const;

    /** 获取类型的可读名称（"Null" / "Integer" / "Integer64" / "Real" / "String"）。 */
    std::string typeName() const;

    /**
     * 从 OGRFeature 按索引读取字段值。
     *
     * 类型映射策略：
     * - Null：IsFieldNull() 为 true 时返回空值
     * - OFTInteger / OFTIntegerList：映射为 Integer（List 类型取 OGR 首元素）
     * - OFTInteger64：映射为 Integer64
     * - OFTReal / OFTRealList：映射为 Real（List 类型取 OGR 首元素）
     * - OFTString / OFTStringList / OFTWideString / OFTWideStringList：
     *   映射为 String（OGR GetFieldAsString 已处理列表拼接）
     * - OFTDate / OFTTime / OFTDateTime：映射为 String
     *   （OGR 内部已格式化为 "YYYY/MM/DD" 等字符串）
     * - 其他类型：返回空值
     *
     * 注意：List 类型与标量归为一组，是因为本设计面向单值字段场景，
     * 取 OGR 的 GetFieldAsXxx() 返回值（通常是列表首元素或拼接字符串）。
     * 日期/时间类型转为字符串是因为 OGR 的 GetFieldAsString 已经提供了
     * 格式化的字符串表示，无需在 GdbField 层额外处理。
     *
     * @param feat  OGRFeature 指针，为空时返回空值
     * @param index 字段索引，< 0 时返回空值
     */
    static GdbField fromOgrField(const OGRFeature* feat, int index);

    /**
     * 从 OGRFeature 按名称读取字段值。
     * 内部调用 GetFieldIndex() 查找索引后转发到索引版本。
     *
     * @param feat OGRFeature 指针，为空时返回空值
     * @param name 字段名称
     */
    static GdbField fromOgrField(const OGRFeature* feat, const std::string& name);

private:
    /**
     * 底层存储：std::variant 天然支持类型安全和默认构造（Null）。
     *
     * 分支顺序与 Type 枚举对应：
     *   index 0: std::nullptr_t  -> Type::Null
     *   index 1: int32_t         -> Type::Integer
     *   index 2: int64_t         -> Type::Integer64
     *   index 3: double          -> Type::Real
     *   index 4: std::string     -> Type::String
     *
     * 默认构造时 variant 激活第一个分支（nullptr_t），即 Null 状态。
     */
    std::variant<std::nullptr_t, int32_t, int64_t, double, std::string> m_value;
};

#endif // GDB_FIELD_H
