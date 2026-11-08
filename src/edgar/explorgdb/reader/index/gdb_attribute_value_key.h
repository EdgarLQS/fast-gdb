#ifndef EXPLORGDB_ATTRIBUTE_VALUE_KEY_H
#define EXPLORGDB_ATTRIBUTE_VALUE_KEY_H

#include "explorgdb_types.h"
#include "ole_date.h"
#include "uuid_value.h"
#include <cmath>
#include <cstring>
#include <string>

namespace explorgdb::attribute_value {
enum class Domain : uint8_t { String, Floating, Integer, Temporal, Uuid };

inline void bytes(uint64_t value, std::string& result) {
    result.resize(8);
    for (size_t i = 0; i < 8; ++i) result[i] = static_cast<char>(value >> (56 - 8 * i));
}
inline void integer(int64_t value, std::string& result) {
    bytes(static_cast<uint64_t>(value) ^ (uint64_t{1} << 63), result);
}
inline bool floating(double value, std::string& result) {
    if (std::isnan(value)) return false;
    if (value == 0) value = 0; // both zero signs have the same SQL comparison value
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    bytes((bits >> 63) ? ~bits : bits ^ (uint64_t{1} << 63), result);
    return true;
}
inline bool uuid(std::string_view value, std::string& result) {
    std::array<char, 32> hex;
    if (!canonical_uuid(value, hex)) return false;
    result.assign(hex.data(), hex.size());
    return true;
}

// Sortable encodings are private interval keys, never public field values.
inline bool field(const FieldRef& value, Domain domain, std::string& result) {
    if (value.is_null) return false;
    if (domain == Domain::Uuid) {
        if ((value.type != FieldType::UUID_1 && value.type != FieldType::UUID_2) ||
            value.byte_len != 16 || !value.data) return false;
        std::array<char, 32> hex;
        if (!canonical_uuid_bytes(value.data, value.byte_len, hex)) return false;
        result.assign(hex.data(), hex.size());
        return true;
    }
    if (domain == Domain::Integer) {
        if (value.type != FieldType::Int64 && value.type != FieldType::ObjectId) return false;
        integer(value.as_i64(), result);
        return true;
    }
    if (domain == Domain::Temporal) {
        if (value.type != FieldType::DateTime && value.type != FieldType::Date && value.type != FieldType::Time)
            return false;
        int64_t ms = 0;
        if (!try_ole_to_unix_milliseconds(value.as_f64(), ms)) return false;
        integer(ms, result);
        return true;
    }
    double number = 0;
    switch (value.type) {
        case FieldType::Int16: number = value.as_i16(); break;
        case FieldType::Int32: number = value.as_i32(); break;
        case FieldType::ObjectId:
        case FieldType::Int64: {
            const auto integer_value = value.as_i64();
            constexpr int64_t exact = int64_t{1} << 53;
            if (integer_value < -exact || integer_value > exact) return false;
            number = static_cast<double>(integer_value); break;
        }
        case FieldType::Float32: number = value.as_f32(); break;
        case FieldType::Float64:
        case FieldType::DateTime:
        case FieldType::Date:
        case FieldType::Time:
        case FieldType::DateTimeWithOffset: number = value.as_f64(); break;
        default: return false;
    }
    return floating(number, result);
}

inline bool index(const uint8_t* data, size_t width, FieldType type, Domain domain, std::string& result) {
    if (domain == Domain::Uuid) {
        const std::string_view key(reinterpret_cast<const char*>(data), width);
        return is_canonical_uuid_index_key(key) && uuid(key, result);
    }
    size_t expected = 8;
    if (type == FieldType::Int16) expected = 2;
    if (type == FieldType::Int32 || type == FieldType::Float32) expected = 4;
    if (type == FieldType::ObjectId && width == 4) {
        type = FieldType::Int32;
        expected = 4;
    }
    if (width != expected) return false;
    FieldRef value;
    value.is_null = false; value.type = type; value.data = data; value.byte_len = static_cast<int>(width);
    return field(value, domain, result);
}
} // namespace explorgdb::attribute_value
#endif
