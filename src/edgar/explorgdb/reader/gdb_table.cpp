// src/edgar/explorgdb/gdb_table.cpp
// .gdbtable binary parser implementation.

#include "gdb_table.h"
#include "binary_reader.h"
#include "field_layout.h"
#include "gdb_tablx.h"
#include "gdb_tablx_cache.h"

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace explorgdb {
namespace {

bool read_fixed_field_value(BinaryReader& reader,
                            FieldType type,
                            uint32_t implicit_object_id,
                            FieldValue& value) {
    if (type == FieldType::ObjectId) {
        value = static_cast<int32_t>(implicit_object_id);
        return true;
    }

    const size_t width = fixed_physical_width(type);
    if (width == 0 || !reader.can_read(width)) return false;

    switch (type) {
        case FieldType::Int16:
            value = static_cast<int32_t>(reader.read_i16());
            return true;
        case FieldType::Int32:
            value = reader.read_i32();
            return true;
        case FieldType::Int64:
            value = reader.read_i64();
            return true;
        case FieldType::Float32:
            value = static_cast<double>(reader.read_f32());
            return true;
        case FieldType::Float64:
        case FieldType::DateTime:
        case FieldType::Date:
        case FieldType::Time:
            value = reader.read_f64();
            return true;
        case FieldType::DateTimeWithOffset: {
            const double date = reader.read_f64();
            (void)reader.read_i16();
            value = date;
            return true;
        }
        case FieldType::UUID_1:
        case FieldType::UUID_2: {
            const auto bytes = reader.read_bytes(width);
            char uuid[33];
            for (size_t i = 0; i < bytes.size(); ++i)
                std::snprintf(uuid + i * 2, 3, "%02x", bytes[i]);
            uuid[32] = '\0';
            value = std::string(uuid, 32);
            return true;
        }
        default:
            return false;
    }
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
    if (width == 0 || cursor + width > end) return false;

    ref.data = cursor;
    // Preserve the existing FieldRef contract: DateTimeWithOffset exposes the
    // date double while consuming the complete 10-byte physical value.
    ref.byte_len = type == FieldType::DateTimeWithOffset ? sizeof(double) : width;
    cursor += width;
    return true;
}

bool read_varuint_inline(const uint8_t*& cursor,
                         const uint8_t* end,
                         uint64_t& value) {
    value = 0;
    int shift = 0;
    while (cursor < end && shift <= 63) {
        const uint8_t byte = *cursor++;
        value |= static_cast<uint64_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) return true;
        shift += 7;
    }
    return false;
}

size_t nullable_bitmap_bytes_for(int nullable_count) {
    return static_cast<size_t>((nullable_count + 7) / 8);
}

bool nullable_bit_is_set(const std::vector<uint8_t>& bitmap, int nullable_bit) {
    const int byte_index = nullable_bit / 8;
    const int bit_index = nullable_bit % 8;
    return byte_index < static_cast<int>(bitmap.size()) &&
           ((bitmap[byte_index] >> bit_index) & 1U) != 0;
}

bool is_zero_padding(const uint8_t* cursor, const uint8_t* end) {
    while (cursor < end) {
        if (*cursor++ != 0) return false;
    }
    return true;
}

} // namespace

GdbTableParser::GdbTableParser(const std::string& file_path)
    : file_path_(file_path) {}

GdbTableParser::~GdbTableParser() {
    close_file();
}

bool GdbTableParser::open() {
    if (fd_ >= 0 || mapped_data_) close_file();

    fd_ = ::open(file_path_.c_str(), O_RDONLY);
    if (fd_ < 0) {
        std::cerr << "Failed to open: " << file_path_ << "\n";
        return false;
    }

    struct stat st;
    if (fstat(fd_, &st) < 0) {
        std::cerr << "Failed to stat: " << file_path_ << "\n";
        close_file();
        return false;
    }
    file_size_ = static_cast<size_t>(st.st_size);

    if (file_size_ > 0) {
        void* ptr = mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (ptr != MAP_FAILED) {
            mapped_data_ = static_cast<uint8_t*>(ptr);
            madvise(mapped_data_, file_size_, MADV_SEQUENTIAL);
        }
    }

    uint8_t header_buffer[48];
    const size_t header_size = std::min(file_size_, sizeof(header_buffer));
    if (!read_at(0, header_buffer, header_size)) {
        close_file();
        return false;
    }

    try {
        BinaryReader reader(header_buffer, header_size);
        header_.version = reader.read_u32();
        if (header_.version == 3) {
            header_.nfeatures_v3 = reader.read_u32();
            header_.largest_size_record = reader.read_u32();
            header_.unknown_role = reader.read_u32();
            header_.unknown_16 = reader.read_u32();
            header_.unknown_20 = reader.read_u32();
            header_.file_size = reader.read_u64();
            header_.field_desc_offset = reader.read_u64();
        } else if (header_.version == 4) {
            header_.has_deleted_features = reader.read_u32();
            header_.largest_size_record = reader.read_u32();
            header_.unknown_role = reader.read_u32();
            reader.skip(4);
            header_.nfeatures_v4 = reader.read_u64();
            header_.file_size = reader.read_u64();
            header_.field_desc_offset = reader.read_u64();
        } else {
            std::cerr << "Unknown .gdbtable version: " << header_.version << "\n";
            close_file();
            return false;
        }
    } catch (const std::exception& error) {
        std::cerr << "Failed to parse table header: " << error.what() << "\n";
        close_file();
        return false;
    }

    return ensure_fields_loaded();
}

bool GdbTableParser::ensure_fields_loaded() {
    if (!fields_.empty()) return true;
    if (fd_ < 0 || header_.field_desc_offset >= file_size_) return false;

    uint8_t section_header[14];
    if (!read_at(header_.field_desc_offset, section_header, sizeof(section_header)))
        return false;

    try {
        BinaryReader section_reader(section_header, sizeof(section_header));
        const uint32_t section_length = section_reader.read_u32();
        (void)section_reader.read_u32();
        header_.geom_type_full = section_reader.read_u32();

        const bool layer_has_z = ((header_.geom_type_full >> 24U) & (1U << 7U)) != 0;
        const bool layer_has_m = ((header_.geom_type_full >> 24U) & (1U << 6U)) != 0;
        const size_t section_size = static_cast<size_t>(section_length) + 4;
        if (header_.field_desc_offset + section_size > file_size_) return false;

        std::vector<uint8_t> buffer(section_size);
        if (!read_at(header_.field_desc_offset, buffer.data(), buffer.size())) return false;

        BinaryReader reader(buffer);
        reader.skip(12);
        const uint16_t field_count = reader.read_u16();
        fields_.clear();
        fields_.reserve(field_count);
        for (uint16_t i = 0; i < field_count; ++i) {
            if (!reader.can_read(4)) return false;
            parse_field_descriptor(reader, layer_has_z, layer_has_m);
        }
    } catch (const std::exception& error) {
        std::cerr << "Failed to parse fields: " << error.what() << "\n";
        fields_.clear();
        return false;
    }

    geometry_field_index_ = -1;
    geometry_nullable_bit_index_ = -1;
    int nullable_bit = 0;
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i].type == FieldType::Geometry) {
            geometry_field_index_ = static_cast<int>(i);
            geometry_nullable_bit_index_ = nullable_bit;
            break;
        }
        if ((fields_[i].flag & 1U) != 0) ++nullable_bit;
    }
    return true;
}

void GdbTableParser::close_file() {
    if (mapped_data_) {
        munmap(mapped_data_, file_size_);
        mapped_data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    file_size_ = 0;
}

bool GdbTableParser::read_at(uint64_t offset, void* buffer, size_t size) const {
    if (offset > file_size_ || size > file_size_ - static_cast<size_t>(offset)) return false;
    if (mapped_data_) {
        std::memcpy(buffer, mapped_data_ + offset, size);
        return true;
    }
    if (fd_ < 0) return false;
    return pread(fd_, buffer, size, static_cast<off_t>(offset)) ==
           static_cast<ssize_t>(size);
}

bool GdbTableParser::load_file() {
    std::ifstream stream(file_path_, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) return false;
    const auto end = stream.tellg();
    if (end < 0) return false;
    file_data_.resize(static_cast<size_t>(end));
    stream.seekg(0, std::ios::beg);
    return static_cast<bool>(stream.read(
        reinterpret_cast<char*>(file_data_.data()),
        static_cast<std::streamsize>(file_data_.size())));
}

bool GdbTableParser::load_tablx(const std::string& tablx_path) {
    // 尝试从缓存获取（除非被 FAST_GDB_TABLX_CACHE=0 绕过）
    if (!TablxCache::is_bypassed()) {
        TablxCacheKey key;
        if (TablxCache::make_key(tablx_path, key)) {
            auto cached = TablxCache::instance().get(key);
            if (cached) {
                feature_offsets_ = cached->offsets;
                active_feature_count_ = cached->feature_count;
                active_feature_count_known_ = true;
                return true;
            }
        }
    }

    // 缓存未命中或绕过：完整解析
    GdbTablxParser parser(tablx_path);
    if (!parser.parse()) return false;
    feature_offsets_ = parser.offsets();
    active_feature_count_ = parser.feature_count();
    active_feature_count_known_ = true;

    // 将解析结果存入缓存
    if (!TablxCache::is_bypassed()) {
        TablxCacheKey key;
        if (TablxCache::make_key(tablx_path, key)) {
            TablxCache::instance().put(
                key, feature_offsets_, active_feature_count_);
        }
    }

    return true;
}

bool GdbTableParser::parse_header() {
    if (file_data_.empty() && !load_file()) return false;
    try {
        BinaryReader reader(file_data_);
        header_.version = reader.read_u32();
        if (header_.version == 3) {
            header_.nfeatures_v3 = reader.read_u32();
            header_.largest_size_record = reader.read_u32();
            header_.unknown_role = reader.read_u32();
            header_.unknown_16 = reader.read_u32();
            header_.unknown_20 = reader.read_u32();
            header_.file_size = reader.read_u64();
            header_.field_desc_offset = reader.read_u64();
        } else if (header_.version == 4) {
            header_.has_deleted_features = reader.read_u32();
            header_.largest_size_record = reader.read_u32();
            header_.unknown_role = reader.read_u32();
            reader.skip(4);
            header_.nfeatures_v4 = reader.read_u64();
            header_.file_size = reader.read_u64();
            header_.field_desc_offset = reader.read_u64();
        } else {
            return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

int GdbTableParser::nullable_field_count() const {
    return static_cast<int>(std::count_if(
        fields_.begin(), fields_.end(),
        [](const FieldDescriptor& field) { return (field.flag & 1U) != 0; }));
}

bool GdbTableParser::parse_fields() {
    if (header_.version == 0 && !parse_header()) return false;
    if (file_data_.empty() || header_.field_desc_offset >= file_data_.size()) return false;

    try {
        BinaryReader reader(file_data_);
        reader.seek(static_cast<size_t>(header_.field_desc_offset));
        (void)reader.read_u32();
        (void)reader.read_u32();
        header_.geom_type_full = reader.read_u32();
        const bool layer_has_z = ((header_.geom_type_full >> 24U) & (1U << 7U)) != 0;
        const bool layer_has_m = ((header_.geom_type_full >> 24U) & (1U << 6U)) != 0;
        const uint16_t count = reader.read_u16();

        fields_.clear();
        fields_.reserve(count);
        for (uint16_t i = 0; i < count; ++i)
            parse_field_descriptor(reader, layer_has_z, layer_has_m);
    } catch (const std::exception&) {
        fields_.clear();
        return false;
    }

    geometry_field_index_ = -1;
    geometry_nullable_bit_index_ = -1;
    int nullable_bit = 0;
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i].type == FieldType::Geometry) {
            geometry_field_index_ = static_cast<int>(i);
            geometry_nullable_bit_index_ = nullable_bit;
            break;
        }
        if ((fields_[i].flag & 1U) != 0) ++nullable_bit;
    }
    return true;
}

void GdbTableParser::parse_field_descriptor(BinaryReader& reader,
                                             bool layer_has_z,
                                             bool layer_has_m) {
    FieldDescriptor field;
    field.name = reader.read_utf16(reader.read_u8());
    field.alias = reader.read_utf16(reader.read_u8());
    field.type = static_cast<FieldType>(reader.read_u8());

    switch (field.type) {
        case FieldType::Int16:
        case FieldType::Int32:
        case FieldType::Float32:
        case FieldType::Float64:
        case FieldType::DateTime:
        case FieldType::Date:
        case FieldType::Time:
        case FieldType::DateTimeWithOffset:
        case FieldType::Int64: {
            field.width = reader.read_u8();
            field.flag = reader.read_u8();
            const uint8_t default_length = reader.read_u8();
            if (default_length > 0 && (field.flag & 4U) != 0)
                reader.skip(default_length);
            break;
        }
        case FieldType::String: {
            field.width = static_cast<uint8_t>(std::min<uint32_t>(reader.read_u32(), 255));
            field.flag = reader.read_u8();
            const uint64_t default_length = reader.read_varuint();
            if (default_length > 0 && (field.flag & 4U) != 0)
                reader.skip(static_cast<size_t>(default_length));
            break;
        }
        case FieldType::ObjectId:
            field.width = reader.read_u8();
            field.flag = reader.read_u8();
            break;
        case FieldType::Geometry: {
            (void)reader.read_u8();
            field.flag = reader.read_u8();
            const uint16_t wkt_bytes = reader.read_u16();
            field.wkt = reader.read_utf16(wkt_bytes / 2);
            const uint8_t flags = reader.read_u8();
            const bool has_m = (flags & 2U) != 0;
            const bool has_z = (flags & 4U) != 0;
            field.xorig = reader.read_f64();
            field.yorig = reader.read_f64();
            field.xyscale = reader.read_f64();
            if (has_m) {
                field.morig = reader.read_f64();
                field.mscale = reader.read_f64();
            }
            if (has_z) {
                field.zorig = reader.read_f64();
                field.zscale = reader.read_f64();
            }
            field.xytolerance = reader.read_f64();
            if (has_m) field.mtolerance = reader.read_f64();
            if (has_z) field.ztolerance = reader.read_f64();
            field.xmin = reader.read_f64();
            field.ymin = reader.read_f64();
            field.xmax = reader.read_f64();
            field.ymax = reader.read_f64();
            if (layer_has_z) {
                field.zmin = reader.read_f64();
                field.zmax = reader.read_f64();
            }
            if (layer_has_m) {
                field.mmin = reader.read_f64();
                field.mmax = reader.read_f64();
            }
            (void)reader.read_u8();
            const uint32_t grids = reader.read_u32();
            field.grid_sizes.resize(grids);
            for (double& grid : field.grid_sizes) grid = reader.read_f64();
            break;
        }
        case FieldType::Binary:
        case FieldType::Raster:
            field.flag = reader.read_u8();
            if (field.type == FieldType::Raster) {
                (void)reader.read_utf16(reader.read_u8());
                const uint16_t wkt_bytes = reader.read_u16();
                (void)reader.read_utf16(wkt_bytes / 2);
                (void)reader.read_u8();
                (void)reader.read_u8();
            }
            break;
        case FieldType::UUID_1:
        case FieldType::UUID_2:
        case FieldType::XML:
            field.width = reader.read_u8();
            field.flag = reader.read_u8();
            break;
        default:
            throw std::runtime_error("unknown FileGDB field type");
    }

    fields_.push_back(std::move(field));
}

bool GdbTableParser::parse_records() {
    if (fields_.empty() && !parse_fields()) return false;
    if (feature_offsets_.empty()) return false;

    records_.clear();
    for (uint32_t fid = 0; fid < feature_offsets_.size(); ++fid) {
        const uint64_t offset = feature_offsets_[fid];
        if (offset == 0 || offset >= file_data_.size()) continue;
        FeatureRecord record;
        record.fid = fid;
        parse_record_at_offset(static_cast<size_t>(offset), record);
        records_.push_back(std::move(record));
    }
    return true;
}

bool GdbTableParser::read_record_by_fid(uint32_t fid, FeatureRecord& record) {
    if (fd_ < 0 && file_data_.empty() && !open()) return false;
    if (fields_.empty() && fd_ >= 0 && !ensure_fields_loaded()) return false;
    if (feature_offsets_.empty() || fid >= feature_offsets_.size()) return false;

    const uint64_t offset = feature_offsets_[fid];
    if (offset == 0) return false;

    if (fd_ < 0) {
        if (offset >= file_data_.size()) return false;
        record = FeatureRecord{};
        record.fid = fid;
        try {
            parse_record_at_offset(static_cast<size_t>(offset), record);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    if (offset + 4 > file_size_) return false;
    uint8_t length_buffer[4];
    if (!read_at(offset, length_buffer, sizeof(length_buffer))) return false;
    BinaryReader length_reader(length_buffer, sizeof(length_buffer));
    const uint32_t blob_length = length_reader.read_u32();
    if (offset + 4 + blob_length > file_size_) return false;

    record = FeatureRecord{};
    record.fid = fid;
    record.blob_len = blob_length;
    if (blob_length == 0) return true;

    if (row_buffer_.size() < blob_length) row_buffer_.resize(blob_length);
    if (!read_at(offset + 4, row_buffer_.data(), blob_length)) return false;

    try {
        if (parse_record_payload(row_buffer_.data(), blob_length, fid, record))
            return true;
    } catch (const std::exception& error) {
        std::cerr << "FID " << fid << " @offset " << offset
                  << ": parse error: " << error.what() << "\n";
    }
    return false;
}

void GdbTableParser::parse_record_at_offset(size_t offset, FeatureRecord& record) {
    BinaryReader reader(file_data_.data() + offset, file_data_.size() - offset);
    record.blob_len = reader.read_u32();
    if (record.blob_len > file_data_.size() - offset - 4)
        throw std::out_of_range("record outside file");
    if (!parse_record_payload(file_data_.data() + offset + 4,
                              record.blob_len,
                              record.fid,
                              record))
        throw std::runtime_error("record payload could not be parsed");
}

bool GdbTableParser::parse_record_payload(const uint8_t* row_data,
                                          size_t row_size,
                                          uint32_t fid,
                                          FeatureRecord& record) {
    if (row_size == 0) return true;

    const int nullable_count = nullable_field_count();
    const size_t max_bitmap_bytes = nullable_bitmap_bytes_for(nullable_count);
    FeatureRecord best_record;
    size_t best_padding = row_size + 1;
    bool found = false;

    for (size_t bitmap_bytes = max_bitmap_bytes;; --bitmap_bytes) {
        if (bitmap_bytes > row_size) {
            if (bitmap_bytes == 0) break;
            continue;
        }

        const int max_present_bits =
            std::min(nullable_count, static_cast<int>(bitmap_bytes * 8));
        for (int present_bits = max_present_bits; present_bits >= 0; --present_bits) {
            FeatureRecord candidate;
            candidate.fid = fid;
            candidate.blob_len = static_cast<uint32_t>(row_size);

            try {
                BinaryReader reader(row_data, row_size);
                if (bitmap_bytes > 0)
                    candidate.nullable_flags = reader.read_bytes(bitmap_bytes);

                candidate.field_values.reserve(fields_.size());
                int nullable_bit = 0;
                bool valid = true;
                for (const auto& field : fields_) {
                    bool is_null = false;
                    if ((field.flag & 1U) != 0) {
                        is_null = nullable_bit >= present_bits ||
                                  nullable_bit_is_set(candidate.nullable_flags, nullable_bit);
                        ++nullable_bit;
                    }
                    if (is_null) {
                        candidate.field_values.push_back(nullptr);
                        continue;
                    }

                    if (field.type == FieldType::ObjectId ||
                        fixed_physical_width(field.type) != 0) {
                        FieldValue value;
                        if (!read_fixed_field_value(reader, field.type, fid + 1, value)) {
                            valid = false;
                            break;
                        }
                        candidate.field_values.push_back(std::move(value));
                        continue;
                    }

                    switch (field.type) {
                        case FieldType::String:
                        case FieldType::XML: {
                            const uint64_t length = reader.read_varuint();
                            if (!reader.can_read(static_cast<size_t>(length))) {
                                valid = false;
                                break;
                            }
                            std::string text(static_cast<size_t>(length), '\0');
                            for (char& c : text) c = static_cast<char>(reader.read_u8());
                            candidate.field_values.push_back(std::move(text));
                            break;
                        }
                        case FieldType::Binary: {
                            const uint64_t length = reader.read_varuint();
                            if (!reader.can_read(static_cast<size_t>(length))) {
                                valid = false;
                                break;
                            }
                            candidate.field_values.push_back(
                                reader.read_bytes(static_cast<size_t>(length)));
                            break;
                        }
                        case FieldType::Raster: {
                            const uint64_t length = reader.read_varuint();
                            if (!reader.can_read(static_cast<size_t>(length))) {
                                valid = false;
                                break;
                            }
                            reader.skip(static_cast<size_t>(length));
                            candidate.field_values.push_back(nullptr);
                            break;
                        }
                        case FieldType::Geometry: {
                            const uint64_t length = reader.read_varuint();
                            if (!reader.can_read(static_cast<size_t>(length))) {
                                valid = false;
                                break;
                            }
                            const size_t blob_offset = reader.tell();
                            if (length == 0) {
                                candidate.field_values.push_back("POINT EMPTY");
                            } else {
                                try {
                                    const auto geometry = make_geom_decoder(field).decode(
                                        row_data + blob_offset,
                                        static_cast<size_t>(length));
                                    candidate.field_values.push_back(geometry.wkt);
                                } catch (const std::exception&) {
                                    candidate.field_values.push_back("<geom decode error>");
                                }
                            }
                            reader.skip(static_cast<size_t>(length));
                            break;
                        }
                        default:
                            valid = false;
                            break;
                    }
                    if (!valid) break;
                }

                if (valid && reader.tell() <= row_size) {
                    const size_t padding = row_size - reader.tell();
                    if (is_zero_padding(row_data + reader.tell(), row_data + row_size) &&
                        padding < best_padding) {
                        best_padding = padding;
                        best_record = std::move(candidate);
                        found = true;
                        if (padding == 0) break;
                    }
                }
            } catch (const std::exception&) {
            }
        }
        if (found && best_padding == 0) break;

        if (bitmap_bytes == 0) break;
    }

    if (!found) return false;
    record = std::move(best_record);
    return true;
}

GdbGeomDecoder GdbTableParser::make_geom_decoder(const FieldDescriptor& field) const {
    const bool has_z = ((header_.geom_type_full >> 24U) & (1U << 7U)) != 0;
    const bool has_m = ((header_.geom_type_full >> 24U) & (1U << 6U)) != 0;
    return GdbGeomDecoder(
        field.xorig, field.yorig, field.xyscale,
        field.zorig, field.zscale,
        field.morig, field.mscale,
        has_z, has_m);
}

uint64_t GdbTableParser::sequential_scan(ScanCallback callback) {
    if (fields_.empty() || feature_offsets_.empty() || !callback)
        return 0;

    const int field_count = static_cast<int>(fields_.size());
    const int nullable_count = nullable_field_count();
    const size_t max_bitmap_bytes = nullable_bitmap_bytes_for(nullable_count);
    std::vector<FieldRef> refs(static_cast<size_t>(field_count));
    uint64_t scanned = 0;

    for (uint32_t fid = 0; fid < feature_offsets_.size(); ++fid) {
        const uint64_t offset = feature_offsets_[fid];
        if (offset == 0 || offset >= file_size_) continue;

        // Read blob length (4 bytes at offset)
        uint32_t blob_length = 0;
        const uint8_t* cursor = nullptr;
        const uint8_t* record_end = nullptr;

        if (mapped_data_ != nullptr) {
            // mmap path: direct pointer into mapped region
            cursor = mapped_data_ + offset;
            const uint8_t* file_end = mapped_data_ + file_size_;
            if (cursor + 4 > file_end) break;
            std::memcpy(&blob_length, cursor, sizeof(blob_length));
            cursor += 4;
            if (cursor + blob_length > file_end) break;
            record_end = cursor + blob_length;
        } else if (fd_ >= 0) {
            // fd path: read blob into row_buffer_
            uint8_t len_buf[4];
            if (!read_at(offset, len_buf, sizeof(len_buf))) break;
            std::memcpy(&blob_length, len_buf, sizeof(blob_length));
            if (offset + 4 + blob_length > file_size_) break;
            if (row_buffer_.size() < blob_length) row_buffer_.resize(blob_length);
            if (!read_at(offset + 4, row_buffer_.data(), blob_length)) break;
            cursor = row_buffer_.data();
            record_end = cursor + blob_length;
        } else {
            break;
        }

        if (blob_length == 0) {
            for (int i = 0; i < field_count; ++i)
                refs[static_cast<size_t>(i)] =
                    FieldRef{fields_[static_cast<size_t>(i)].type, nullptr, 0, true, 0};
            if (!callback(fid, refs.data(), field_count)) break;
            ++scanned;
            continue;
        }

        bool valid = true;
        bool accepted = false;
        size_t best_padding = static_cast<size_t>(record_end - cursor) + 1;
        std::vector<FieldRef> best_refs(refs.size());
        for (size_t bitmap_bytes = max_bitmap_bytes;; --bitmap_bytes) {
            if (cursor + bitmap_bytes > record_end) {
                if (bitmap_bytes == 0) break;
                continue;
            }

            const int max_present_bits =
                std::min(nullable_count, static_cast<int>(bitmap_bytes * 8));
            for (int present_bits = max_present_bits; present_bits >= 0; --present_bits) {
                const uint8_t* field_cursor = cursor + bitmap_bytes;
                const uint8_t* nullable_bitmap = bitmap_bytes > 0 ? cursor : nullptr;
                int nullable_bit = 0;
                valid = true;
                for (int i = 0; i < field_count; ++i) {
                    const auto& field = fields_[static_cast<size_t>(i)];
                    auto& ref = refs[static_cast<size_t>(i)];
                    ref = FieldRef{};
                    ref.type = field.type;
                    ref.implicit_value = static_cast<int32_t>(fid + 1);

                    if ((field.flag & 1U) != 0) {
                        const int byte_index = nullable_bit / 8;
                        const int bit_index = nullable_bit % 8;
                        ref.is_null =
                            nullable_bit >= present_bits ||
                            (nullable_bitmap &&
                             ((nullable_bitmap[byte_index] >> bit_index) & 1U) != 0);
                        ++nullable_bit;
                    }
                    if (ref.is_null) continue;

                    if (field.type == FieldType::ObjectId ||
                        fixed_physical_width(field.type) != 0) {
                        valid = set_fixed_field_ref(field_cursor, record_end, field.type, ref);
                        if (!valid) break;
                        continue;
                    }

                    switch (field.type) {
                        case FieldType::String:
                        case FieldType::XML:
                        case FieldType::Binary:
                        case FieldType::Geometry:
                        case FieldType::Raster: {
                            uint64_t length = 0;
                            if (!read_varuint_inline(field_cursor, record_end, length) ||
                                length > static_cast<uint64_t>(record_end - field_cursor)) {
                                valid = false;
                                break;
                            }
                            if (field.type == FieldType::Raster) {
                                ref.is_null = true;
                                ref.data = nullptr;
                                ref.byte_len = 0;
                            } else {
                                ref.data = field_cursor;
                                ref.byte_len = static_cast<size_t>(length);
                            }
                            field_cursor += static_cast<size_t>(length);
                            break;
                        }
                        default:
                            valid = false;
                            break;
                    }
                    if (!valid) break;
                }
                if (valid) {
                    const size_t padding = static_cast<size_t>(record_end - field_cursor);
                    if (is_zero_padding(field_cursor, record_end) && padding < best_padding) {
                        best_padding = padding;
                        best_refs = refs;
                        accepted = true;
                        if (padding == 0) break;
                    }
                }
            }
            if (accepted && best_padding == 0) break;
            valid = false;
            if (bitmap_bytes == 0) break;
        }

        if (accepted) {
            refs = best_refs;
        } else {
            for (auto& ref : refs) {
                if (!ref.is_null && ref.data == nullptr && ref.type != FieldType::ObjectId)
                    ref.is_null = true;
            }
        }

        if (!callback(fid, refs.data(), field_count)) break;
        ++scanned;
    }

    return scanned;
}

} // namespace explorgdb
