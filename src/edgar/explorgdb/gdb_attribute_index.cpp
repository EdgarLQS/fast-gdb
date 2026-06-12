// src/edgar/explorgdb/gdb_attribute_index.cpp
// .atx 属性索引解析实现
//
// 解析流程与 .spx 相同（B+ 树结构一致），区别在于值解码和查询逻辑。

#include "gdb_attribute_index.h"
#include "binary_reader.h"
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace explorgdb {

static inline int max_per_page(uint8_t value_size) {
    return static_cast<int>((GdbAttributeIndexParser::kPageSize - 12) / (4 + value_size));
}

GdbAttributeIndexParser::GdbAttributeIndexParser(const std::string& file_path)
    : file_path_(file_path) {}

// ── 解析 ──
bool GdbAttributeIndexParser::parse() {
    std::ifstream ifs(file_path_, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) return false;

    auto file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    file_data_.resize(static_cast<size_t>(file_size));
    ifs.read(reinterpret_cast<char*>(file_data_.data()), file_size);

    if (file_data_.size() < kTrailerSize) return false;
    if (!parse_trailer()) return false;

    all_entries_.clear();
    if (trailer_.tree_depth > 0) {
        traverse_tree(1, static_cast<int>(trailer_.tree_depth));
    }

    return true;
}

// ── Trailer ──
bool GdbAttributeIndexParser::parse_trailer() {
    size_t trailer_off = file_data_.size() - kTrailerSize;
    BinaryReader br(file_data_.data() + trailer_off, kTrailerSize);

    trailer_.value_size = br.read_u8();
    uint8_t flags = br.read_u8();
    trailer_.is_string = (flags & 0x20) != 0;
    trailer_.is_numeric = (flags & 0x40) != 0;
    trailer_.magic1 = br.read_u32();
    trailer_.tree_depth = br.read_u32();
    trailer_.total_value_count = br.read_u32();

    if (trailer_.magic1 != 1) return false;
    return true;
}

// ── 页面信息 ──
GdbAttributeIndexParser::PageInfo GdbAttributeIndexParser::parse_page_info(size_t page_offset) const {
    BinaryReader br(file_data_.data() + page_offset, kPageSize);
    PageInfo info;
    info.next_page_id = br.read_u32();
    info.entry_count = br.read_u32();
    return info;
}

// ── 解析叶子页面 ──
void GdbAttributeIndexParser::parse_leaf_page(size_t page_offset, std::vector<AttributeIndexEntry>& out) {
    BinaryReader br(file_data_.data() + page_offset, kPageSize);

    uint32_t next_page_id = br.read_u32(); (void)next_page_id;
    uint32_t n_features = br.read_u32();
    uint32_t unknown = br.read_u32(); (void)unknown;

    int mpp = max_per_page(trailer_.value_size);

    // FID 数组
    std::vector<uint32_t> fids(n_features);
    for (uint32_t i = 0; i < n_features; ++i) {
        fids[i] = br.read_u32();
    }

    // 值数组
    size_t values_off = 12 + static_cast<size_t>(mpp) * 4;
    br.seek(values_off);

    for (uint32_t i = 0; i < n_features; ++i) {
        std::vector<uint8_t> val_bytes(trailer_.value_size);
        for (uint8_t j = 0; j < trailer_.value_size; ++j) {
            val_bytes[j] = br.read_u8();
        }

        out.push_back(decode_value(val_bytes.data(), trailer_.value_size,
                                    trailer_.is_string, fids[i]));
    }
}

// ── 解析非叶子页面 ──
std::vector<uint32_t> GdbAttributeIndexParser::parse_nonleaf_page(size_t page_offset) {
    BinaryReader br(file_data_.data() + page_offset, kPageSize);

    uint32_t next_page_id = br.read_u32(); (void)next_page_id;
    uint32_t n_subpages = br.read_u32();

    int mpp = max_per_page(trailer_.value_size);

    std::vector<uint32_t> page_ids(n_subpages);
    for (uint32_t i = 0; i < n_subpages; ++i) {
        page_ids[i] = br.read_u32();
    }

    // 跳过分隔值
    size_t values_off = 12 + static_cast<size_t>(mpp) * 4;
    br.seek(values_off);
    for (uint32_t i = 0; i < n_subpages; ++i) {
        for (uint8_t j = 0; j < trailer_.value_size; ++j) {
            br.read_u8();
        }
    }

    return page_ids;
}

// ── B+ 树遍历 ──
// 叶子页面通过 next_page_id 形成链表。只需要找到第一个叶子，然后跟随链表。
void GdbAttributeIndexParser::traverse_tree(uint32_t page_id, int depth_remaining) {
    if (page_id == 0 || depth_remaining <= 0) return;

    size_t page_offset = static_cast<size_t>(page_id - 1) * kPageSize;
    if (page_offset + kPageSize > file_data_.size()) return;

    if (depth_remaining == 1) {
        // 叶子页面：只处理第一个，然后跟随 next_page_id 链表
        uint32_t current = page_id;
        while (current != 0) {
            size_t off = static_cast<size_t>(current - 1) * kPageSize;
            if (off + kPageSize > file_data_.size()) break;

            BinaryReader br(file_data_.data() + off, kPageSize);
            uint32_t next_page_id = br.read_u32();
            parse_leaf_page(off, all_entries_);
            current = next_page_id;
        }
    } else {
        // 非叶子页面：只走第一个子页面（最左路径）
        auto child_ids = parse_nonleaf_page(page_offset);
        if (!child_ids.empty()) {
            traverse_tree(child_ids[0], depth_remaining - 1);
        }
    }
}

// ── 值解码 ──
AttributeIndexEntry GdbAttributeIndexParser::decode_value(
    const uint8_t* bytes, uint8_t value_size, bool is_string, uint32_t fid) const {

    AttributeIndexEntry entry;
    entry.fid = fid;

    if (is_string) {
        // UTF16-LE → UTF-8, 去除尾部空格填充 (0x0020)
        int n_chars = value_size / 2;
        std::string s;
        for (int i = 0; i < n_chars; ++i) {
            uint16_t ch = bytes[i * 2] | (static_cast<uint16_t>(bytes[i * 2 + 1]) << 8);
            if (ch == 0x0000 || ch == 0x0020) break;
            // UTF-16 → UTF-8 编码
            if (ch < 0x80) {
                s += static_cast<char>(ch);
            } else if (ch < 0x800) {
                s += static_cast<char>(0xC0 | (ch >> 6));
                s += static_cast<char>(0x80 | (ch & 0x3F));
            } else {
                s += static_cast<char>(0xE0 | (ch >> 12));
                s += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                s += static_cast<char>(0x80 | (ch & 0x3F));
            }
        }
        entry.string_value = s;
        entry.numeric_value = std::nan("");
    } else {
        // 数值类型
        switch (value_size) {
            case 2:
                entry.numeric_value = static_cast<double>(bytes[0] | (bytes[1] << 8));
                break;
            case 4: {
                int32_t v;
                std::memcpy(&v, bytes, 4);
                entry.numeric_value = static_cast<double>(v);
                break;
            }
            case 8: {
                double v;
                std::memcpy(&v, bytes, 8);
                entry.numeric_value = v;
                break;
            }
            default:
                entry.numeric_value = 0;
                break;
        }
    }

    return entry;
}

// ── 比较 ──
int GdbAttributeIndexParser::compare_value(const AttributeIndexEntry& entry, double numeric,
                                             const std::string& str, bool is_string) const {
    if (is_string) {
        return entry.string_value.compare(str);
    } else {
        double ev = entry.numeric_value;
        if (ev < numeric) return -1;
        if (ev > numeric) return 1;
        return 0;
    }
}

// ── 数值查询 ──
std::vector<uint32_t> GdbAttributeIndexParser::query_double(double value, AttrOp op) const {
    std::vector<uint32_t> result;
    std::string dummy;

    for (const auto& entry : all_entries_) {
        int cmp = compare_value(entry, value, dummy, false);
        bool match = false;
        switch (op) {
            case AttrOp::Eq: match = (cmp == 0); break;
            case AttrOp::Ne: match = (cmp != 0); break;
            case AttrOp::Lt: match = (cmp < 0); break;
            case AttrOp::Le: match = (cmp <= 0); break;
            case AttrOp::Gt: match = (cmp > 0); break;
            case AttrOp::Ge: match = (cmp >= 0); break;
        }
        if (match) {
            result.push_back(entry.fid - 1);  // 1-based → 0-based
        }
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

// ── 字符串查询 ──
std::vector<uint32_t> GdbAttributeIndexParser::query_string(const std::string& value, AttrOp op) const {
    std::vector<uint32_t> result;

    for (const auto& entry : all_entries_) {
        int cmp = entry.string_value.compare(value);
        bool match = false;
        switch (op) {
            case AttrOp::Eq: match = (cmp == 0); break;
            case AttrOp::Ne: match = (cmp != 0); break;
            case AttrOp::Lt: match = (cmp < 0); break;
            case AttrOp::Le: match = (cmp <= 0); break;
            case AttrOp::Gt: match = (cmp > 0); break;
            case AttrOp::Ge: match = (cmp >= 0); break;
        }
        if (match) {
            result.push_back(entry.fid - 1);
        }
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace explorgdb
