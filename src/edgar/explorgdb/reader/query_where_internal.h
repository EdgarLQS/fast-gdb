// src/edgar/explorgdb/reader/query_where_internal.h
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

    bool valid() const { return impl_ != nullptr && error_.empty(); }
    const std::string& error() const { return error_; }

    /** 返回表达式实际引用的字段索引，用于候选扫描裁剪。 */
    const std::vector<size_t>& referenced_field_indexes() const;

    /**
     * 返回可安全使用单个 .atx 的谓词；复合表达式或编码不安全时为空。
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

/** 在零拷贝扫描行上求值；FieldRef 只在当前回调期间有效。 */
bool evaluate_where(
    const CompiledWhere& expression,
    const FieldRef* fields,
    int field_count);

/** 在已物化 FeatureRecord 上复核完整 WHERE 语义。 */
bool evaluate_where(
    const CompiledWhere& expression,
    const FeatureRecord& record);

/**
 * 对两个升序 FID 序列执行线性双指针交集。
 *
 * 输入可包含相邻重复项；输出始终升序且唯一。
 */
std::vector<uint32_t> intersect_sorted_fids(
    const std::vector<uint32_t>& left,
    const std::vector<uint32_t>& right);

} // namespace explorgdb

#endif // EXPLORGDB_QUERY_WHERE_INTERNAL_H
