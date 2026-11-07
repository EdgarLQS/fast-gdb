// UUID value normalization shared by WHERE and typed attribute-index queries.
#ifndef EXPLORGDB_UUID_VALUE_H
#define EXPLORGDB_UUID_VALUE_H

#include <array>
#include <cstddef>
#include <string_view>

namespace explorgdb {

// Parse a standard hyphenated UUID, with optional outer braces, into lowercase hex.
inline bool canonical_uuid(std::string_view text, std::array<char, 32>& output) {
    if (text.size() == 38 && text.front() == '{' && text.back() == '}')
        text = text.substr(1, 36);
    if (text.size() != 36) return false;
    std::size_t target = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return false;
            continue;
        }
        const char ch = text[i];
        if (ch >= '0' && ch <= '9') output[target++] = ch;
        else if (ch >= 'a' && ch <= 'f') output[target++] = ch;
        else if (ch >= 'A' && ch <= 'F')
            output[target++] = static_cast<char>(ch - 'A' + 'a');
        else return false;
    }
    return target == output.size();
}

// FileGDB GUID ATX keys are fixed 38-byte uppercase ASCII with braces.
inline std::array<char, 38> uuid_index_key(const std::array<char, 32>& hex) {
    std::array<char, 38> key{};
    key.front() = '{';
    key.back() = '}';
    std::size_t source = 0;
    for (std::size_t i = 1; i < key.size() - 1; ++i) {
        if (i == 9 || i == 14 || i == 19 || i == 24) {
            key[i] = '-';
        } else {
            const char ch = hex[source++];
            key[i] = ch >= 'a' && ch <= 'f' ? static_cast<char>(ch - 'a' + 'A') : ch;
        }
    }
    return key;
}

inline bool is_canonical_uuid_index_key(std::string_view text) {
    std::array<char, 32> hex;
    if (text.size() != 38 || !canonical_uuid(text, hex)) return false;
    const auto key = uuid_index_key(hex);
    return text == std::string_view(key.data(), key.size());
}

} // namespace explorgdb
#endif // EXPLORGDB_UUID_VALUE_H
