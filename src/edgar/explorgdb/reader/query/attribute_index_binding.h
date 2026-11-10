// src/edgar/explorgdb/reader/query/attribute_index_binding.h
// 将 .gdbindexes 中的索引名绑定到唯一直接索引字段；重复名称按歧义 fail closed。

#ifndef EXPLORGDB_ATTRIBUTE_INDEX_BINDING_H
#define EXPLORGDB_ATTRIBUTE_INDEX_BINDING_H

#include "explorgdb_types.h"
#include "gdb_indexes.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace explorgdb::detail {

enum class DirectIndexBinding {
    Bound,
    Missing,
    Ambiguous
};

inline char ascii_lower(char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

inline bool ascii_iequals(const std::string& left, const std::string& right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(),
            [](char a, char b) { return ascii_lower(a) == ascii_lower(b); });
}

// Bind index_name to exactly one direct indexed field. The same name declared
// for different fields is ambiguous: callers must not silently choose one.
inline DirectIndexBinding resolve_direct_index_field(
    const std::vector<IndexEntry>& metadata,
    const std::string& index_name,
    const std::vector<FieldDescriptor>& fields,
    FieldType& type,
    size_t& position) {
    bool bound = false;
    for (const auto& entry : metadata) {
        if (entry.name != index_name ||
            !GdbIndexesParser::is_direct_field_expression(entry.column_name))
            continue;
        const std::string column =
            GdbIndexesParser::field_name_from_expression(entry.column_name);
        for (size_t i = 0; i < fields.size(); ++i) {
            if (!ascii_iequals(fields[i].name, column)) continue;
            if (bound && i != position)
                return DirectIndexBinding::Ambiguous;
            if (!bound) {
                bound = true;
                type = fields[i].type;
                position = i;
            }
            break;
        }
    }
    return bound ? DirectIndexBinding::Bound : DirectIndexBinding::Missing;
}

} // namespace explorgdb::detail
#endif
