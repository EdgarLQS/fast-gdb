// src/edgar/explorgdb/reader/index/gdb_attribute_index_query.cpp
// .atx 直接查询 — 按页下探 B+ 树并只物化命中的零基 FID。

#include "gdb_attribute_index.h"
#include "gdb_tablx_cache.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

namespace explorgdb {
namespace {

using AttributeClock = std::chrono::steady_clock;

// metrics 为 nullptr 时返回默认时间点，热路径不会读取时钟。
AttributeClock::time_point metric_start(
    AttributeIndexQueryMetrics* metrics) {
    return metrics == nullptr ? AttributeClock::time_point{}
                              : AttributeClock::now();
}

double elapsed_ms(AttributeClock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        AttributeClock::now() - start).count();
}

void metric_set(AttributeIndexQueryMetrics* metrics,
                double AttributeIndexQueryMetrics::*member,
                AttributeClock::time_point start) {
    if (metrics != nullptr) metrics->*member = elapsed_ms(start);
}

size_t leaf_header_size(uint32_t version) {
    return version == 1U ? 12U : 20U;
}

size_t nonleaf_header_size(uint32_t version) {
    return version == 1U ? 8U : 12U;
}

/** 根据 trailer 布局计算页面能容纳的最大 ObjectID/value 对数量。 */
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

ObjectId read_object_id(const uint8_t* bytes, uint8_t object_id_size) {
    ObjectId value = 0U;
    for (size_t index = 0; index < object_id_size; ++index) {
        value |= static_cast<ObjectId>(bytes[index]) << (index * 8U);
    }
    return value;
}

/** 将 1-based 页号转为文件偏移，并在乘法前限制到 size_t 范围。 */
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

uint32_t read_u32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8U) |
           (static_cast<uint32_t>(bytes[2]) << 16U) |
           (static_cast<uint32_t>(bytes[3]) << 24U);
}

bool comparison_matches(int comparison, AttrOp op) {
    switch (op) {
        case AttrOp::Eq: return comparison == 0;
        case AttrOp::Ne: return comparison != 0;
        case AttrOp::Lt: return comparison < 0;
        case AttrOp::Le: return comparison <= 0;
        case AttrOp::Gt: return comparison > 0;
        case AttrOp::Ge: return comparison >= 0;
    }
    return false;
}

/** 按 trailer 的 value_size 解码当前数值物理表示。 */
bool decode_numeric(const uint8_t* bytes,
                    uint8_t value_size,
                    bool decode_as_int64,
                    bool decode_as_float32,
                    double& double_value,
                    int64_t& integer_value) {
    switch (value_size) {
        case 2: {
            if (decode_as_int64) return false;
            const uint16_t raw = static_cast<uint16_t>(bytes[0]) |
                (static_cast<uint16_t>(bytes[1]) << 8U);
            double_value = raw < 0x8000U
                ? static_cast<double>(raw)
                : static_cast<double>(static_cast<int32_t>(raw) - 0x10000);
            return true;
        }
        case 4: {
            if (decode_as_int64) return false;
            if (decode_as_float32) {
                float raw = 0;
                std::memcpy(&raw, bytes, sizeof(raw));
                double_value = static_cast<double>(raw);
            } else {
                int32_t raw = 0;
                std::memcpy(&raw, bytes, sizeof(raw));
                double_value = static_cast<double>(raw);
            }
            return true;
        }
        case 8:
            if (decode_as_int64) {
                uint64_t raw = 0;
                for (size_t index = 0; index < sizeof(raw); ++index) {
                    raw |= static_cast<uint64_t>(bytes[index]) <<
                           (index * 8U);
                }
                if (raw <= static_cast<uint64_t>(
                               std::numeric_limits<int64_t>::max())) {
                    integer_value = static_cast<int64_t>(raw);
                } else {
                    integer_value = -1 - static_cast<int64_t>(~raw);
                }
            } else {
                std::memcpy(&double_value, bytes, sizeof(double_value));
            }
            return true;
        default:
            return false;
    }
}

size_t utf8_sequence_bytes(unsigned char lead) {
    if ((lead & 0x80U) == 0) return 1;
    if ((lead & 0xe0U) == 0xc0U) return 2;
    if ((lead & 0xf0U) == 0xe0U) return 3;
    if ((lead & 0xf8U) == 0xf0U) return 4;
    return 0;
}

/**
 * 按索引可容纳的 UTF-16 单元数截断查询字符串。
 *
 * .atx 字符串尾部以空格填充；遇到空格或损坏 UTF-8 序列即停止。此函数只
 * 准备索引比较键，完整 WHERE 仍由上层对候选记录复核。
 */
std::string truncate_index_string(const std::string& value,
                                  size_t max_utf16_units) {
    std::string result;
    size_t units = 0;
    for (size_t index = 0;
         index < value.size() && units < max_utf16_units;) {
        if (value[index] == ' ') break;
        const size_t bytes = utf8_sequence_bytes(
            static_cast<unsigned char>(value[index]));
        if (bytes == 0 || index + bytes > value.size()) break;
        result.append(value, index, bytes);
        index += bytes;
        ++units;
    }
    return result;
}

} // namespace

bool GdbAttributeIndexParser::query_double_direct(
    double value,
    AttrOp op,
    size_t max_fid_count,
    std::vector<RowSlot>& result,
    AttributeIndexQueryMetrics* metrics) {
    const std::string unused;
    return query_direct(value, 0, false, unused, false, op,
                        max_fid_count, result, metrics);
}

bool GdbAttributeIndexParser::query_int64_direct(
    int64_t value,
    AttrOp op,
    size_t max_fid_count,
    std::vector<RowSlot>& result,
    AttributeIndexQueryMetrics* metrics) {
    const std::string unused;
    return query_direct(0.0, value, true, unused, false, op,
                        max_fid_count, result, metrics);
}

bool GdbAttributeIndexParser::query_string_direct(
    const std::string& value,
    AttrOp op,
    size_t max_fid_count,
    std::vector<RowSlot>& result,
    AttributeIndexQueryMetrics* metrics) {
    return query_direct(0.0, 0, false, value, true, op,
                        max_fid_count, result, metrics);
}

bool GdbAttributeIndexParser::query_direct(
    double numeric_value,
    int64_t integer_value,
    bool numeric_is_int64,
    const std::string& string_value,
    bool is_string,
    AttrOp op,
    size_t max_fid_count,
    std::vector<RowSlot>& result,
    AttributeIndexQueryMetrics* metrics) {
    if (metrics != nullptr) *metrics = AttributeIndexQueryMetrics{};
    const auto total_start = metric_start(metrics);

    // Direct queries use bounded page reads; parse() remains the explicit
    // full-materialization API for diagnostics and compatibility.
    std::vector<uint8_t>().swap(file_data_);
    std::vector<AttributeIndexEntry>().swap(all_entries_);
    trailer_ = BPlusTreeTrailer{};

    TablxCacheKey source_before;
    if (!TablxCache::make_key(file_path_, source_before)) return false;

    // ── 文件大小与 trailer ──
    const auto load_start = metric_start(metrics);
    std::ifstream input(std::filesystem::u8path(file_path_),
                        std::ios::binary | std::ios::ate);
    if (!input.is_open()) return false;
    const std::streamoff end_offset = input.tellg();
    if (end_offset < 0) return false;
    const uintmax_t unsigned_size = static_cast<uintmax_t>(end_offset);
    if (unsigned_size > static_cast<uintmax_t>(
                            std::numeric_limits<size_t>::max()) ||
        unsigned_size > static_cast<uintmax_t>(
                            std::numeric_limits<std::streamsize>::max())) {
        return false;
    }

    const size_t byte_count = static_cast<size_t>(unsigned_size);
    if (byte_count < sizeof(uint32_t)) return false;
    std::array<uint8_t, sizeof(uint32_t)> version_bytes{};
    input.clear();
    input.seekg(static_cast<std::streamoff>(
                    byte_count - sizeof(uint32_t)),
                std::ios::beg);
    input.read(reinterpret_cast<char*>(version_bytes.data()),
               static_cast<std::streamsize>(version_bytes.size()));
    if (!input) return false;
    const uint32_t version = read_u32(version_bytes.data());
    if (version == 1U) {
        trailer_.version = 1U;
        trailer_.page_size = static_cast<uint32_t>(kPageSize);
        trailer_.object_id_size = 4U;
    } else if (version == 2U) {
        trailer_.version = 2U;
        trailer_.page_size = static_cast<uint32_t>(kV2PageSize);
        trailer_.object_id_size = 8U;
    } else {
        return false;
    }
    const size_t trailer_size = version == 1U ? kTrailerSize : kV2TrailerSize;
    const size_t page_size = trailer_.page_size;
    if (byte_count < trailer_size) return false;
    const size_t page_bytes = byte_count - trailer_size;
    if (page_bytes % page_size != 0U) return false;
    const size_t page_count = page_bytes / page_size;
    if (metrics != nullptr) {
        metrics->file_bytes = byte_count;
        metrics->file_load_ms = elapsed_ms(load_start);
    }

    const auto trailer_start = metric_start(metrics);
    std::array<uint8_t, GdbAttributeIndexParser::kV2TrailerSize>
        trailer_storage{};
    uint8_t* trailer_bytes = trailer_storage.data();
    input.clear();
    input.seekg(static_cast<std::streamoff>(page_bytes), std::ios::beg);
    input.read(reinterpret_cast<char*>(trailer_bytes),
               static_cast<std::streamsize>(trailer_size));
    if (!input) return false;
    trailer_.value_size = trailer_bytes[0];
    const uint8_t flags = trailer_bytes[1];
    trailer_.is_string = (flags & 0x20U) != 0;
    trailer_.is_numeric = (flags & 0x40U) != 0;
    trailer_.magic1 = read_u32(trailer_bytes + 2U);
    trailer_.tree_depth = read_u32(trailer_bytes + 6U);
    trailer_.total_value_count = version == 1U
        ? read_u32(trailer_bytes + 10U)
        : read_object_id(trailer_bytes + 10U, 8U);
    const uint32_t stored_version = read_u32(
        trailer_bytes + trailer_size - sizeof(uint32_t));
    if (trailer_.magic1 != 1U || trailer_.value_size == 0U ||
        stored_version != version) return false;
    if (metrics != nullptr) {
        metrics->page_count = page_count;
        metrics->trailer_ms = elapsed_ms(trailer_start);
    }

    if ((is_string && !trailer_.is_string) ||
        (!is_string && !trailer_.is_numeric) ||
        (numeric_is_int64 && trailer_.value_size != 8U) ||
        (is_string && (trailer_.value_size % 2U) != 0U)) {
        return false;
    }

    // 深度为 0 只允许真正的空树；空结果是成功，不是回退信号。
    if (trailer_.tree_depth == 0) {
        if (trailer_.total_value_count != 0) return false;
        TablxCacheKey source_after;
        if (!TablxCache::make_key(file_path_, source_after) ||
            !(source_before == source_after)) {
            return false;
        }
        result.clear();
        if (metrics != nullptr) {
            metrics->total_ms = elapsed_ms(total_start);
        }
        return true;
    }
    if (page_count == 0 || trailer_.tree_depth > 4U ||
        trailer_.tree_depth > page_count) return false;

    const size_t capacity = max_per_page(trailer_);
    if (capacity == 0U) return false;
    const size_t values_offset = leaf_header_size(version) +
        capacity * trailer_.object_id_size;
    if (values_offset > page_size ||
        static_cast<size_t>(trailer_.value_size) >
            page_size - values_offset) {
        return false;
    }

    const std::string effective_string = is_string
        ? truncate_index_string(
              string_value, trailer_.value_size / 2U)
        : std::string{};

    // A fixed 16-page LRU bounds temporary memory independently of ATX size.
    struct CachedPage {
        ObjectId page_id = 0;
        std::vector<uint8_t> bytes;
        uint64_t last_used = 0;
        bool valid = false;
    };
    std::array<CachedPage, 16> page_cache{};
    for (CachedPage& cached : page_cache) cached.bytes.resize(page_size);
    uint64_t cache_clock = 0;
    const auto read_page = [&](ObjectId page_id) -> const uint8_t* {
        size_t page_offset = 0;
        if (!page_offset_for(page_id, page_count, page_size, page_offset))
            return nullptr;
        for (CachedPage& cached : page_cache) {
            if (cached.valid && cached.page_id == page_id) {
                cached.last_used = ++cache_clock;
                if (metrics != nullptr) ++metrics->pages_visited;
                return cached.bytes.data();
            }
        }
        CachedPage* slot = &page_cache[0];
        for (CachedPage& cached : page_cache) {
            if (!cached.valid) {
                slot = &cached;
                break;
            }
            if (cached.last_used < slot->last_used) slot = &cached;
        }
        input.clear();
        input.seekg(static_cast<std::streamoff>(page_offset), std::ios::beg);
        input.read(reinterpret_cast<char*>(slot->bytes.data()),
                   static_cast<std::streamsize>(slot->bytes.size()));
        if (!input) {
            slot->valid = false;
            return nullptr;
        }
        slot->page_id = page_id;
        slot->last_used = ++cache_clock;
        slot->valid = true;
        if (metrics != nullptr) ++metrics->pages_visited;
        return slot->bytes.data();
    };

    const auto compare_key_to_query = [&](const uint8_t* bytes,
                                          int& comparison) -> bool {
        if (is_string) {
            const AttributeIndexEntry entry = decode_value(
                bytes, trailer_.value_size, true, 0U);
            comparison = entry.string_value.compare(effective_string);
            comparison = (comparison > 0) - (comparison < 0);
            return true;
        }
        double indexed_value = 0.0;
        int64_t indexed_integer = 0;
        if (!decode_numeric(bytes, trailer_.value_size, numeric_is_int64,
                            field_type_ == FieldType::Float32,
                            indexed_value, indexed_integer)) {
            return false;
        }
        if (numeric_is_int64) {
            comparison = (indexed_integer > integer_value) -
                         (indexed_integer < integer_value);
        } else {
            comparison = (indexed_value > numeric_value) -
                         (indexed_value < numeric_value);
        }
        return true;
    };
    const auto compare_keys = [&](const uint8_t* lhs,
                                  const uint8_t* rhs,
                                  int& comparison) -> bool {
        if (is_string) {
            const AttributeIndexEntry left = decode_value(
                lhs, trailer_.value_size, true, 0U);
            const AttributeIndexEntry right = decode_value(
                rhs, trailer_.value_size, true, 0U);
            comparison = left.string_value.compare(right.string_value);
            comparison = (comparison > 0) - (comparison < 0);
            return true;
        }
        double left_value = 0.0;
        double right_value = 0.0;
        int64_t left_integer = 0;
        int64_t right_integer = 0;
        if (!decode_numeric(lhs, trailer_.value_size, numeric_is_int64,
                            field_type_ == FieldType::Float32,
                            left_value, left_integer) ||
            !decode_numeric(rhs, trailer_.value_size, numeric_is_int64,
                            field_type_ == FieldType::Float32,
                            right_value, right_integer)) {
            return false;
        }
        if (numeric_is_int64) {
            comparison = (left_integer > right_integer) -
                         (left_integer < right_integer);
        } else {
            comparison = (left_value > right_value) -
                         (left_value < right_value);
        }
        return true;
    };

    const bool descend_to_lower_bound =
        op == AttrOp::Eq || op == AttrOp::Ge || op == AttrOp::Gt;
    bool starts_at_leftmost = true;
    ObjectId leaf_page_id = 1U;
    std::vector<ObjectId> navigation_pages;

    // ── 内部页按键下探 ──
    const auto navigation_start = metric_start(metrics);
    for (uint32_t depth = trailer_.tree_depth; depth > 1U; --depth) {
        if (std::find(navigation_pages.begin(), navigation_pages.end(),
                      leaf_page_id) != navigation_pages.end()) {
            return false;
        }
        navigation_pages.push_back(leaf_page_id);
        const uint8_t* page = read_page(leaf_page_id);
        if (page == nullptr) return false;
        const size_t nonleaf_header = nonleaf_header_size(version);
        const uint32_t entry_count = read_u32(page + trailer_.object_id_size);
        if (entry_count == 0U || entry_count > capacity) {
            return false;
        }

        uint32_t child_index = 0U;
        if (descend_to_lower_bound) {
            child_index = entry_count - 1U;
            for (uint32_t index = 0U; index < entry_count; ++index) {
                const uint8_t* separator = page + values_offset +
                    static_cast<size_t>(index) * trailer_.value_size;
                int comparison = 0;
                if (!compare_key_to_query(separator, comparison)) return false;
                if (index != 0U) {
                    const uint8_t* previous = page + values_offset +
                        static_cast<size_t>(index - 1U) * trailer_.value_size;
                    int order = 0;
                    if (!compare_keys(previous, separator, order) ||
                        order > 0) {
                        return false;
                    }
                }
                const bool is_lower_bound = op == AttrOp::Gt
                    ? comparison > 0 : comparison >= 0;
                if (is_lower_bound) {
                    child_index = index;
                    break;
                }
            }
        }
        if (child_index != 0U) starts_at_leftmost = false;
        for (uint32_t index = 0U; index < entry_count; ++index) {
            const ObjectId child = read_object_id(
                page + nonleaf_header +
                    static_cast<size_t>(index) * trailer_.object_id_size,
                trailer_.object_id_size);
            if (child == 0U || child > page_count) {
                return false;
            }
        }
        leaf_page_id = read_object_id(
            page + nonleaf_header +
                static_cast<size_t>(child_index) * trailer_.object_id_size,
            trailer_.object_id_size);
    }
    metric_set(metrics,
               &AttributeIndexQueryMetrics::tree_navigation_ms,
               navigation_start);

    // ── 定位叶页扫描到目标范围结束 ──
    // 候选在本次查询成功前只保存在局部 vector，失败时由上层回退扫描。
    const auto scan_start = metric_start(metrics);
    std::vector<RowSlot> candidates;
    size_t entries_seen = 0;
    ObjectId current_page_id = leaf_page_id;
    size_t leaf_pages_seen = 0;
    std::vector<uint8_t> previous_key(trailer_.value_size);
    bool have_previous_key = false;
    bool stopped_at_range_end = false;
    while (current_page_id != 0U) {
        if (++leaf_pages_seen > page_count) return false;
        const uint8_t* page = read_page(current_page_id);
        if (page == nullptr) return false;
        const ObjectId next_page_id = read_object_id(
            page, trailer_.object_id_size);
        if (next_page_id > page_count) return false;
        const uint32_t entry_count = read_u32(
            page + trailer_.object_id_size);
        if (entry_count > capacity) return false;

        for (uint32_t index = 0; index < entry_count; ++index) {
            const ObjectId stored_fid = read_object_id(
                page + leaf_header_size(version) +
                    static_cast<size_t>(index) * trailer_.object_id_size,
                trailer_.object_id_size);
            if (stored_fid == 0U ||
                (max_fid_count != 0U &&
                 stored_fid > max_fid_count)) {
                return false;
            }

            const uint8_t* value_bytes =
                page + values_offset +
                static_cast<size_t>(index) * trailer_.value_size;
            int comparison = 0;
            if (!compare_key_to_query(value_bytes, comparison)) return false;
            if (have_previous_key) {
                int order = 0;
                if (!compare_keys(previous_key.data(), value_bytes, order) ||
                    order > 0) {
                    return false;
                }
            }

            if (comparison_matches(comparison, op)) {
                // .atx 存储 1-based ObjectID，Reader 对外统一发布 0-based FID。
                RowSlot row_slot = 0U;
                if (!object_id_to_row_slot(stored_fid, row_slot)) return false;
                candidates.push_back(row_slot);
            }
            std::memcpy(previous_key.data(), value_bytes,
                        trailer_.value_size);
            have_previous_key = true;
            ++entries_seen;

            if ((op == AttrOp::Eq && comparison > 0) ||
                (op == AttrOp::Lt && comparison >= 0) ||
                (op == AttrOp::Le && comparison > 0)) {
                stopped_at_range_end = true;
                break;
            }
        }

        if (stopped_at_range_end) break;
        if (static_cast<uint64_t>(entries_seen) >
            trailer_.total_value_count) {
            return false;
        }
        current_page_id = next_page_id;
    }

    if (current_page_id == 0U && !stopped_at_range_end &&
        starts_at_leftmost &&
        static_cast<uint64_t>(entries_seen) !=
            trailer_.total_value_count) {
        return false;
    }
    if (metrics != nullptr) {
        metrics->entries_scanned = entries_seen;
        metrics->leaf_scan_ms = elapsed_ms(scan_start);
    }

    // ── 发布候选 ──
    // 索引物理顺序不作为公开保证；统一排序去重后再交给 QueryEngine。
    const auto order_start = metric_start(metrics);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(
        std::unique(candidates.begin(), candidates.end()),
        candidates.end());
    if (metrics != nullptr) {
        metrics->candidate_count = candidates.size();
        metrics->candidate_order_ms = elapsed_ms(order_start);
    }

    TablxCacheKey source_after;
    if (!TablxCache::make_key(file_path_, source_after) ||
        !(source_before == source_after)) {
        return false;
    }
    result = std::move(candidates);
    if (metrics != nullptr) metrics->total_ms = elapsed_ms(total_start);
    return true;
}

} // namespace explorgdb
