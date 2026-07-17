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

struct IndexableWherePredicate {
    size_t field_index = 0;
    std::string field_name;
    AttrOp op = AttrOp::Eq;
    bool is_string = false;
    std::string string_value;
    double numeric_value = 0.0;
};

struct CompiledWhereImpl;

class CompiledWhere {
public:
    CompiledWhere() = default;

    bool valid() const { return impl_ != nullptr && error_.empty(); }
    const std::string& error() const { return error_; }
    const std::vector<size_t>& referenced_field_indexes() const;
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

CompiledWhere compile_where(
    const std::string& text,
    const std::vector<FieldDescriptor>& fields);

bool evaluate_where(
    const CompiledWhere& expression,
    const FieldRef* fields,
    int field_count);

bool evaluate_where(
    const CompiledWhere& expression,
    const FeatureRecord& record);

// Both inputs must be ascending. The result is ascending and unique even if an
// input contains adjacent duplicate FIDs.
std::vector<uint32_t> intersect_sorted_fids(
    const std::vector<uint32_t>& left,
    const std::vector<uint32_t>& right);

} // namespace explorgdb

#endif // EXPLORGDB_QUERY_WHERE_INTERNAL_H
