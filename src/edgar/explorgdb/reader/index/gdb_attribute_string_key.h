#ifndef EXPLORGDB_ATTRIBUTE_STRING_KEY_H
#define EXPLORGDB_ATTRIBUTE_STRING_KEY_H

#include "gdb_attribute_index.h"
#include "utf16.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace explorgdb::attribute_string {
inline uint16_t unit(const uint8_t* bytes, size_t i) {
    return static_cast<uint16_t>(bytes[2 * i]) |
        (static_cast<uint16_t>(bytes[2 * i + 1]) << 8);
}

struct KeyInfo {
    size_t text_units = 0;
    size_t prefix_units = 0;
};

// Padding may hide real trailing spaces. A final high surrogate at full key
// width may be the first half of a scalar cut by index truncation.
inline bool inspect(const uint8_t* bytes, size_t count, KeyInfo& info) {
    size_t end = count;
    while (end != 0 && (unit(bytes, end - 1) == 0x20 || unit(bytes, end - 1) == 0)) --end;
    info = {end, end};
    for (size_t i = 0; i < end; ++i) {
        const auto value = unit(bytes, i);
        if (value == 0) return false;
        if (value >= 0xd800 && value <= 0xdbff) {
            if (i + 1 == end) {
                if (end != count) return false;
                info.prefix_units = end - 1;
                return true;
            }
            const auto low = unit(bytes, ++i);
            if (low < 0xdc00 || low > 0xdfff) return false;
        } else if (value >= 0xdc00 && value <= 0xdfff) {
            return false;
        }
    }
    return true;
}

inline bool prepare(const std::string& value, size_t count, std::u16string& key) {
    std::u16string converted;
    if (!try_utf8_to_utf16(value, converted) || converted.find(u'\0') != std::u16string::npos)
        return false;
    converted.resize(count, u' ');
    key = std::move(converted);
    return true;
}

// Some GDAL-written UCS-2 indexes retain only the low 16 bits of a scalar.
// Match both physical representations for equality, then verify the record.
inline bool prepare_legacy(const std::string& value, size_t count, std::u16string& key) {
    std::u16string converted;
    if (!try_utf8_to_utf16(value, converted) || converted.find(u'\0') != std::u16string::npos)
        return false;
    std::u16string result;
    for (size_t i = 0; i < converted.size(); ++i) {
        uint32_t scalar = converted[i];
        if (scalar >= 0xd800 && scalar <= 0xdbff) {
            scalar = 0x10000 + ((scalar - 0xd800) << 10) + (converted[++i] - 0xdc00);
        }
        result.push_back(static_cast<char16_t>(scalar & 0xffff));
    }
    result.resize(count, u' ');
    key = std::move(result);
    return true;
}

inline void append_scalar(std::string& text, uint32_t scalar) {
    if (scalar < 0x80) text.push_back(static_cast<char>(scalar));
    else if (scalar < 0x800) {
        text.push_back(static_cast<char>(0xc0 | (scalar >> 6)));
        text.push_back(static_cast<char>(0x80 | (scalar & 63)));
    } else if (scalar < 0x10000) {
        text.push_back(static_cast<char>(0xe0 | (scalar >> 12)));
        text.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 63)));
        text.push_back(static_cast<char>(0x80 | (scalar & 63)));
    } else {
        text.push_back(static_cast<char>(0xf0 | (scalar >> 18)));
        text.push_back(static_cast<char>(0x80 | ((scalar >> 12) & 63)));
        text.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 63)));
        text.push_back(static_cast<char>(0x80 | (scalar & 63)));
    }
}

inline int compare(const uint8_t* bytes, const std::u16string& key) {
    size_t left_end = key.size(), right_end = key.size();
    while (left_end && (unit(bytes, left_end - 1) == 0 || unit(bytes, left_end - 1) == 0x20)) --left_end;
    while (right_end && (key[right_end - 1] == 0 || key[right_end - 1] == u' ')) --right_end;
    for (size_t i = 0; i < key.size(); ++i) {
        const auto left = i < left_end ? unit(bytes, i) : uint16_t{0x20};
        const auto right = i < right_end ? static_cast<uint16_t>(key[i]) : uint16_t{0x20};
        if (left != right) return left < right ? -1 : 1;
    }
    return 0;
}

inline int compare(const uint8_t* left, const uint8_t* right, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const auto a = unit(left, i), b = unit(right, i);
        if (a != b) return a < b ? -1 : 1;
    }
    return 0;
}

inline bool matches(int order, AttrOp op) {
    switch (op) {
        case AttrOp::Eq: return order == 0;
        case AttrOp::Ne: return order != 0;
        case AttrOp::Lt: return order < 0;
        case AttrOp::Le: return order <= 0;
        case AttrOp::Gt: return order > 0;
        case AttrOp::Ge: return order >= 0;
    }
    return false;
}

// Every original value starts with this prefix; padded/truncated suffixes
// remain unknown. Keep all possible matches for the final record-value check.
inline bool may_match(std::string_view prefix, std::string_view value, AttrOp op) {
    const size_t common = std::min(prefix.size(), value.size());
    const int order = prefix.substr(0, common).compare(value.substr(0, common));
    if (order != 0) return matches(order, op);
    if (prefix.size() > value.size()) return matches(1, op);
    if (prefix.size() == value.size()) return op != AttrOp::Lt;
    return true;
}
inline bool legacy_may_match(const uint8_t* bytes, size_t text_units,
                              std::string_view value, AttrOp op) {
    std::string minimum, maximum;
    for (size_t i = 0; i < text_units; ++i) {
        const uint32_t code = unit(bytes, i);
        append_scalar(minimum, code >= 0xd800 && code <= 0xdfff ? 0x10000 + code : code);
        append_scalar(maximum, 0x100000 | code);
    }
    if (op == AttrOp::Lt) return minimum.compare(value) < 0;
    if (op == AttrOp::Le) return minimum.compare(value) <= 0;
    if (op == AttrOp::Ne) return true;
    return may_match(maximum, value, op);
}

} // namespace explorgdb::attribute_string
#endif
