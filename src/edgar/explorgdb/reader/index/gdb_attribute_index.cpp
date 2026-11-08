// src/edgar/explorgdb/gdb_attribute_index.cpp
// .atx 属性索引解析实现
//
// 解析流程与 .spx 相同（B+ 树结构一致），区别在于值解码和查询逻辑。

#include "gdb_attribute_index.h"
#include "binary_reader.h"
#include "explorgdb_constants.h"
#include "uuid_value.h"
#include "gdb_attribute_string_key.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace explorgdb {
namespace {

size_t leaf_header_size(uint32_t version) {
    return version == 1U ? 12U : 20U;
}

size_t nonleaf_header_size(uint32_t version) {
    return version == 1U ? 8U : 12U;
}

size_t max_per_page(const BPlusTreeTrailer& trailer) {
    const size_t header_size = leaf_header_size(trailer.version);
    const size_t object_id_size = trailer.object_id_size;
    if (trailer.page_size < header_size || object_id_size == 0U ||
        trailer.value_size == 0U) {
        return 0U;
    }
    return (trailer.page_size - header_size) /
           (object_id_size + trailer.value_size);
}

ObjectId read_object_id(BinaryReader& reader, uint8_t object_id_size) {
    if (object_id_size == sizeof(uint32_t)) return reader.read_u32();
    if (object_id_size == sizeof(uint64_t)) return reader.read_u64();
    throw std::out_of_range("unsupported index ObjectID width");
}

bool page_offset_for(ObjectId page_id,
                     size_t page_count,
                     size_t page_size,
                     size_t& page_offset) {
    if (page_id == 0U || page_id > page_count || page_size == 0U) return false;
    const ObjectId page_index = page_id - 1U;
    if (page_index > static_cast<ObjectId>(
                         std::numeric_limits<size_t>::max() / page_size)) {
        return false;
    }
    page_offset = static_cast<size_t>(page_index) * page_size;
    return true;
}

} // namespace

GdbAttributeIndexParser::GdbAttributeIndexParser(const std::string& file_path)
    : file_path_(file_path) {}

GdbAttributeIndexParser::GdbAttributeIndexParser(
    const std::string& file_path, FieldType field_type)
    : explicit_field_type_(true), file_path_(file_path), field_type_(field_type) {}

bool GdbAttributeIndexParser::valid_key_layout() const {
    const auto width = trailer_.value_size;
    if (!explicit_field_type_) {
        return trailer_.is_string ? width != 0 && width % 2 == 0 :
            trailer_.is_numeric && (width == 2 || width == 4 || width == 8);
    }
    switch (field_type_) {
        case FieldType::String: return trailer_.is_string && width != 0 && width % 2 == 0;
        case FieldType::UUID_1:
        case FieldType::UUID_2: return !trailer_.is_string && !trailer_.is_numeric && width == 38;
        case FieldType::Int16: return trailer_.is_numeric && width == 2;
        case FieldType::Int32:
        case FieldType::Float32: return trailer_.is_numeric && width == 4;
        case FieldType::ObjectId: return trailer_.is_numeric && (width == 4 || width == 8);
        case FieldType::Int64:
        case FieldType::Float64:
        case FieldType::DateTime:
        case FieldType::Date:
        case FieldType::Time:
        case FieldType::DateTimeWithOffset: return trailer_.is_numeric && width == 8;
        default: return false;
    }
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbAttributeIndexParser::parse() {
    file_data_.clear();
    all_entries_.clear();
    string_value_offsets_.clear();
    trailer_ = BPlusTreeTrailer{};
    page_count_ = 0U;

    std::ifstream input(std::filesystem::u8path(file_path_),
                        std::ios::binary | std::ios::ate);
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

    if (file_data_.size() < sizeof(uint32_t)) return false;
    BinaryReader version_reader(
        file_data_.data() + file_data_.size() - sizeof(uint32_t),
        sizeof(uint32_t));
    trailer_.version = version_reader.read_u32();
    if (trailer_.version == 1U) {
        trailer_.page_size = static_cast<uint32_t>(kPageSize);
        trailer_.object_id_size = 4U;
    } else if (trailer_.version == 2U) {
        trailer_.page_size = static_cast<uint32_t>(kV2PageSize);
        trailer_.object_id_size = 8U;
    } else {
        return false;
    }
    const size_t trailer_size = trailer_.version == 1U
        ? kTrailerSize : kV2TrailerSize;
    if (file_data_.size() < trailer_size) return false;
    const size_t page_bytes = file_data_.size() - trailer_size;
    if (page_bytes % trailer_.page_size != 0U) return false;
    page_count_ = page_bytes / trailer_.page_size;

    try {
        if (!parse_trailer() || trailer_.value_size == 0) return false;

        if (trailer_.tree_depth == 0) {
            return trailer_.total_value_count == 0;
        }
        if (page_count_ == 0U || trailer_.tree_depth > 4U ||
            trailer_.tree_depth > page_count_) return false;

        size_t remaining_page_visits = page_count_;
        if (!traverse_tree(1, static_cast<int>(trailer_.tree_depth),
                           remaining_page_visits)) {
            all_entries_.clear();
            return false;
        }
    } catch (const std::exception&) {
        all_entries_.clear();
        return false;
    }

    if (trailer_.total_value_count >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        all_entries_.size() !=
            static_cast<size_t>(trailer_.total_value_count)) {
        all_entries_.clear();
        return false;
    }
    for (const AttributeIndexEntry& entry : all_entries_) {
        RowSlot row_slot = 0;
        if (!object_id_to_row_slot(entry.fid, row_slot)) {
            all_entries_.clear();
            return false;
        }
    }
    return true;
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbAttributeIndexParser::parse_trailer() {
    const size_t trailer_size = trailer_.version == 1U
        ? kTrailerSize : kV2TrailerSize;
    const size_t trailer_offset = file_data_.size() - trailer_size;
    BinaryReader reader(file_data_.data() + trailer_offset, trailer_size);

    trailer_.value_size = reader.read_u8();
    const uint8_t flags = reader.read_u8();
    trailer_.is_numeric = (flags & 0x40U) != 0;
    // Floating/date keys use 0x60; their 0x20 bit does not imply UTF-16.
    trailer_.is_string = !trailer_.is_numeric && (flags & 0x20U) != 0;
    trailer_.magic1 = reader.read_u32();
    trailer_.tree_depth = reader.read_u32();
    trailer_.total_value_count = trailer_.version == 1U
        ? reader.read_u32() : reader.read_u64();
    const size_t reserved_bytes = trailer_.version == 1U ? 4U : 8U;
    reader.skip(reserved_bytes);
    const uint32_t stored_version = reader.read_u32();

    const bool uuid_field = field_type_ == FieldType::UUID_1 || field_type_ == FieldType::UUID_2;
    if (uuid_field) {
        if (trailer_.value_size != 38U || trailer_.is_string || trailer_.is_numeric) return false;
    } else if (!trailer_.is_string && !trailer_.is_numeric) {
        return false;
    }
    return trailer_.magic1 == 1U && stored_version == trailer_.version && valid_key_layout();
}

GdbAttributeIndexParser::PageInfo
GdbAttributeIndexParser::parse_page_info(size_t page_offset) const {
    BinaryReader reader(file_data_.data() + page_offset, trailer_.page_size);
    PageInfo info;
    info.next_page_id = read_object_id(reader, trailer_.object_id_size);
    info.entry_count = reader.read_u32();
    return info;
}

void GdbAttributeIndexParser::parse_leaf_page(
    size_t page_offset,
    std::vector<AttributeIndexEntry>& output) {
    BinaryReader reader(file_data_.data() + page_offset, trailer_.page_size);

    const ObjectId next_page_id = read_object_id(
        reader, trailer_.object_id_size);
    (void)next_page_id;
    const uint32_t feature_count = reader.read_u32();
    const size_t header_size = leaf_header_size(trailer_.version);
    reader.skip(header_size - reader.tell());

    const size_t capacity = max_per_page(trailer_);
    if (capacity == 0U || feature_count > capacity)
        throw std::out_of_range("attribute index leaf entry count");

    std::vector<ObjectId> fids(feature_count);
    for (uint32_t index = 0; index < feature_count; ++index)
        fids[index] = read_object_id(reader, trailer_.object_id_size);

    const size_t values_offset = header_size +
        capacity * trailer_.object_id_size;
    reader.seek(values_offset);

    for (uint32_t index = 0; index < feature_count; ++index) {
        if (trailer_.is_string) string_value_offsets_.push_back(page_offset + reader.tell());
        std::vector<uint8_t> value_bytes(trailer_.value_size);
        for (uint8_t byte = 0; byte < trailer_.value_size; ++byte)
            value_bytes[byte] = reader.read_u8();

        output.push_back(decode_value(
            value_bytes.data(), trailer_.value_size,
            trailer_.is_string, fids[index]));
    }
}

std::vector<ObjectId> GdbAttributeIndexParser::parse_nonleaf_page(
    size_t page_offset) {
    BinaryReader reader(file_data_.data() + page_offset, trailer_.page_size);

    const ObjectId next_page_id = read_object_id(
        reader, trailer_.object_id_size);
    (void)next_page_id;
    const uint32_t subpage_count = reader.read_u32();
    const size_t header_size = nonleaf_header_size(trailer_.version);
    reader.skip(header_size - reader.tell());

    const size_t capacity = max_per_page(trailer_);
    if (capacity == 0U || subpage_count > capacity)
        throw std::out_of_range("attribute index child count");

    std::vector<ObjectId> page_ids(subpage_count);
    for (uint32_t index = 0; index < subpage_count; ++index)
        page_ids[index] = read_object_id(reader, trailer_.object_id_size);

    const size_t values_offset = leaf_header_size(trailer_.version) +
        capacity * trailer_.object_id_size;
    reader.seek(values_offset);
    for (uint32_t index = 0; index < subpage_count; ++index) {
        for (uint8_t byte = 0; byte < trailer_.value_size; ++byte)
            (void)reader.read_u8();
    }

    return page_ids;
}

bool GdbAttributeIndexParser::traverse_tree(
    ObjectId page_id,
    int depth_remaining,
    size_t& remaining_page_visits) {
    if (page_id == 0 || depth_remaining <= 0 ||
        remaining_page_visits == 0) {
        return false;
    }
    --remaining_page_visits;

    size_t page_offset = 0;
    if (!page_offset_for(page_id, page_count_, trailer_.page_size,
                         page_offset))
        return false;

    if (depth_remaining == 1) {
        ObjectId current = page_id;
        bool first_page = true;
        while (current != 0) {
            if (!first_page) {
                if (remaining_page_visits == 0) return false;
                --remaining_page_visits;
            }
            first_page = false;

            size_t offset = 0;
            if (!page_offset_for(current, page_count_, trailer_.page_size,
                                 offset))
                return false;

            const PageInfo page_info = parse_page_info(offset);
            const ObjectId next_page_id = page_info.next_page_id;
            if (next_page_id > page_count_) return false;
            parse_leaf_page(offset, all_entries_);
            current = next_page_id;
        }
        return true;
    }

    const std::vector<ObjectId> child_ids =
        parse_nonleaf_page(page_offset);
    if (child_ids.empty()) return false;
    for (ObjectId child_id : child_ids) {
        if (child_id == 0U || child_id > page_count_) return false;
    }
    return traverse_tree(child_ids.front(), depth_remaining - 1,
                         remaining_page_visits);
}

AttributeIndexEntry GdbAttributeIndexParser::decode_value(
    const uint8_t* bytes,
    uint8_t value_size,
    bool is_string,
    ObjectId fid) const {
    AttributeIndexEntry entry;
    entry.fid = fid;

    if (field_type_ == FieldType::UUID_1 || field_type_ == FieldType::UUID_2) {
        const std::string_view key(reinterpret_cast<const char*>(bytes), value_size);
        if (!is_canonical_uuid_index_key(key))
            throw std::out_of_range("invalid GUID attribute index key");
        entry.string_value.assign(key.data(), key.size());
        entry.numeric_value = std::nan("");
    } else if (is_string) {
        attribute_string::KeyInfo info;
        if ((value_size % 2) != 0 || !attribute_string::inspect(bytes, value_size / 2, info))
            throw std::out_of_range("invalid UTF-16 attribute index key");
        entry.string_value = utf16le_to_utf8(bytes, static_cast<int>(info.text_units));
        entry.numeric_value = std::nan("");
    } else {
        switch (value_size) {
            case 2: {
                const uint16_t raw = static_cast<uint16_t>(bytes[0]) |
                    (static_cast<uint16_t>(bytes[1]) << 8U);
                entry.numeric_value = raw < 0x8000U
                    ? static_cast<double>(raw)
                    : static_cast<double>(static_cast<int32_t>(raw) - 0x10000);
                break;
            }
            case 4: {
                if (field_type_ == FieldType::Float32) {
                    float value;
                    std::memcpy(&value, bytes, sizeof(value));
                    entry.numeric_value = static_cast<double>(value);
                } else {
                    int32_t value;
                    std::memcpy(&value, bytes, sizeof(value));
                    entry.numeric_value = static_cast<double>(value);
                }
                break;
            }
            case 8: {
                if (field_type_ == FieldType::Int64) {
                    uint64_t raw = 0;
                    for (size_t i = 0; i < 8; ++i) raw |= static_cast<uint64_t>(bytes[i]) << (8 * i);
                    const int64_t value = raw <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                        ? static_cast<int64_t>(raw) : -1 - static_cast<int64_t>(~raw);
                    entry.integer_value = value;
                    entry.numeric_value = static_cast<double>(value);
                } else {
                    double value;
                    std::memcpy(&value, bytes, sizeof(value));
                    entry.numeric_value = value;
                }
                break;
            }
            default:
                throw std::out_of_range("unsupported numeric attribute key width");
        }
        if (std::isnan(entry.numeric_value))
            throw std::out_of_range("unordered NaN attribute index key");
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

std::vector<RowSlot> GdbAttributeIndexParser::query_double(
    double value,
    AttrOp op) const {
    std::vector<RowSlot> result;
    if (field_type_ == FieldType::UUID_1 || field_type_ == FieldType::UUID_2 ||
        field_type_ == FieldType::Int64 || std::isnan(value)) return result;
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
        if (match) result.push_back(static_cast<RowSlot>(entry.fid - 1U));
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<RowSlot> GdbAttributeIndexParser::query_int64(int64_t value, AttrOp op) const {
    std::vector<RowSlot> result;
    if (field_type_ != FieldType::Int64) return result;
    for (const auto& entry : all_entries_) {
        if (!entry.integer_value.has_value()) continue;
        const int order = (*entry.integer_value > value) - (*entry.integer_value < value);
        const bool matches = op == AttrOp::Eq ? order == 0 : op == AttrOp::Ne ? order != 0 :
            op == AttrOp::Lt ? order < 0 : op == AttrOp::Le ? order <= 0 :
            op == AttrOp::Gt ? order > 0 : order >= 0;
        if (matches) result.push_back(entry.fid - 1);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<RowSlot> GdbAttributeIndexParser::query_string(
    const std::string& value,
    AttrOp op) const {
    std::vector<RowSlot> result;
    std::string effective_value = value;
    const bool uuid_field = field_type_ == FieldType::UUID_1 || field_type_ == FieldType::UUID_2;
    std::u16string key;
    if (uuid_field) {
        std::array<char, 32> hex;
        if (!canonical_uuid(value, hex)) return result;
        const auto encoded = uuid_index_key(hex);
        effective_value.assign(encoded.data(), encoded.size());
    } else if (!trailer_.is_string || string_value_offsets_.size() != all_entries_.size() ||
               !attribute_string::prepare(value, trailer_.value_size / 2, key)) {
        return result;
    }

    for (size_t i = 0; i < all_entries_.size(); ++i) {
        const auto& entry = all_entries_[i];
        const int comparison = uuid_field ? entry.string_value.compare(effective_value) :
            attribute_string::compare(file_data_.data() + string_value_offsets_[i], key);
        bool match = false;
        switch (op) {
            case AttrOp::Eq: match = comparison == 0; break;
            case AttrOp::Ne: match = comparison != 0; break;
            case AttrOp::Lt: match = comparison < 0; break;
            case AttrOp::Le: match = comparison <= 0; break;
            case AttrOp::Gt: match = comparison > 0; break;
            case AttrOp::Ge: match = comparison >= 0; break;
        }
        if (match) result.push_back(static_cast<RowSlot>(entry.fid - 1U));
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace explorgdb
