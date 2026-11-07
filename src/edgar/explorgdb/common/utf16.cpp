#include "utf16.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace explorgdb {
namespace {

void append_utf8(std::string& output, uint32_t codepoint) {
    if (codepoint <= 0x7FU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        output.push_back(
            static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        output.push_back(
            static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        output.push_back(
            static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

void append_utf16(std::u16string& output, uint32_t codepoint) {
    if (codepoint <= 0xFFFFU) {
        output.push_back(static_cast<char16_t>(codepoint));
        return;
    }

    codepoint -= 0x10000U;
    output.push_back(static_cast<char16_t>(0xD800U + (codepoint >> 10U)));
    output.push_back(static_cast<char16_t>(0xDC00U + (codepoint & 0x3FFU)));
}

bool is_continuation(uint8_t value) {
    return (value & 0xC0U) == 0x80U;
}

}  // namespace

std::string utf16le_to_utf8(const uint8_t* data, int char_count) {
    std::string output;
    if (data == nullptr || char_count <= 0) return output;

    output.reserve(static_cast<size_t>(char_count) * 3U);
    for (int index = 0; index < char_count; ++index) {
        uint32_t codepoint = static_cast<uint32_t>(data[index * 2]) |
                             (static_cast<uint32_t>(data[index * 2 + 1]) << 8U);
        if (codepoint == 0U) break;

        if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
            if (index + 1 < char_count) {
                const uint32_t low =
                    static_cast<uint32_t>(data[(index + 1) * 2]) |
                    (static_cast<uint32_t>(data[(index + 1) * 2 + 1]) << 8U);
                if (low >= 0xDC00U && low <= 0xDFFFU) {
                    codepoint = 0x10000U +
                                ((codepoint - 0xD800U) << 10U) +
                                (low - 0xDC00U);
                    ++index;
                } else {
                    codepoint = 0xFFFDU;
                }
            } else {
                codepoint = 0xFFFDU;
            }
        } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
            codepoint = 0xFFFDU;
        }
        append_utf8(output, codepoint);
    }
    return output;
}

static std::u16string convert_utf8(const std::string& input, bool& all_valid) {
    all_valid = true;
    std::u16string output;
    output.reserve(input.size());

    for (size_t index = 0; index < input.size();) {
        const uint8_t first = static_cast<uint8_t>(input[index]);
        uint32_t codepoint = 0xFFFDU;
        size_t sequence_length = 1U;
        uint32_t minimum_codepoint = 0U;

        if (first <= 0x7FU) {
            codepoint = first;
        } else if (first >= 0xC2U && first <= 0xDFU) {
            codepoint = first & 0x1FU;
            sequence_length = 2U;
            minimum_codepoint = 0x80U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            codepoint = first & 0x0FU;
            sequence_length = 3U;
            minimum_codepoint = 0x800U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            codepoint = first & 0x07U;
            sequence_length = 4U;
            minimum_codepoint = 0x10000U;
        }

        bool valid = sequence_length == 1U ? first <= 0x7FU : true;
        if (valid && sequence_length > input.size() - index) valid = false;
        if (valid) {
            for (size_t offset = 1U; offset < sequence_length; ++offset) {
                const uint8_t continuation =
                    static_cast<uint8_t>(input[index + offset]);
                if (!is_continuation(continuation)) {
                    valid = false;
                    break;
                }
                codepoint = (codepoint << 6U) | (continuation & 0x3FU);
            }
        }
        if (valid && sequence_length > 1U) {
            valid = codepoint >= minimum_codepoint &&
                    codepoint <= 0x10FFFFU &&
                    !(codepoint >= 0xD800U && codepoint <= 0xDFFFU);
        }

        if (!valid) {
            all_valid = false;
            append_utf16(output, 0xFFFDU);
            ++index;
            continue;
        }

        append_utf16(output, codepoint);
        index += sequence_length;
    }
    return output;
}

std::u16string utf8_to_utf16(const std::string& input) {
    bool valid = true;
    return convert_utf8(input, valid);
}

bool try_utf8_to_utf16(const std::string& input, std::u16string& output) {
    bool valid = true;
    auto converted = convert_utf8(input, valid);
    if (!valid) return false;
    output = std::move(converted);
    return true;
}

}  // namespace explorgdb
