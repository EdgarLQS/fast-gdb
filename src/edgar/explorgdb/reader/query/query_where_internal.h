// src/edgar/explorgdb/reader/query/query_where_internal.h
// WHERE 内部模块 — 编译受支持表达式、绑定字段并在两种行表示上求值。

#ifndef EXPLORGDB_QUERY_WHERE_INTERNAL_H
#define EXPLORGDB_QUERY_WHERE_INTERNAL_H

#include "gdb_attribute_index.h"
#include "gdb_table.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace explorgdb {

/** 首版可直接映射到单个 .atx 查询的安全比较谓词。 */
struct IndexableWherePredicate {
    size_t field_index = 0;
    std::string field_name;
    AttrOp op = AttrOp::Eq;
    bool is_string = false;
    std::string string_value;
    double numeric_value = 0.0;
};

struct CompiledWhereImpl;

/**
 * 已完成词法、语法和字段绑定的 WHERE 表达式句柄。
 *
 * AST 通过共享不可变实现隐藏在内部模块，避免 parser 类型泄露到 QueryEngine
 * 公开头。对象可复制，多个候选扫描可安全复用同一编译结果。
 */
class CompiledWhere {
public:
    CompiledWhere() = default;

    /** 判断 WHERE 表达式是否编译并绑定成功。
     * @return 表达式有效时返回 true。
     */
    bool valid() const { return impl_ != nullptr && error_.empty(); }
    /** 获取编译错误文本。
     * @return 错误文本的只读引用；无错误时为空。
     */
    const std::string& error() const { return error_; }

    /** 返回表达式实际引用的字段索引，用于候选扫描裁剪。 */
    /** 获取表达式引用的字段索引。
     * @return 字段索引列表的只读引用。
     */
    const std::vector<size_t>& referenced_field_indexes() const;

    /**
     * 返回可安全使用单个 .atx 的谓词；复合表达式或编码不安全时为空。
     */
    /** 获取可直接映射到单个属性索引的谓词。
     * @return 可索引谓词；不满足安全条件时返回空值。
     */
    std::optional<IndexableWherePredicate> indexable_predicate() const;

private:
    friend CompiledWhere compile_where(
        const std::string& text,
        const std::vector<FieldDescriptor>& fields);
    friend bool evaluate_where(
        const CompiledWhere& expression,
        const FieldRef* fields,
        int field_count);
    friend bool evaluate_where(
        const CompiledWhere& expression,
        const FeatureRecord& record);

    std::shared_ptr<const CompiledWhereImpl> impl_;
    std::string error_;
};

/**
 * 解析并绑定 WHERE 子集。
 *
 * @param text WHERE 文本，不包含 `WHERE` 关键字。
 * @param fields 当前表字段描述符。
 * @return 有效表达式或携带具体错误文本的无效句柄。
 */
CompiledWhere compile_where(
    const std::string& text,
    const std::vector<FieldDescriptor>& fields);

/** 在零拷贝扫描行上求值。
 * @param expression 已编译 WHERE 表达式。
 * @param fields 当前扫描行的字段数组。
 * @param field_count 字段数量。
 * @return 表达式匹配时返回 true。
 */
bool evaluate_where(
    const CompiledWhere& expression,
    const FieldRef* fields,
    int field_count);

/** 在已物化记录上复核完整 WHERE 语义。
 * @param expression 已编译 WHERE 表达式。
 * @param record 已物化字段记录。
 * @return 表达式匹配时返回 true。
 */
bool evaluate_where(
    const CompiledWhere& expression,
    const FeatureRecord& record);

/**
 * 对两个升序 FID 序列执行线性双指针交集。
 *
 * 输入可包含相邻重复项；输出始终升序且唯一。
 * @param left 左侧升序 FID 序列。
 * @param right 右侧升序 FID 序列。
 * @return 两个序列的唯一交集。
 */
std::vector<uint32_t> intersect_sorted_fids(
    const std::vector<uint32_t>& left,
    const std::vector<uint32_t>& right);

} // namespace explorgdb

#endif // EXPLORGDB_QUERY_WHERE_INTERNAL_H
