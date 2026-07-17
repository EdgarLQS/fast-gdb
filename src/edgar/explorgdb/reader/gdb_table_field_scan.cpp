#include "gdb_table.h"
#include "field_layout.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace explorgdb {
namespace {

struct CandidateRow {
    uint32_t fid = 0;
    uint64_t offset = 0;
};

bool read_varuint_checked(const uint8_t*& cursor,
                          const uint8_t* end,
                          uint64_t& value) {
    value = 0;
    unsigned shift = 0;
    while (cursor < end && shift <= 63U) {
        const uint8_t byte = *cursor++;
        value |= static_cast<uint64_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) return true;
        shift += 7U;
    }
    return false;
}

bool set_fixed_field_ref(const uint8_t*& cursor,
                         const uint8_t* end,
                         FieldType type,
                         FieldRef& ref) {
    if (type == FieldType::ObjectId) {
        ref.data = nullptr;
        ref.byte_len = 0;
        return true;
    }
    const size_t width = fixed_physical_width(type);
    if (width == 0 || width > static_cast<size_t>(end - cursor)) return false;
    ref.data = cursor;
    ref.byte_len = width;
    cursor += width;
    return true;
}

bool all_zero(const uint8_t* cursor, const uint8_t* end) {
    while (cursor < end) {
        if (*cursor++ != 0) return false;
    }
    return true;
}

size_t bitmap_bytes_for(int nullable_count) {
    return static_cast<size_t>((nullable_count + 7) / 8);
}

bool parse_row_refs(const std::vector<FieldDescriptor>& fields,
                    int nullable_count,
                    uint32_t fid,
                    const uint8_t* row_begin,
                    size_t row_size,
                    std::vector<FieldRef>& output,
                    std::vector<FieldRef>& scratch) {
    const int field_count = static_cast<int>(fields.size());
    if (output.size() != fields.size()) output.resize(fields.size());
    if (scratch.size() != fields.size()) scratch.resize(fields.size());

    if (row_size == 0) {
        for (int index = 0; index < field_count; ++index) {
            output[static_cast<size_t>(index)] = FieldRef{
                fields[static_cast<size_t>(index)].type,
                nullptr,
                0,
                true,
                static_cast<int32_t>(fid + 1)};
        }
        return true;
    }
    if (row_begin == nullptr) return false;

    const uint8_t* row_end = row_begin + row_size;
    const size_t max_bitmap_bytes = bitmap_bytes_for(nullable_count);
    size_t best_padding = row_size + 1U;
    bool accepted = false;

    for (size_t bitmap_bytes = max_bitmap_bytes;; --bitmap_bytes) {
        if (bitmap_bytes <= row_size) {
            const int max_present_bits = std::min(
                nullable_count, static_cast<int>(bitmap_bytes * 8U));
            for (int present_bits = max_present_bits;
                 present_bits >= 0;
                 --present_bits) {
                const uint8_t* cursor = row_begin + bitmap_bytes;
                const uint8_t* bitmap =
                    bitmap_bytes == 0 ? nullptr : row_begin;
                int nullable_bit = 0;
                bool valid = true;

                for (int index = 0; index < field_count; ++index) {
                    const FieldDescriptor& field =
                        fields[static_cast<size_t>(index)];
                    FieldRef& ref = scratch[static_cast<size_t>(index)];
                    ref = FieldRef{};
                    ref.type = field.type;
                    ref.implicit_value = static_cast<int32_t>(fid + 1);

                    if ((field.flag & 1U) != 0) {
                        const size_t byte_index =
                            static_cast<size_t>(nullable_bit / 8);
                        const unsigned bit_index =
                            static_cast<unsigned>(nullable_bit % 8);
                        ref.is_null = nullable_bit >= present_bits ||
                            bitmap == nullptr || byte_index >= bitmap_bytes ||
                            ((bitmap[byte_index] >> bit_index) & 1U) != 0;
                        ++nullable_bit;
                    }
                    if (ref.is_null) continue;

                    if (field.type == FieldType::ObjectId ||
                        fixed_physical_width(field.type) != 0) {
                        valid = set_fixed_field_ref(
                            cursor, row_end, field.type, ref);
                        if (!valid) break;
                        continue;
                    }

                    switch (field.type) {
                        case FieldType::String:
                        case FieldType::XML:
                        case FieldType::Binary:
                        case FieldType::Geometry:
                        case FieldType::Raster: {
                            uint64_t encoded_size = 0;
                            if (!read_varuint_checked(
                                    cursor, row_end, encoded_size) ||
                                encoded_size > static_cast<uint64_t>(
                                    row_end - cursor) ||
                                encoded_size > static_cast<uint64_t>(
                                    std::numeric_limits<size_t>::max())) {
                                valid = false;
                                break;
                            }
                            const size_t value_size =
                                static_cast<size_t>(encoded_size);
                            if (field.type == FieldType::Raster) {
                                ref.is_null = true;
                            } else {
                                ref.data = cursor;
                                ref.byte_len = value_size;
                            }
                            cursor += value_size;
                            break;
                        }
                        default:
                            valid = false;
                            break;
                    }
                    if (!valid) break;
                }

                if (!valid) continue;
                const size_t padding = static_cast<size_t>(row_end - cursor);
                if (!all_zero(cursor, row_end) || padding >= best_padding)
                    continue;
                best_padding = padding;
                std::copy(scratch.begin(), scratch.end(), output.begin());
                accepted = true;
                if (padding == 0) break;
            }
        }
        if (accepted && best_padding == 0) break;
        if (bitmap_bytes == 0) break;
    }

    return accepted;
}

} // namespace

uint64_t GdbTableParser::scan_field_candidates(
    const std::vector<uint32_t>& candidates,
    ScanCallback callback) {
    if (!callback || candidates.empty() || fields_.empty() ||
        feature_offsets_.empty()) {
        return 0;
    }

    const uint64_t file_size64 = static_cast<uint64_t>(file_size_);
    const int field_count = static_cast<int>(fields_.size());
    const int nullable_count = nullable_field_count();
    std::vector<FieldRef> refs(fields_.size());
    std::vector<FieldRef> scratch(fields_.size());
    std::vector<uint8_t> row_bytes;
    uint64_t scanned = 0;

    auto emit_candidate = [&](uint32_t fid,
                              const uint8_t* row_begin,
                              size_t row_size) -> bool {
        if (!parse_row_refs(fields_, nullable_count, fid,
                            row_begin, row_size, refs, scratch)) {
            return false;
        }
        if (!callback(fid, refs.data(), field_count)) return false;
        ++scanned;
        return true;
    };

    bool physical_offsets_monotonic = true;
    uint64_t previous_offset = 0;
    bool have_previous_offset = false;
    for (uint32_t fid : candidates) {
        if (fid >= feature_offsets_.size()) return 0;
        const uint64_t offset = feature_offsets_[fid];
        if (offset == 0 || offset > file_size64 ||
            file_size64 - offset < 4U) {
            return 0;
        }
        if (have_previous_offset && offset < previous_offset)
            physical_offsets_monotonic = false;
        previous_offset = offset;
        have_previous_offset = true;
    }

    // The common Linux/macOS path has a full mmap and candidates already sorted
    // by FID. When their physical offsets are also monotonic, scanning them
    // directly avoids allocating and sorting a second candidate vector.
    if (mapped_data_ != nullptr && physical_offsets_monotonic) {
        for (uint32_t fid : candidates) {
            const uint64_t offset = feature_offsets_[fid];
            uint32_t row_size = 0;
            std::memcpy(&row_size, mapped_data_ + offset, sizeof(row_size));
            if (static_cast<uint64_t>(row_size) >
                file_size64 - offset - 4U) {
                return 0;
            }
            const uint8_t* row_begin = row_size == 0
                ? nullptr
                : mapped_data_ + offset + 4U;
            if (!emit_candidate(fid, row_begin, row_size)) break;
        }
        return scanned;
    }

    std::vector<CandidateRow> physical;
    physical.reserve(candidates.size());
    for (uint32_t fid : candidates)
        physical.push_back(CandidateRow{fid, feature_offsets_[fid]});
    std::sort(physical.begin(), physical.end(),
              [](const CandidateRow& left, const CandidateRow& right) {
                  if (left.offset != right.offset)
                      return left.offset < right.offset;
                  return left.fid < right.fid;
              });

    for (const CandidateRow& candidate : physical) {
        uint32_t row_size = 0;
        const uint8_t* row_begin = nullptr;

        if (mapped_data_ != nullptr) {
            std::memcpy(&row_size,
                        mapped_data_ + candidate.offset,
                        sizeof(row_size));
            if (static_cast<uint64_t>(row_size) >
                file_size64 - candidate.offset - 4U) {
                return 0;
            }
            row_begin = row_size == 0
                ? nullptr
                : mapped_data_ + candidate.offset + 4U;
        } else {
            uint8_t prefix[4];
            if (!read_at(candidate.offset, prefix, sizeof(prefix))) return 0;
            std::memcpy(&row_size, prefix, sizeof(row_size));
            if (static_cast<uint64_t>(row_size) >
                file_size64 - candidate.offset - 4U) {
                return 0;
            }
            row_bytes.resize(row_size);
            if (row_size != 0 &&
                !read_at(candidate.offset + 4U,
                         row_bytes.data(), row_bytes.size())) {
                return 0;
            }
            row_begin = row_size == 0 ? nullptr : row_bytes.data();
        }

        if (!emit_candidate(candidate.fid, row_begin, row_size)) break;
    }
    return scanned;
}

} // namespace explorgdb
