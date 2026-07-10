#ifndef EXPLORGDB_FIELD_LAYOUT_H
#define EXPLORGDB_FIELD_LAYOUT_H

#include "binary_reader.h"
#include "explorgdb_types.h"
#include <cstddef>

namespace explorgdb {

// The single source of truth for physical field widths in a row blob.
// A return value of 0 means variable-width or implicit storage.
inline constexpr size_t fixed_physical_width(FieldType type) noexcept {
    switch (type) {
        case FieldType::Int16: return 2;
        case FieldType::Int32:
        case FieldType::Float32: return 4;
        case FieldType::Int64:
        case FieldType::Float64:
        case FieldType::DateTime:
        case FieldType::Date:
        case FieldType::Time: return 8;
        case FieldType::DateTimeWithOffset: return 10; // double + int16 UTC offset minutes
        case FieldType::UUID_1:
        case FieldType::UUID_2: return 16;
        default: return 0;
    }
}

inline bool skip_field_value(BinaryReader& reader, FieldType type) {
    const size_t width = fixed_physical_width(type);
    if (width != 0) {
        if (!reader.can_read(width)) return false;
        reader.skip(width);
        return true;
    }

    switch (type) {
        case FieldType::ObjectId:
            return true; // implicit, occupies no row bytes
        case FieldType::String:
        case FieldType::XML:
        case FieldType::Binary:
        case FieldType::Raster:
        case FieldType::Geometry: {
            const uint64_t length = reader.read_varuint();
            if (!reader.can_read(static_cast<size_t>(length))) return false;
            reader.skip(static_cast<size_t>(length));
            return true;
        }
        default:
            return false;
    }
}

} // namespace explorgdb

#endif
