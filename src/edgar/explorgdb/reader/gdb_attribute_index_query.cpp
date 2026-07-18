// src/edgar/explorgdb/reader/gdb_attribute_index_query.cpp
// .atx 直接查询 — 校验完整 B+ 树叶链并只物化命中的零基 FID。

#include "gdb_attribute_index.h"

#include <algorithm>
#include <chrono>
#include <cstring>
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

/** 根据值宽度计算单页能够容纳的最大 FID/value 对数量。 */
int max_per_page(uint8_t value_size) {
    if (value_size == 0) return 0;
    return static_cast<int>(
        (GdbAttributeIndexParser::kPageSize - 12U) /
        (4U + static_cast<size_t>(value_size)));
}

/**
 * 将 1-based page id 转为文件偏移，并在乘法前限制到 size_t 范围。
 */
bool page_offset_for(uint32_t page_id,
                     size_t page_count,
                     size_t& page_offset) {
    if (page_id == 0 || page_id > page_count) return false;
    const uint64_t raw_offset =
        static_cast<uint64_t>(page_id - 1U) *
        static_cast<uint64_t>(GdbAttributeIndexParser::kPageSize);
    if (raw_offset > static_cast<uint64_t>(
                         std::numeric_limits<size_t>::max())) {
        return false;
    }
    page_offset = static_cast<size_t>(raw_offset);
    return true;
}

uint32_t read_u32(const uint8_t* bytes) {
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
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
                    double& value) {
    switch (value_size) {
        case 2: {
            const uint16_t raw = static_cast<uint16_t>(bytes[0]) |
                (static_cast<uint16_t>(bytes[1]) << 8U);
            value = static_cast<double>(raw);
            return true;
        }
        case 4: {
            int32_t raw = 0;
            std::memcpy(&raw, bytes, sizeof(raw));
            value = static_cast<double>(raw);
            return true;
        }
        case 8:
            std::memcpy(&value, bytes, sizeof(value));
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
    std::vector<uint32_t>& result,
    AttributeIndexQueryMetrics* metrics) {
    const std::string unused;
    return query_direct(value, unused, false, op,
                        max_fid_count, result, metrics);
}

bool GdbAttributeIndexParser::query_string_direct(
    const std::string& value,
    AttrOp op,
    size_t max_fid_count,
    std::vector<uint32_t>& result,
    AttributeIndexQueryMetrics* metrics) {
    return query_direct(0.0, value, true, op,
                        max_fid_count, result, metrics);
}

bool GdbAttributeIndexParser::query_direct(
    double numeric_value,
    const std::string& string_value,
    bool is_string,
    AttrOp op,
    size_t max_fid_count,
    std::vector<uint32_t>& result,
    AttributeIndexQueryMetrics* metrics) {
    if (metrics != nullptr) *metrics = AttributeIndexQueryMetrics{};
    const auto total_start = metric_start(metrics);

    // 每次查询使用当前文件快照；旧物化结果和 trailer 不跨文件版本复用。
    file_data_.clear();
    all_entries_.clear();
    trailer_ = BPlusTreeTrailer{};

    // ── 文件加载与尺寸边界 ──
    const auto load_start = metric_start(metrics);
    std::ifstream input(file_path_, std::ios::binary | std::ios::ate);
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
    input.seekg(0, std::ios::beg);
    file_data_.resize(byte_count);
    if (byte_count != 0) {
        input.read(reinterpret_cast<char*>(file_data_.data()),
                   static_cast<std::streamsize>(byte_count));
        if (!input) return false;
    }
    if (metrics != nullptr) {
        metrics->file_bytes = byte_count;
        metrics->file_load_ms = elapsed_ms(load_start);
    }

    // ── Trailer 与页面布局验证 ──
    const auto trailer_start = metric_start(metrics);
    if (file_data_.size() < kTrailerSize) return false;
    const size_t page_bytes = file_data_.size() - kTrailerSize;
    if (page_bytes % kPageSize != 0) return false;
    const size_t page_count = page_bytes / kPageSize;
    if (!parse_trailer() || trailer_.value_size == 0) return false;
    if (metrics != nullptr) {
        metrics->page_count = page_count;
        metrics->trailer_ms = elapsed_ms(trailer_start);
    }

    if ((is_string && !trailer_.is_string) ||
        (!is_string && !trailer_.is_numeric)) {
        return false;
    }

    // 深度为 0 只允许真正的空树；空结果是成功，不是回退信号。
    if (trailer_.tree_depth == 0) {
        if (trailer_.total_value_count != 0) return false;
        result.clear();
        if (metrics != nullptr) {
            metrics->total_ms = elapsed_ms(total_start);
        }
        return true;
    }
    if (page_count == 0 || trailer_.tree_depth > page_count) return false;

    const int capacity = max_per_page(trailer_.value_size);
    if (capacity <= 0) return false;
    const size_t values_offset =
        12U + static_cast<size_t>(capacity) * 4U;
    if (values_offset > kPageSize ||
        static_cast<size_t>(trailer_.value_size) >
            kPageSize - values_offset) {
        return false;
    }

    const std::string effective_string = is_string
        ? truncate_index_string(
              string_value, trailer_.value_size / 2U)
        : std::string{};

    // visited 同时保护根到叶导航和叶链扫描，任何循环或重复页面均视为损坏。
    std::vector<uint8_t> visited(page_count + 1U, 0U);
    uint32_t leaf_page_id = 1U;

    // ── 根节点导航到最左叶 ──
    // 当前直接查询仍扫描完整叶链，以验证全树计数并正确支持所有比较操作。
    const auto navigation_start = metric_start(metrics);
    for (uint32_t depth = trailer_.tree_depth; depth > 1U; --depth) {
        size_t page_offset = 0;
        if (!page_offset_for(leaf_page_id, page_count, page_offset) ||
            visited[leaf_page_id] != 0U) {
            return false;
        }
        visited[leaf_page_id] = 1U;
        if (metrics != nullptr) ++metrics->pages_visited;

        const uint8_t* page = file_data_.data() + page_offset;
        const uint32_t entry_count = read_u32(page + 4U);
        if (entry_count == 0U ||
            entry_count > static_cast<uint32_t>(capacity)) {
            return false;
        }
        const uint32_t first_child = read_u32(page + 8U);
        if (first_child == 0U) return false;
        leaf_page_id = first_child;
    }
    metric_set(metrics,
               &AttributeIndexQueryMetrics::tree_navigation_ms,
               navigation_start);

    // ── 完整叶链扫描 ──
    // 候选只暂存在局部 vector；文件结构全部通过后才移动到 result，确保 fail closed。
    const auto scan_start = metric_start(metrics);
    std::vector<uint32_t> candidates;
    size_t entries_seen = 0;
    uint32_t current_page_id = leaf_page_id;
    while (current_page_id != 0U) {
        size_t page_offset = 0;
        if (!page_offset_for(current_page_id, page_count, page_offset) ||
            visited[current_page_id] != 0U) {
            return false;
        }
        visited[current_page_id] = 1U;
        if (metrics != nullptr) ++metrics->pages_visited;

        const uint8_t* page = file_data_.data() + page_offset;
        const uint32_t next_page_id = read_u32(page);
        const uint32_t entry_count = read_u32(page + 4U);
        if (entry_count > static_cast<uint32_t>(capacity)) return false;

        // 累积计数在加法前验证，避免损坏条目数产生 size_t 下溢/溢出。
        if (entries_seen > static_cast<size_t>(
                               trailer_.total_value_count) ||
            static_cast<size_t>(entry_count) >
                static_cast<size_t>(trailer_.total_value_count) -
                    entries_seen) {
            return false;
        }

        for (uint32_t index = 0; index < entry_count; ++index) {
            const uint32_t stored_fid =
                read_u32(page + 12U + static_cast<size_t>(index) * 4U);
            if (stored_fid == 0U ||
                (max_fid_count != 0U &&
                 static_cast<size_t>(stored_fid) > max_fid_count)) {
                return false;
            }

            const uint8_t* value_bytes =
                page + values_offset +
                static_cast<size_t>(index) * trailer_.value_size;
            int comparison = 0;
            if (is_string) {
                const AttributeIndexEntry entry = decode_value(
                    value_bytes, trailer_.value_size, true, stored_fid);
                comparison = entry.string_value.compare(effective_string);
            } else {
                double indexed_value = 0.0;
                if (!decode_numeric(
                        value_bytes, trailer_.value_size, indexed_value)) {
                    return false;
                }
                if (indexed_value < numeric_value) comparison = -1;
                else if (indexed_value > numeric_value) comparison = 1;
            }

            if (comparison_matches(comparison, op)) {
                // .atx 存储 1-based ObjectID，Reader 对外统一发布 0-based FID。
                candidates.push_back(stored_fid - 1U);
            }
        }

        entries_seen += entry_count;
        current_page_id = next_page_id;
    }

    if (entries_seen != static_cast<size_t>(
                            trailer_.total_value_count)) {
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
        metrics->total_ms = elapsed_ms(total_start);
    }

    result = std::move(candidates);
    return true;
}

} // namespace explorgdb
