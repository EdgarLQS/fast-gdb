// src/edgar/explorgdb/writer/bplus_tree_writer.h
// B+ 树索引写入器 — .spx/.atx 共享的通用 B+ 树构建
//
// FileGDB B+ 树格式：
//   - 固定 4096 字节页面
//   - 末尾 22 字节 trailer
//   - 页面布局（value_size=8 时 mpp=340）：
//     分支页：next_page_id(4) + entry_count(4) + child_ids[mpp*4] + values[mpp*8]
//     叶子页：next_page_id(4) + entry_count(4) + UNUSED(4) + fids[mpp*4] + values[mpp*8]
//
// 使用方式（模板类）：
//   BPlusTreeWriter<uint64_t> writer;
//   writer.set_value_size(8);     // 空间索引
//   writer.set_numeric(true);
//   writer.add_entry(value1, fid1);
//   writer.add_entry(value2, fid2);
//   writer.write("a00000001.spx");

#ifndef EXPLORGDB_BPLUS_TREE_WRITER_H
#define EXPLORGDB_BPLUS_TREE_WRITER_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <algorithm>
#include <utility>

namespace explorgdb {
namespace writer {

// B+ 树常量
static constexpr size_t kBPlusPageSize = 4096;       // 固定页面大小
static constexpr size_t kBPlusTrailerSize = 22;      // trailer 大小
static constexpr size_t kPageHeaderSize = 12;        // next_page(4) + entry_count(4) + pad(4)
static constexpr int kMaxTreeDepth = 4;              // 最大树深度

// 计算每页最大条目数
// mpp = (4096 - 12) / (4 + value_size)
inline int calc_max_per_page(uint8_t value_size) {
    return static_cast<int>((kBPlusPageSize - kPageHeaderSize) / (4 + value_size));
}

// 计算值数组的固定偏移
// values_offset = 12 + max_per_page * 4
inline int calc_values_offset(int max_per_page) {
    return static_cast<int>(kPageHeaderSize) + max_per_page * 4;
}

// B+ 树写入器（模板类）
// T: 值类型（uint16_t, uint32_t, uint64_t, double, std::string）
template<typename T>
class BPlusTreeWriter {
public:
    BPlusTreeWriter() = default;

    // 配置
    void set_value_size(uint8_t size) { value_size_ = size; }
    void set_numeric(bool v) { is_numeric_ = v; }
    void set_string(bool v) { is_string_ = v; }

    // 添加条目（value, fid）对
    void add_entry(T value, uint32_t fid) {
        entries_.emplace_back(std::move(value), fid);
    }

    // 清空
    void clear() { entries_.clear(); }

    // 写入文件
    bool write(const std::string& path) const;

    // 条目数
    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

private:
    // 计算树深度
    int calc_tree_depth() const {
        if (entries_.empty()) return 0;
        int mpp = calc_max_per_page(value_size_);
        size_t n = entries_.size();
        int depth = 1;
        size_t capacity = mpp;
        while (n > capacity && depth < kMaxTreeDepth) {
            capacity = capacity * mpp + 1;
            ++depth;
        }
        return depth;
    }

    // 写入叶子页
    void write_leaf_page(std::vector<uint8_t>& page,
                         const std::vector<std::pair<T, uint32_t>>& entries,
                         size_t start, size_t count,
                         uint32_t next_page_id) const;

    // 写入分支页
    void write_branch_page(std::vector<uint8_t>& page,
                           const std::vector<uint64_t>& child_ids,
                           const std::vector<T>& separators,
                           uint32_t next_page_id) const;

    // 写入 trailer
    void write_trailer(std::vector<uint8_t>& data) const;

    // 值序列化
    void serialize_value(std::vector<uint8_t>& buf, const T& value) const;

    uint8_t value_size_ = 8;
    bool is_numeric_ = true;
    bool is_string_ = false;
    std::vector<std::pair<T, uint32_t>> entries_;
};

// ─── 模板实现 ───

// 值序列化特化
template<>
inline void BPlusTreeWriter<uint16_t>::serialize_value(std::vector<uint8_t>& buf, const uint16_t& v) const {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

template<>
inline void BPlusTreeWriter<uint32_t>::serialize_value(std::vector<uint8_t>& buf, const uint32_t& v) const {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

template<>
inline void BPlusTreeWriter<uint64_t>::serialize_value(std::vector<uint8_t>& buf, const uint64_t& v) const {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

template<>
inline void BPlusTreeWriter<double>::serialize_value(std::vector<uint8_t>& buf, const double& v) const {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
    }
}

template<typename T>
void BPlusTreeWriter<T>::write_leaf_page(
    std::vector<uint8_t>& page,
    const std::vector<std::pair<T, uint32_t>>& entries,
    size_t start, size_t count,
    uint32_t next_page_id) const
{
    page.assign(kBPlusPageSize, 0);
    int mpp = calc_max_per_page(value_size_);
    int values_off = calc_values_offset(mpp);

    // header
    page[0] = static_cast<uint8_t>(next_page_id & 0xFF);
    page[1] = static_cast<uint8_t>((next_page_id >> 8) & 0xFF);
    page[2] = static_cast<uint8_t>((next_page_id >> 16) & 0xFF);
    page[3] = static_cast<uint8_t>((next_page_id >> 24) & 0xFF);

    uint32_t cnt = static_cast<uint32_t>(count);
    page[4] = static_cast<uint8_t>(cnt & 0xFF);
    page[5] = static_cast<uint8_t>((cnt >> 8) & 0xFF);
    page[6] = static_cast<uint8_t>((cnt >> 16) & 0xFF);
    page[7] = static_cast<uint8_t>((cnt >> 24) & 0xFF);

    // bytes 8-11: UNUSED (padding)

    // FID 数组（从偏移 12 开始）
    size_t pos = kPageHeaderSize;
    for (size_t i = 0; i < count; ++i) {
        uint32_t fid = entries[start + i].second;
        page[pos++] = static_cast<uint8_t>(fid & 0xFF);
        page[pos++] = static_cast<uint8_t>((fid >> 8) & 0xFF);
        page[pos++] = static_cast<uint8_t>((fid >> 16) & 0xFF);
        page[pos++] = static_cast<uint8_t>((fid >> 24) & 0xFF);
    }

    // 值数组（从固定偏移开始）
    std::vector<uint8_t> val_buf;
    val_buf.reserve(count * value_size_);
    for (size_t i = 0; i < count; ++i) {
        serialize_value(val_buf, entries[start + i].first);
    }
    if (val_buf.size() > 0) {
        std::memcpy(page.data() + values_off, val_buf.data(), val_buf.size());
    }
}

template<typename T>
void BPlusTreeWriter<T>::write_branch_page(
    std::vector<uint8_t>& page,
    const std::vector<uint64_t>& child_ids,
    const std::vector<T>& separators,
    uint32_t next_page_id) const
{
    page.assign(kBPlusPageSize, 0);
    int mpp = calc_max_per_page(value_size_);
    int values_off = calc_values_offset(mpp);
    size_t count = child_ids.size();

    // header
    page[0] = static_cast<uint8_t>(next_page_id & 0xFF);
    page[1] = static_cast<uint8_t>((next_page_id >> 8) & 0xFF);
    page[2] = static_cast<uint8_t>((next_page_id >> 16) & 0xFF);
    page[3] = static_cast<uint8_t>((next_page_id >> 24) & 0xFF);

    uint32_t cnt = static_cast<uint32_t>(count);
    page[4] = static_cast<uint8_t>(cnt & 0xFF);
    page[5] = static_cast<uint8_t>((cnt >> 8) & 0xFF);
    page[6] = static_cast<uint8_t>((cnt >> 16) & 0xFF);
    page[7] = static_cast<uint8_t>((cnt >> 24) & 0xFF);

    // child_ids 数组（从偏移 8 开始）— 注意分支页没有 padding，child_ids 从 8 开始
    // 但为了统一，我们还是从 12 开始（和叶子页的 fid 数组对齐）
    // 实际上分支页结构：next_page(4) + entry_count(4) + child_ids[N*4] + values[N*value_size]
    // 没有 UNUSED padding，但 GDAL 实现中 child_ids 从偏移 8 开始
    // 让我重新检查 GDAL 源码...
    // 根据 gdb_spatial_index.cpp 注释：
    //   bytes 8..8+N*4: child_page_id 数组 (N+1 × 4)
    //   bytes 12+mpp*4..: 值数组
    // 所以 child_ids 从偏移 8 开始，不是 12！
    size_t pos = 8;  // child_ids 从偏移 8 开始
    for (size_t i = 0; i < count; ++i) {
        uint64_t cid = child_ids[i];
        page[pos++] = static_cast<uint8_t>(cid & 0xFF);
        page[pos++] = static_cast<uint8_t>((cid >> 8) & 0xFF);
        page[pos++] = static_cast<uint8_t>((cid >> 16) & 0xFF);
        page[pos++] = static_cast<uint8_t>((cid >> 24) & 0xFF);
    }

    // 值数组（分隔符，从固定偏移开始）
    std::vector<uint8_t> val_buf;
    val_buf.reserve(separators.size() * value_size_);
    for (size_t i = 0; i < separators.size(); ++i) {
        serialize_value(val_buf, separators[i]);
    }
    if (val_buf.size() > 0) {
        std::memcpy(page.data() + values_off, val_buf.data(), val_buf.size());
    }
}

template<typename T>
void BPlusTreeWriter<T>::write_trailer(std::vector<uint8_t>& data) const {
    // 22 字节 trailer
    uint8_t flags = 0;
    if (is_string_) flags |= 0x20;
    if (is_numeric_) flags |= 0x40;

    data.push_back(value_size_);
    data.push_back(flags);
    // magic1 = 1
    for (int i = 0; i < 4; ++i) data.push_back(i == 0 ? 1 : 0);
    // tree_depth
    int depth = calc_tree_depth();
    for (int i = 0; i < 4; ++i) data.push_back(static_cast<uint8_t>((depth >> (i * 8)) & 0xFF));
    // total_value_count
    uint32_t total = static_cast<uint32_t>(entries_.size());
    for (int i = 0; i < 4; ++i) data.push_back(static_cast<uint8_t>((total >> (i * 8)) & 0xFF));
    // padding to 22 bytes (already 1+1+4+4+4 = 14, need 8 more)
    for (int i = 0; i < 8; ++i) data.push_back(0);
}

template<typename T>
bool BPlusTreeWriter<T>::write(const std::string& path) const {
    if (entries_.empty()) return false;

    // 1. 排序 entries
    auto sorted = entries_;
    std::sort(sorted.begin(), sorted.end());

    int mpp = calc_max_per_page(value_size_);
    size_t n = sorted.size();

    std::vector<std::vector<uint8_t>> all_pages;

    // 简化实现：只支持单层叶子页（depth=1）
    // 对于大数据集，需要扩展到多层 B+ 树
    size_t num_pages = (n + mpp - 1) / mpp;

    for (size_t i = 0; i < num_pages; ++i) {
        size_t start = i * mpp;
        size_t count = std::min(static_cast<size_t>(mpp), n - start);
        uint32_t next_page = (i + 1 < num_pages) ? static_cast<uint32_t>(i + 1) : 0;
        std::vector<uint8_t> page;
        write_leaf_page(page, sorted, start, count, next_page);
        all_pages.push_back(std::move(page));
    }

    // 写入文件
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;

    for (const auto& page : all_pages) {
        if (std::fwrite(page.data(), 1, kBPlusPageSize, fp) != kBPlusPageSize) {
            std::fclose(fp);
            return false;
        }
    }

    // 写 trailer（tree_depth=1 for single level）
    uint8_t flags = 0;
    if (is_string_) flags |= 0x20;
    if (is_numeric_) flags |= 0x40;

    std::vector<uint8_t> trailer;
    trailer.reserve(kBPlusTrailerSize);
    trailer.push_back(value_size_);
    trailer.push_back(flags);
    // magic1 = 1
    for (int i = 0; i < 4; ++i) trailer.push_back(i == 0 ? 1 : 0);
    // tree_depth = 1
    for (int i = 0; i < 4; ++i) trailer.push_back(i == 0 ? 1 : 0);
    // total_value_count
    uint32_t total = static_cast<uint32_t>(n);
    for (int i = 0; i < 4; ++i) trailer.push_back(static_cast<uint8_t>((total >> (i * 8)) & 0xFF));
    // padding to 22 bytes
    for (int i = 0; i < 8; ++i) trailer.push_back(0);

    if (std::fwrite(trailer.data(), 1, kBPlusTrailerSize, fp) != kBPlusTrailerSize) {
        std::fclose(fp);
        return false;
    }

    std::fclose(fp);
    return true;
}

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_BPLUS_TREE_WRITER_H
