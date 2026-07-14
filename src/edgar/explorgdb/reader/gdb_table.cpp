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
    if (st.st_size < 0 ||
        static_cast<uint64_t>(st.st_size) >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        std::cerr << "Unsupported table size: " << file_path_ << "\n";
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
#ifdef _WIN32
    // A sliding view must be unmapped and its mapping handle closed before the
    // CRT descriptor releases the underlying file handle.
    sliding_map_.reset();
#endif
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

void GdbTableParser::parse_field_descriptor(BinaryReader& reader,
                                             bool layer_has_z,
                                             bool layer_has_m) {
    FieldDescriptor field;
    field.name = reader.read_utf16_string();
    field.alias = reader.read_utf16_string();
    field.type = static_cast<FieldType>(reader.read_u8());

    if (field.type == FieldType::Geometry) {
        parse_geometry_field(reader.position_ref(), field);
        field.has_z = layer_has_z;
        field.has_m = layer_has_m;
    } else {
        field.size = reader.read_u32();
        field.flag = reader.read_u8();
        if (field.type == FieldType::String) {
            field.max_length = reader.read_u32();
        }
    }

    fields_.push_back(std::move(field));
}

void GdbTableParser::parse_geometry_field(size_t& offset,
                                          FieldDescriptor& field) {
    BinaryReader reader(file_data_.data() + offset, file_data_.size() - offset);
    field.srs_wkt = reader.read_utf16_string();
    field.x_origin = reader.read_f64();
    field.y_origin = reader.read_f64();
    field.xy_scale = reader.read_f64();
    field.z_origin = reader.read_f64();
    field.z_scale = reader.read_f64();
    field.m_origin = reader.read_f64();
    field.m_scale = reader.read_f64();
    field.xy_tolerance = reader.read_f64();
    field.z_tolerance = reader.read_f64();
    field.m_tolerance = reader.read_f64();
    const uint32_t grid_count = reader.read_u32();
    field.grid_resolutions.clear();
    field.grid_resolutions.reserve(grid_count);
    for (uint32_t i = 0; i < grid_count; ++i)
        field.grid_resolutions.push_back(reader.read_f64());
    field.size = reader.read_u32();
    field.flag = reader.read_u8();
    offset += reader.tell();
}

bool GdbTableParser::parse_fields() {
    if (file_data_.empty() && !load_file()) return false;
    if (header_.field_desc_offset >= file_data_.size()) return false;

    try {
        BinaryReader reader(file_data_.data() + header_.field_desc_offset,
                            file_data_.size() - header_.field_desc_offset);
        const uint32_t section_length = reader.read_u32();
        (void)section_length;
        (void)reader.read_u32();
        header_.geom_type_full = reader.read_u32();
        const bool layer_has_z =
            ((header_.geom_type_full >> 24U) & (1U << 7U)) != 0;
        const bool layer_has_m =
            ((header_.geom_type_full >> 24U) & (1U << 6U)) != 0;
        const uint16_t field_count = reader.read_u16();
        fields_.clear();
        fields_.reserve(field_count);
        for (uint16_t i = 0; i < field_count; ++i)
            parse_field_descriptor(reader, layer_has_z, layer_has_m);
        return true;
    } catch (const std::exception&) {
        fields_.clear();
        return false;
    }
}

bool GdbTableParser::parse_record_payload(const uint8_t* row_data,
                                          size_t row_size,
                                          uint32_t fid,
                                          FeatureRecord& record) {
    try {
        BinaryReader reader(row_data, row_size);
        const int nullable_count = nullable_field_count();
        const size_t bitmap_size = nullable_bitmap_bytes_for(nullable_count);
        const std::vector<uint8_t> nullable_bitmap =
            reader.read_bytes(bitmap_size);

        record.fid = fid;
        record.values.clear();
        record.values.reserve(fields_.size());
        int nullable_bit = 0;
        for (const auto& field : fields_) {
            const bool nullable = (field.flag & 1U) != 0;
            const bool is_null = nullable &&
                nullable_bit_is_set(nullable_bitmap, nullable_bit);
            if (nullable) ++nullable_bit;
            if (is_null) {
                record.values.emplace_back(std::monostate{});
                continue;
            }

            FieldValue value;
            if (read_fixed_field_value(reader, field.type, fid, value)) {
                record.values.push_back(std::move(value));
                continue;
            }

            switch (field.type) {
                case FieldType::String:
                case FieldType::XML: {
                    const uint64_t size = reader.read_varuint();
                    value = reader.read_utf8_string(static_cast<size_t>(size));
                    break;
                }
                case FieldType::Binary:
                case FieldType::Raster: {
                    const uint64_t size = reader.read_varuint();
                    value = reader.read_bytes(static_cast<size_t>(size));
                    break;
                }
                case FieldType::Geometry: {
                    const uint64_t size = reader.read_varuint();
                    const std::vector<uint8_t> blob =
                        reader.read_bytes(static_cast<size_t>(size));
                    const FieldDescriptor* geometry = geometry_field_descriptor();
                    if (geometry == nullptr) return false;
                    GdbGeomDecoder decoder = make_geom_decoder(*geometry);
                    value = decoder.decode(blob.data(), blob.size(), fid);
                    break;
                }
                default:
                    return false;
            }
            record.values.push_back(std::move(value));
        }
        return is_zero_padding(
            row_data + reader.tell(), row_data + row_size);
    } catch (const std::exception&) {
        return false;
    }
}

bool GdbTableParser::read_record_by_fid(uint32_t fid,
                                        FeatureRecord& record) {
    if (fid >= feature_offsets_.size()) return false;
    const uint64_t offset = feature_offsets_[fid];
    if (offset == 0 || offset + 4 > file_size_) return false;

    uint32_t row_size = 0;
    if (!read_at(offset, &row_size, sizeof(row_size))) return false;
    if (row_size > file_size_ - static_cast<size_t>(offset + 4)) return false;
    if (row_buffer_.size() < row_size) row_buffer_.resize(row_size);
    if (row_size != 0 &&
        !read_at(offset + 4, row_buffer_.data(), row_size)) {
        return false;
    }
    return parse_record_payload(row_buffer_.data(), row_size, fid, record);
}

void GdbTableParser::parse_record_at_offset(size_t offset,
                                            FeatureRecord& record) {
    BinaryReader reader(file_data_.data() + offset,
                        file_data_.size() - offset);
    const uint32_t row_size = reader.read_u32();
    if (!reader.can_read(row_size))
        throw std::runtime_error("record exceeds table buffer");
    if (!parse_record_payload(reader.data() + reader.tell(),
                              row_size,
                              static_cast<uint32_t>(records_.size()),
                              record)) {
        throw std::runtime_error("failed to parse record payload");
    }
}

bool GdbTableParser::parse_records() {
    if (file_data_.empty() && !load_file()) return false;
    records_.clear();
    for (uint32_t fid = 0; fid < feature_offsets_.size(); ++fid) {
        const uint64_t offset = feature_offsets_[fid];
        if (offset == 0 || offset >= file_data_.size()) continue;
        FeatureRecord record;
        try {
            parse_record_at_offset(static_cast<size_t>(offset), record);
            record.fid = fid;
            records_.push_back(std::move(record));
        } catch (const std::exception&) {
            return false;
        }
    }
    return true;
}

uint64_t GdbTableParser::sequential_scan(ScanCallback callback) {
    if (!callback || fields_.empty() || feature_offsets_.empty()) return 0;

    const int nullable_count = nullable_field_count();
    const size_t bitmap_size = nullable_bitmap_bytes_for(nullable_count);
    std::vector<FieldRef> refs(fields_.size());
    uint64_t scanned = 0;

    for (uint32_t fid = 0; fid < feature_offsets_.size(); ++fid) {
        const uint64_t offset = feature_offsets_[fid];
        if (offset == 0 || offset >= file_size_) continue;

        uint32_t row_size = 0;
        const uint8_t* row_begin = nullptr;
        const uint8_t* row_end = nullptr;
        if (mapped_data_ != nullptr) {
            if (offset + 4 > file_size_) return 0;
            std::memcpy(&row_size, mapped_data_ + offset, sizeof(row_size));
            row_begin = mapped_data_ + offset + 4;
            if (row_size > static_cast<size_t>(mapped_data_ + file_size_ - row_begin))
                return 0;
            row_end = row_begin + row_size;
        } else if (fd_ >= 0) {
            if (!read_at(offset, &row_size, sizeof(row_size))) return 0;
            if (offset + 4 > file_size_ ||
                row_size > file_size_ - static_cast<size_t>(offset + 4)) {
                return 0;
            }
            if (row_buffer_.size() < row_size) row_buffer_.resize(row_size);
            if (row_size != 0 &&
                !read_at(offset + 4, row_buffer_.data(), row_size)) {
                return 0;
            }
            row_begin = row_buffer_.data();
            row_end = row_begin + row_size;
        } else {
            return 0;
        }

        if (bitmap_size > static_cast<size_t>(row_end - row_begin)) return 0;
        const uint8_t* bitmap = bitmap_size == 0 ? nullptr : row_begin;
        const uint8_t* cursor = row_begin + bitmap_size;
        int nullable_bit = 0;
        bool valid = true;

        for (size_t field_index = 0;
             field_index < fields_.size();
             ++field_index) {
            const FieldDescriptor& field = fields_[field_index];
            FieldRef& ref = refs[field_index];
            ref.type = field.type;
            ref.is_null = false;
            ref.data = nullptr;
            ref.byte_len = 0;

            bool is_null = false;
            if ((field.flag & 1U) != 0) {
                const size_t byte_index =
                    static_cast<size_t>(nullable_bit / 8);
                const unsigned bit_index =
                    static_cast<unsigned>(nullable_bit % 8);
                is_null = bitmap != nullptr && byte_index < bitmap_size &&
                          ((bitmap[byte_index] >> bit_index) & 1U) != 0;
                ++nullable_bit;
            }
            if (is_null) {
                ref.is_null = true;
                continue;
            }

            if (set_fixed_field_ref(cursor, row_end, field.type, ref))
                continue;

            switch (field.type) {
                case FieldType::String:
                case FieldType::XML:
                case FieldType::Binary:
                case FieldType::Raster:
                case FieldType::Geometry: {
                    uint64_t size = 0;
                    if (!read_varuint_inline(cursor, row_end, size) ||
                        size > static_cast<uint64_t>(row_end - cursor) ||
                        size > std::numeric_limits<uint32_t>::max()) {
                        valid = false;
                        break;
                    }
                    ref.data = cursor;
                    ref.byte_len = static_cast<uint32_t>(size);
                    cursor += static_cast<size_t>(size);
                    break;
                }
                default:
                    valid = false;
                    break;
            }
            if (!valid) break;
        }

        if (!valid || !is_zero_padding(cursor, row_end)) return 0;
        if (!callback(fid, refs.data(), static_cast<int>(refs.size()))) break;
        ++scanned;
    }

    return scanned;
}

GdbGeomDecoder GdbTableParser::make_geom_decoder(
    const FieldDescriptor& field) const {
    GdbGeomDecoder decoder;
    decoder.set_transform(field.x_origin, field.y_origin, field.xy_scale,
                          field.z_origin, field.z_scale,
                          field.m_origin, field.m_scale,
                          field.has_z, field.has_m);
    return decoder;
}

const FieldDescriptor* GdbTableParser::geometry_field_descriptor() const {
    if (geometry_field_index_ < 0 ||
        geometry_field_index_ >= static_cast<int>(fields_.size())) {
        return nullptr;
    }
    return &fields_[static_cast<size_t>(geometry_field_index_)];
}

bool GdbTableParser::read_geometry_model(uint32_t fid,
                                         GeometryModel& model) {
    const uint8_t* blob = nullptr;
    size_t blob_size = 0;
    if (!peek_geometry_blob(fid, blob, blob_size)) return false;
    const FieldDescriptor* field = geometry_field_descriptor();
    if (field == nullptr) return false;
    GdbGeomDecoder decoder = make_geom_decoder(*field);
    return decoder.decode_to_model(blob, blob_size, fid, model);
}

bool GdbTableParser::read_geometry_value(uint32_t fid,
                                         GeometryValue& value) {
    const uint8_t* blob = nullptr;
    size_t blob_size = 0;
    if (!peek_geometry_blob(fid, blob, blob_size)) return false;
    const FieldDescriptor* field = geometry_field_descriptor();
    if (field == nullptr) return false;
    GdbGeomDecoder decoder = make_geom_decoder(*field);
    return decoder.decode_to_wkb(blob, blob_size, fid, value);
}

} // namespace explorgdb
