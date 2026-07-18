// src/edgar/explorgdb/gdb_attribute_index.cpp
// .atx 属性索引解析实现
//
// 解析流程与 .spx 相同（B+ 树结构一致），区别在于值解码和查询逻辑。

#include "gdb_attribute_index.h"
#include "binary_reader.h"
#include "explorgdb_constants.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace explorgdb {
namespace {

int max_per_page(uint8_t value_size) {
    return static_cast<int>(
        (GdbAttributeIndexParser::kPageSize - 12) / (4 + value_size));
}

bool page_offset_for(uint32_t page_id,
                     size_t file_size,
                     size_t& page_offset) {
    if (page_id == 0) return false;
    const uint64_t raw_offset =
        static_cast<uint64_t>(page_id - 1U) *
        static_cast<uint64_t>(GdbAttributeIndexParser::kPageSize);
    if (raw_offset >
        static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    page_offset = static_cast<size_t>(raw_offset);
    return page_offset <= file_size &&
           file_size - page_offset >= GdbAttributeIndexParser::kPageSize;
}

} // namespace

GdbAttributeIndexParser::GdbAttributeIndexParser(
    const std::string& file_path)
    : file_path_(file_path) {}

bool GdbAttributeIndexParser::parse() {
    file_data_.clear();
    all_entries_.clear();
    trailer_ = BPlusTreeTrailer{};

    std::ifstream input(file_path_, std::ios::binary | std::ios::ate);
    if (!input.is_open()) return false;

    const std::streamoff end_offset = input.tellg();
    if (end_offset < 0) return false;
    const uintmax_t unsigned_size = static_cast<uintmax_t>(end_offset);
    if (unsigned_size >
            static_cast<uintmax_t>(std::numeric_limits<size_t>::max()) ||
        unsigned_size > static_cast<uintmax_t>(
            std::numeric_limits<std::streamsize>::max())) {
        return false;
    }

    const size_t byte_count = static_cast<size_t>(unsigned_size);
    input.seekg(0, std::ios::beg);
    file_data_.resize(byte_count);
    if (byte_count != 0) {
        input.read(reinterpret_cast<char*>(file_data_.data()),
                   static_cast<std::streamsize>(byte_count));
        if (!input) return false;
    }

    if (file_data_.size() < kTrailerSize) return false;
    const size_t page_bytes = file_data_.size() - kTrailerSize;
    if (page_bytes % kPageSize != 0) return false;
    const size_t page_count = page_bytes / kPageSize;

    try {
        if (!parse_trailer() || trailer_.value_size == 0) return false;

        if (trailer_.tree_depth == 0) {
            return trailer_.total_value_count == 0;
        }
        if (page_count == 0 || trailer_.tree_depth > page_count) return false;

        size_t remaining_page_visits = page_count;
        if (!traverse_tree(1, static_cast<int>(trailer_.tree_depth),
                           remaining_page_visits)) {
            all_entries_.clear();
            return false;
        }
    } catch (const std::exception&) {
        all_entries_.clear();
        return false;
    }

    if (all_entries_.size() !=
        static_cast<size_t>(trailer_.total_value_count)) {
        all_entries_.clear();
        return false;
    }
    for (const AttributeIndexEntry& entry : all_entries_) {
        if (entry.fid == 0) {
            all_entries_.clear();
            return false;
        }
    }
    return true;
}

bool GdbAttributeIndexParser::parse_trailer() {
    const size_t trailer_offset = file_data_.size() - kTrailerSize;
    BinaryReader reader(file_data_.data() + trailer_offset, kTrailerSize);

    trailer_.value_size = reader.read_u8();
    const uint8_t flags = reader.read_u8();
    trailer_.is_string = (flags & 0x20U) != 0;
    trailer_.is_numeric = (flags & 0x40U) != 0;
    trailer_.magic1 = reader.read_u32();
    trailer_.tree_depth = reader.read_u32();
    trailer_.total_value_count = reader.read_u32();

    return trailer_.magic1 == 1;
}

GdbAttributeIndexParser::PageInfo
GdbAttributeIndexParser::parse_page_info(size_t page_offset) const {
    BinaryReader reader(file_data_.data() + page_offset, kPageSize);
    PageInfo info;
    info.next_page_id = reader.read_u32();
    info.entry_count = reader.read_u32();
    return info;
}

void GdbAttributeIndexParser::parse_leaf_page(
    size_t page_offset,
    std::vector<AttributeIndexEntry>& output) {
    BinaryReader reader(file_data_.data() + page_offset, kPageSize);

    const uint32_t next_page_id = reader.read_u32();
    (void)next_page_id;
    const uint32_t feature_count = reader.read_u32();
    const uint32_t unknown = reader.read_u32();
    (void)unknown;

    const int capacity = max_per_page(trailer_.value_size);
    if (capacity <= 0 || feature_count > static_cast<uint32_t>(capacity))
        throw std::out_of_range("attribute index leaf entry count");

    std::vector<uint32_t> fids(feature_count);
    for (uint32_t index = 0; index < feature_count; ++index)
        fids[index] = reader.read_u32();

    const size_t values_offset =
        12U + static_cast<size_t>(capacity) * 4U;
    reader.seek(values_offset);

    for (uint32_t index = 0; index < feature_count; ++index) {
        std::vector<uint8_t> value_bytes(trailer_.value_size);
        for (uint8_t byte = 0; byte < trailer_.value_size; ++byte)
            value_bytes[byte] = reader.read_u8();

        output.push_back(decode_value(
            value_bytes.data(), trailer_.value_size,
            trailer_.is_string, fids[index]));
    }
}

std::vector<uint32_t> GdbAttributeIndexParser::parse_nonleaf_page(
    size_t page_offset) {
    BinaryReader reader(file_data_.data() + page_offset, kPageSize);

    const uint32_t next_page_id = reader.read_u32();
    (void)next_page_id;
    const uint32_t subpage_count = reader.read_u32();

    const int capacity = max_per_page(trailer_.value_size);
    if (capacity <= 0 || subpage_count > static_cast<uint32_t>(capacity))
        throw std::out_of_range("attribute index child count");

    std::vector<uint32_t> page_ids(subpage_count);
    for (uint32_t index = 0; index < subpage_count; ++index)
        page_ids[index] = reader.read_u32();

    const size_t values_offset =
        12U + static_cast<size_t>(capacity) * 4U;
    reader.seek(values_offset);
    for (uint32_t index = 0; index < subpage_count; ++index) {
        for (uint8_t byte = 0; byte < trailer_.value_size; ++byte)
            (void)reader.read_u8();
    }

    return page_ids;
}

bool GdbAttributeIndexParser::traverse_tree(
    uint32_t page_id,
    int depth_remaining,
    size_t& remaining_page_visits) {
    if (page_id == 0 || depth_remaining <= 0 ||
        remaining_page_visits == 0) {
        return false;
    }
    --remaining_page_visits;

    size_t page_offset = 0;
    if (!page_offset_for(page_id, file_data_.size(), page_offset))
        return false;

    if (depth_remaining == 1) {
        uint32_t current = page_id;
        bool first_page = true;
        while (current != 0) {
            if (!first_page) {
                if (remaining_page_visits == 0) return false;
                --remaining_page_visits;
            }
            first_page = false;

            size_t offset = 0;
            if (!page_offset_for(current, file_data_.size(), offset))
                return false;

            BinaryReader reader(file_data_.data() + offset, kPageSize);
            const uint32_t next_page_id = reader.read_u32();
            parse_leaf_page(offset, all_entries_);
            current = next_page_id;
        }
        return true;
    }

    const std::vector<uint32_t> child_ids =
        parse_nonleaf_page(page_offset);
    if (child_ids.empty()) return false;
    return traverse_tree(child_ids.front(), depth_remaining - 1,
                         remaining_page_visits);
}

AttributeIndexEntry GdbAttributeIndexParser::decode_value(
    const uint8_t* bytes,
    uint8_t value_size,
    bool is_string,
    uint32_t fid) const {
    AttributeIndexEntry entry;
    entry.fid = fid;

    if (is_string) {
        const int character_count = value_size / 2;
        std::string value;
        for (int index = 0; index < character_count; ++index) {
            const uint16_t character =
                bytes[index * 2] |
                (static_cast<uint16_t>(bytes[index * 2 + 1]) << 8U);
            if (character == 0x0000 || character == 0x0020) break;
            if (character < 0x80) {
                value += static_cast<char>(character);
            } else if (character < 0x800) {
                value += static_cast<char>(0xC0 | (character >> 6));
                value += static_cast<char>(0x80 | (character & 0x3F));
            } else {
                value += static_cast<char>(0xE0 | (character >> 12));
                value += static_cast<char>(
                    0x80 | ((character >> 6) & 0x3F));
                value += static_cast<char>(0x80 | (character & 0x3F));
            }
        }
        entry.string_value = std::move(value);
        entry.numeric_value = std::nan("");
    } else {
        switch (value_size) {
            case 2:
                entry.numeric_value = static_cast<double>(
                    bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8U));
                break;
            case 4: {
                int32_t value;
                std::memcpy(&value, bytes, sizeof(value));
                entry.numeric_value = static_cast<double>(value);
                break;
            }
            case 8: {
                double value;
                std::memcpy(&value, bytes, sizeof(value));
                entry.numeric_value = value;
                break;
            }
            default:
                entry.numeric_value = 0;
                break;
        }
    }

    return entry;
}

int GdbAttributeIndexParser::compare_value(
    const AttributeIndexEntry& entry,
    double numeric,
    const std::string& string_value,
    bool is_string) const {
    if (is_string) return entry.string_value.compare(string_value);
    const double indexed_value = entry.numeric_value;
    if (indexed_value < numeric) return -1;
    if (indexed_value > numeric) return 1;
    return 0;
}

std::vector<uint32_t> GdbAttributeIndexParser::query_double(
    double value,
    AttrOp op) const {
    std::vector<uint32_t> result;
    const std::string unused;

    for (const AttributeIndexEntry& entry : all_entries_) {
        const int comparison = compare_value(entry, value, unused, false);
        bool match = false;
        switch (op) {
            case AttrOp::Eq: match = comparison == 0; break;
            case AttrOp::Ne: match = comparison != 0; break;
            case AttrOp::Lt: match = comparison < 0; break;
            case AttrOp::Le: match = comparison <= 0; break;
            case AttrOp::Gt: match = comparison > 0; break;
            case AttrOp::Ge: match = comparison >= 0; break;
        }
        if (match) result.push_back(entry.fid - 1U);
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<uint32_t> GdbAttributeIndexParser::query_string(
    const std::string& value,
    AttrOp op) const {
    std::vector<uint32_t> result;

    for (const AttributeIndexEntry& entry : all_entries_) {
        const int comparison = entry.string_value.compare(value);
        bool match = false;
        switch (op) {
            case AttrOp::Eq: match = comparison == 0; break;
            case AttrOp::Ne: match = comparison != 0; break;
            case AttrOp::Lt: match = comparison < 0; break;
            case AttrOp::Le: match = comparison <= 0; break;
            case AttrOp::Gt: match = comparison > 0; break;
            case AttrOp::Ge: match = comparison >= 0; break;
        }
        if (match) result.push_back(entry.fid - 1U);
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace explorgdb