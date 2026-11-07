// src/edgar/explorgdb/gdb_tablx.cpp
// .gdbtablx 偏移索引解析实现
//
// 偏移编码方式（3 种宽度）：
//
// 4 字节偏移:
//   标准小端 uint32，最大支持 4GB 文件
//   byte[0] | (byte[1]<<8) | (byte[2]<<16) | (byte[3]<<24)
//
// 5 字节偏移:
//   byte[0] = 最低 8 位
//   bytes[1:5] = 高 32 位（小端）
//   最终值 = v_low | (v_high << 8)
//   最大支持 ~256TB 文件
//
// 6 字节偏移:
//   byte[0] = 最低 8 位
//   bytes[1:5] = 中间 32 位（小端）
//   byte[5] = 最高 8 位
//   最终值 = v_low | (v_mid << 8) | (v_high << 40)
//   最大支持 ~16EB 文件
//
// 稀疏块位图（仅 v3）:
//   每 1024 个要素为一个块
//   1 bit/块，0 表示整个块无有效要素
//   位图字节数 = (n_bits_for_block_map + 7) / 8（向上取整）

#include "gdb_tablx.h"
#include "binary_reader.h"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <utility>

#ifdef _WIN32
#include "windows_posix_compat.h"
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace explorgdb {

GdbTablxParser::GdbTablxParser(const std::string& file_path)
    : file_path_(file_path) {}

GdbTablxParser::~GdbTablxParser() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    close_file_unlocked();
}

bool GdbTablxParser::open_snapshot_unlocked() {
    close_file_unlocked();
    file_data_.clear();
    offsets_.clear();
    block_bitmap_.clear();
    block_rank_prefix_.clear();
    valid_row_slots_.clear();
    logical_row_count_ = 0;
    valid_feature_count_ = 0;
    hdr_ = {};

#ifdef _WIN32
    file_descriptor_ = fast_gdb_open_utf8(file_path_.c_str(), O_RDONLY);
#else
    file_descriptor_ = ::open(file_path_.c_str(), O_RDONLY);
#endif
    if (file_descriptor_ < 0 ||
        !TablxCache::make_key_from_fd(file_descriptor_, file_identity_) ||
        !source_is_current_unlocked()) {
        close_file_unlocked();
        return false;
    }
    return true;
}

bool GdbTablxParser::open_snapshot() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return open_snapshot_unlocked();
}

void GdbTablxParser::close_file_unlocked() noexcept {
    if (file_descriptor_ >= 0) {
#ifdef _WIN32
        _close(file_descriptor_);
#else
        ::close(file_descriptor_);
#endif
        file_descriptor_ = -1;
    }
    file_identity_ = {};
}

bool GdbTablxParser::source_is_current_unlocked() const noexcept {
    try {
        if (file_descriptor_ < 0) return false;
        TablxCacheKey handle_key;
        TablxCacheKey path_key;
        return TablxCache::make_key_from_fd(file_descriptor_, handle_key) &&
               TablxCache::make_key(file_path_, path_key) &&
               handle_key == file_identity_ && path_key == file_identity_;
    } catch (...) {
        return false;
    }
}

bool GdbTablxParser::source_is_current() const noexcept {
    try {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return source_is_current_unlocked();
    } catch (...) {
        return false;
    }
}

bool GdbTablxParser::parse_opened(bool materialize_valid_fids) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return parse_opened_unlocked(materialize_valid_fids);
}

// ── 主解析入口 ──
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbTablxParser::parse() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!open_snapshot_unlocked()) return false;
    return parse_opened_unlocked(true);
}

std::shared_ptr<const std::vector<uint64_t>> GdbTablxParser::take_offsets() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return std::make_shared<const std::vector<uint64_t>>(
        std::move(offsets_));
}

bool GdbTablxParser::parse_opened_unlocked(bool materialize_valid_fids) {
    file_data_.clear();
    offsets_.clear();
    block_bitmap_.clear();
    block_rank_prefix_.clear();
    valid_row_slots_.clear();
    logical_row_count_ = 0;
    valid_feature_count_ = 0;
    hdr_ = {};

    auto fail = [this]() {
        file_data_.clear();
        offsets_.clear();
        block_bitmap_.clear();
        block_rank_prefix_.clear();
        valid_row_slots_.clear();
        logical_row_count_ = 0;
        valid_feature_count_ = 0;
        hdr_ = {};
        return false;
    };
    if (!source_is_current_unlocked() ||
        file_identity_.file_size > std::numeric_limits<size_t>::max() ||
        file_identity_.file_size > static_cast<uint64_t>(
            std::numeric_limits<off_t>::max()))
        return fail();
    file_data_.resize(static_cast<size_t>(file_identity_.file_size));
    size_t bytes_read = 0;
    while (bytes_read < file_data_.size()) {
        const size_t remaining = file_data_.size() - bytes_read;
        const ssize_t count = pread(
            file_descriptor_, file_data_.data() + bytes_read, remaining,
            static_cast<off_t>(bytes_read));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return fail();
        bytes_read += static_cast<size_t>(count);
    }

    try {
        BinaryReader br(file_data_);
        hdr_.version = br.read_u32();

        if (hdr_.version == 3) {
            hdr_.n1024blocks_v3 = br.read_u32();
            hdr_.nfeatures_v3 = br.read_u32();
            hdr_.size_tablx_offsets = br.read_u32();
        } else if (hdr_.version == 4) {
            hdr_.n1024blocks_v4 = br.read_u64();
            hdr_.size_tablx_offsets = br.read_u32();
        } else {
            std::cerr << "Unknown .gdbtablx version: " << hdr_.version << "\n";
            return fail();
        }

        const int entry_size = static_cast<int>(hdr_.size_tablx_offsets);
        if (entry_size != 4 && entry_size != 5 && entry_size != 6)
            return fail();

        const uint64_t n_blocks_64 = hdr_.version == 3
            ? hdr_.n1024blocks_v3
            : hdr_.n1024blocks_v4;
        if (n_blocks_64 >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max() / 1024U))
            return fail();
        const size_t n_blocks = static_cast<size_t>(n_blocks_64);
        const size_t n_entries = n_blocks * 1024;
        const size_t trailer_size = n_blocks_64 == 0
            ? 0
            : (hdr_.version == 3 ? 16 : 12);
        if (br.tell() > file_data_.size() ||
            file_data_.size() - br.tell() < trailer_size)
            return fail();
        if (n_entries >
            (file_data_.size() - br.tell() - trailer_size) /
                static_cast<size_t>(entry_size))
            return fail();
        const size_t table_bytes =
            n_entries * static_cast<size_t>(entry_size);
        const uint8_t* offset_data = file_data_.data() + br.tell();

        offsets_.resize(n_entries);
        for (size_t i = 0; i < n_entries; ++i) {
            offsets_[i] = read_offset(
                offset_data + i * entry_size, entry_size);
        }
        br.skip(table_bytes);

        if (hdr_.version == 3 && n_blocks_64 != 0) {
            if (!br.can_read(16)) return fail();
            const uint32_t n_bitmap_int32 = br.read_u32();
            const uint32_t n_bits_for_block_map = br.read_u32();
            const uint32_t n1024blocks_bis = br.read_u32();
            const uint32_t n_leading_nonzero = br.read_u32();
            (void)n_leading_nonzero;
            if (n1024blocks_bis != n_blocks_64) return fail();
            logical_row_count_ = hdr_.nfeatures_v3;
            constexpr uint32_t kMaxV3BlockMapBits = 1U +
                static_cast<uint32_t>(
                    std::numeric_limits<int32_t>::max() / 1024);

            // A zero bitmap-word count denotes the format's implicit
            // "all blocks active" representation, even when the metadata
            // retains a non-zero logical bit count.
            if (n_bitmap_int32 != 0) {
                if (n_bits_for_block_map == 0 ||
                    n_bits_for_block_map > kMaxV3BlockMapBits ||
                    logical_row_count_ >
                        static_cast<uint64_t>(n_bits_for_block_map) * 1024U) {
                    return fail();
                }
                const size_t bitmap_bytes =
                    (static_cast<size_t>(n_bits_for_block_map) + 7) / 8;
                if (!br.can_read(bitmap_bytes)) return fail();
                block_bitmap_.resize(n_bits_for_block_map);
                for (size_t i = 0; i < bitmap_bytes; ++i) {
                    const uint8_t byte = br.read_u8();
                    for (int bit = 0; bit < 8; ++bit) {
                        const size_t index = i * 8 + bit;
                        if (index < n_bits_for_block_map)
                            block_bitmap_[index] = (byte >> bit) & 1;
                    }
                }
                const size_t active_blocks = static_cast<size_t>(
                    std::count(block_bitmap_.begin(), block_bitmap_.end(), true));
                if (active_blocks != n_blocks) return fail();
            } else {
                if (n_bits_for_block_map != n_blocks_64 ||
                    logical_row_count_ > offsets_.size()) {
                    return fail();
                }
            }
        } else if (hdr_.version == 3) {
            if (hdr_.nfeatures_v3 != 0) return fail();
            logical_row_count_ = 0;
        } else if (n_blocks != 0) {
            if (!br.can_read(12)) return fail();
            hdr_.n_total_record_count_v4 = br.read_u64();
            hdr_.bitmap_section_size_v4 = br.read_u32();

            logical_row_count_ = hdr_.n_total_record_count_v4;
            if (hdr_.bitmap_section_size_v4 == 0) {
                if (logical_row_count_ > offsets_.size()) return fail();
                for (size_t i = 0; i < offsets_.size(); ++i) {
                    const bool expected_present = i < logical_row_count_;
                    if ((offsets_[i] != 0) != expected_present) return fail();
                }
            } else {
                constexpr size_t kV4BitmapSectionSize = 22 + 32768 + 52;
                constexpr size_t kV4BitmapBytes = 32768;
                constexpr size_t kV4BitmapBlockCount = kV4BitmapBytes * 8;
                constexpr uint64_t kV4BitmapRowCapacity =
                    static_cast<uint64_t>(kV4BitmapBlockCount) * 1024U;
                if (hdr_.bitmap_section_size_v4 != kV4BitmapSectionSize ||
                    logical_row_count_ > kV4BitmapRowCapacity ||
                    !br.can_read(kV4BitmapSectionSize)) {
                    return fail();
                }
                const size_t section_offset = br.tell();
                const uint8_t* section = file_data_.data() + section_offset;
                const uint8_t start_marker[] = {1, 0, 1, 0, 0, 0};
                const uint8_t end_marker[] = {
                    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
                if (!std::equal(std::begin(start_marker), std::end(start_marker),
                                section) ||
                    !std::equal(std::begin(end_marker), std::end(end_marker),
                                section + 22 + kV4BitmapBytes)) {
                    return fail();
                }
                block_bitmap_.resize(kV4BitmapBlockCount);
                for (size_t i = 0; i < kV4BitmapBytes; ++i) {
                    const uint8_t byte = section[22 + i];
                    for (int bit = 0; bit < 8; ++bit) {
                        block_bitmap_[i * 8 + static_cast<size_t>(bit)] =
                            ((byte >> bit) & 1U) != 0;
                    }
                }
                const size_t active_blocks = static_cast<size_t>(
                    std::count(block_bitmap_.begin(), block_bitmap_.end(), true));
                if (active_blocks != n_blocks) return fail();
                br.skip(kV4BitmapSectionSize);
            }

            const size_t parsed_bytes = br.tell();
            const size_t padded_size = (parsed_bytes + 15U) & ~size_t{15U};
            if (padded_size != file_data_.size()) return fail();
            while (br.tell() < file_data_.size()) {
                if (br.read_u8() != 0) return fail();
            }
        } else if (br.tell() != file_data_.size()) {
            return fail();
        }

        if (logical_row_count_ >
            static_cast<RowSlot>(std::numeric_limits<int64_t>::max())) {
            return fail();
        }

        if (!block_bitmap_.empty()) {
            block_rank_prefix_.resize(block_bitmap_.size() + 1, 0);
            for (size_t block = 0; block < block_bitmap_.size(); ++block) {
                block_rank_prefix_[block + 1] = block_rank_prefix_[block] +
                    (block_bitmap_[block] ? 1U : 0U);
            }
        }

        const uint64_t logical_block_count =
            (logical_row_count_ + 1023U) / 1024U;
        for (uint64_t block = 0; block < logical_block_count; ++block) {
            uint64_t physical_block = block;
            if (!block_bitmap_.empty()) {
                if (block >= block_bitmap_.size() || !block_bitmap_[block])
                    continue;
                physical_block = block_rank_prefix_[block];
            }
            const uint64_t first_row = block * 1024U;
            const uint64_t physical_start = physical_block * 1024U;
            const uint64_t rows_in_block = std::min<uint64_t>(
                1024U, logical_row_count_ - first_row);
            for (uint64_t local = 0; local < rows_in_block; ++local) {
                const uint64_t physical_index = physical_start + local;
                if (physical_index < offsets_.size() &&
                    offsets_[static_cast<size_t>(physical_index)] != 0) {
                    ++valid_feature_count_;
                    if (materialize_valid_fids)
                        valid_row_slots_.push_back(first_row + local);
                }
            }
        }
    } catch (const std::exception&) {
        return fail();
    }

    return source_is_current_unlocked() ? true : fail();
}

// ── 变长偏移读取 ──
// 根据 size_bytes（4/5/6）以不同方式组装小端整数
uint64_t GdbTablxParser::read_offset(const uint8_t* data, int size_bytes) const {
    switch (size_bytes) {
        case 4: {
            // 标准 4 字节小端 uint32
            uint32_t v = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
            return v;
        }
        case 5: {
            // 5 字节编码: byte[0]=低 8 位, bytes[1:5]=高 32 位
            uint64_t v_low = data[0];
            uint32_t v_high = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
            return v_low | (static_cast<uint64_t>(v_high) << 8);
        }
        case 6: {
            // 6 字节编码: byte[0]=低 8 位, bytes[1:5]=中 32 位, byte[5]=高 8 位
            uint64_t v_low = data[0];
            uint32_t v_mid = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
            uint64_t v_high = data[5];
            return v_low | (v_mid << 8) | (v_high << 40);
        }
        default:
            return 0;  // 未知宽度返回 0
    }
}

// ── FID → 偏移查找 ──
uint64_t GdbTablxParser::get_offset(RowSlot fid) const {
    if (fid >= logical_row_count_) return 0;
    uint64_t physical_block = fid / 1024U;
    if (!block_bitmap_.empty()) {
        if (physical_block >= block_bitmap_.size() ||
            !block_bitmap_[static_cast<size_t>(physical_block)]) {
            return 0;
        }
        physical_block = block_rank_prefix_[static_cast<size_t>(physical_block)];
    }
    const uint64_t physical_index = physical_block * 1024U + (fid % 1024U);
    if (physical_index < offsets_.size())
        return offsets_[static_cast<size_t>(physical_index)];
    return 0;  // FID 超出范围，视为不存在
}

// ── 块活跃性检查 ──
bool GdbTablxParser::is_block_active(uint64_t block_index) const {
    if (block_index >= (logical_row_count_ + 1023U) / 1024U) return false;
    if (block_bitmap_.empty()) return true;  // 无位图 → 所有逻辑块均存在
    if (block_index < block_bitmap_.size()) return block_bitmap_[block_index];
    return false;  // 超出位图范围 → 视为非活跃
}

} // namespace explorgdb
