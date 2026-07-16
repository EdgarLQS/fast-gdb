// src/edgar/explorgdb/writer/gdb_table_writer.cpp
// .gdbtable 直接写入器实现
//
// 核心策略：
//   1. GDAL 只做 schema（建目录、建图层、建字段），创建空 .gdb
//   2. 我们发现数据表文件（a00000001.gdbtable）和数据起始偏移
//   3. 用 RowBuffer 编码每行，写入空 .gdbtable
//   4. 用 TablxWriter 记录行偏移，close 时写入 .gdbtablx
//   5. 更新 .gdbtable 头部（记录数 + 文件大小）

#include "gdb_table_writer.h"
#include "../common/binary_reader.h"
#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "gdb_tablx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>

namespace explorgdb {
namespace writer {

namespace {

bool seek_file(FILE* file, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

bool tell_file(FILE* file, uint64_t& offset) {
#ifdef _WIN32
    const __int64 position = _ftelli64(file);
#else
    const off_t position = ::ftello(file);
#endif
    if (position < 0) return false;
    offset = static_cast<uint64_t>(position);
    return true;
}

uint8_t filegdb_geometry_base_type(uint32_t geometry_type) {
    switch (geometry_type & 0xFF) {
        case 1: return 1;  // Point -> SHPT_POINT
        case 2: return 8;  // MultiPoint -> SHPT_MULTIPOINT
        case 3: return 3;  // Polyline -> SHPT_ARC
        case 4: return 5;  // Polygon -> SHPT_POLYGON
        default: return 0;
    }
}

bool count_utf8_codepoints(const std::string& value, size_t& count) {
    count = 0;
    for (size_t i = 0; i < value.size(); ++count) {
        const uint8_t lead = static_cast<uint8_t>(value[i]);
        size_t length = 0;
        uint32_t codepoint = 0;
        if (lead <= 0x7F) { length = 1; codepoint = lead; }
        else if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2; codepoint = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3; codepoint = lead & 0x0F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4; codepoint = lead & 0x07;
        } else return false;
        if (i + length > value.size()) return false;
        for (size_t j = 1; j < length; ++j) {
            const uint8_t byte = static_cast<uint8_t>(value[i + j]);
            if ((byte & 0xC0) != 0x80) return false;
            codepoint = (codepoint << 6) | (byte & 0x3F);
        }
        if ((length == 3 && codepoint < 0x800) ||
            (length == 4 && codepoint < 0x10000) ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF) ||
            codepoint > 0x10FFFF) return false;
        i += length;
    }
    return true;
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool parse_uuid(const std::string& value, uint8_t bytes[16]) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') return false;
    size_t byte_index = 0;
    for (size_t i = 0; i < value.size();) {
        if (value[i] == '-') { ++i; continue; }
        if (i + 1 >= value.size() || byte_index >= 16) return false;
        const int high = hex_value(value[i]);
        const int low = hex_value(value[i + 1]);
        if (high < 0 || low < 0) return false;
        bytes[byte_index++] = static_cast<uint8_t>((high << 4) | low);
        i += 2;
    }
    return byte_index == 16;
}

void uuid_to_filegdb_order(uint8_t bytes[16]) {
    std::reverse(bytes, bytes + 4);
    std::reverse(bytes + 4, bytes + 6);
    std::reverse(bytes + 6, bytes + 8);
}

bool supports_schema_default(FieldType type) {
    switch (type) {
        case FieldType::Int16:
        case FieldType::Int32:
        case FieldType::Int64:
        case FieldType::Float32:
        case FieldType::Float64:
        case FieldType::String:
        case FieldType::DateTime:
        case FieldType::Date:
        case FieldType::Time:
        case FieldType::DateTimeWithOffset:
            return true;
        default:
            return false;
    }
}

}  // namespace

// ── 构造 / 析构 ──

GdbTableWriter::GdbTableWriter() {
    write_buffer_.resize(kMaxBufferBytes);
}

GdbTableWriter::~GdbTableWriter() {
    if (is_open_) close();
}

bool GdbTableWriter::fail(const std::string& stage,
                          const std::string& message) {
    last_error_ = "[writer] " + stage + " failed for layer '" + layer_name_ +
                  "' in '" + gdb_path_ + "': " + message;
    std::cerr << last_error_ << "\n";
    return false;
}

bool GdbTableWriter::fail_and_close(FILE*& file, const std::string& stage,
                                    const std::string& message) {
    std::string detail = message;
    if (file && std::fclose(file) != 0) {
        detail += "; close also failed: ";
        detail += std::strerror(errno);
    }
    file = nullptr;
    return fail(stage, detail);
}

// ── 打开已有 .gdb ──
bool GdbTableWriter::open_existing(const std::string& gdb_path, const std::string& layer_name) {
    if (is_open_) {
        return fail("open", "writer is already open");
    }
    gdb_path_ = gdb_path;
    layer_name_ = layer_name;
    last_error_.clear();

    if (!discover_table_layout()) {
        return false;
    }

    // 门禁已确认目标为 pristine empty schema，此后才以读写模式打开。
    table_fp_ = std::fopen(table_path_.c_str(), "r+b");
    if (!table_fp_) {
        return fail("open table", table_path_ + ": " + std::strerror(errno));
    }

    // 定位到数据区末尾
    if (!seek_file(table_fp_, current_offset_)) {
        const std::string reason = std::strerror(errno);
        return fail_and_close(table_fp_, "seek data start",
                              table_path_ + ": " + reason);
    }

    // 初始化 RowBuffer（使用描述符字段数量和 nullable flags）
    int num_desc_fields = static_cast<int>(field_descriptors_.size());
    row_buffer_.init(num_desc_fields, nullable_flags_);
    if (objectid_descriptor_index_ >= 0) {
        row_buffer_.mark_objectid(objectid_descriptor_index_);
    }

    // 初始化 GeometrySerializer
    geom_serializer_.reset(xorig_, yorig_, xyscale_, zorig_, zscale_, morig_, mscale_);

    tablx_writer_.clear();
    row_count_ = 0;
    buffered_rows_ = 0;
    write_buffer_pos_ = 0;
    max_row_size_ = 0;
    in_row_ = false;
    row_valid_ = false;
    io_failed_ = false;
    field_written_.assign(field_descriptors_.size(), false);
    is_open_ = true;
    return true;
}

// ── 发现数据表布局 ──
// 扫描 .gdb 目录，找到匹配字段名的数据表，解析字段描述符获取坐标系参数
bool GdbTableWriter::discover_table_layout() {
    std::string table_path;
    std::string tablx_path;
    if (!resolve_pristine_empty_table(table_path, tablx_path)) return false;
    return parse_table_layout(table_path, tablx_path);
}

bool GdbTableWriter::resolve_pristine_empty_table(
    std::string& table_path, std::string& tablx_path) {
    GdbCatalog catalog;
    if (!catalog.scan(gdb_path_)) {
        return fail("discover", "cannot scan GDB directory");
    }
    CatalogResolver resolver(catalog);
    if (!resolver.load()) {
        return fail("discover", "cannot load system catalog");
    }
    const auto resolved = resolver.resolve(layer_name_);
    if (!resolved) {
        return fail("discover", "layer not found");
    }
    if (resolved->tablx_path.empty()) {
        return fail("discover", "missing .gdbtablx");
    }

    GdbTablxParser tablx_parser(resolved->tablx_path);
    if (!tablx_parser.parse()) {
        return fail("discover", "cannot parse " + resolved->tablx_path);
    }
    if (tablx_parser.feature_count() != 0) {
        return fail("discover", "refusing non-empty layer (" +
                    std::to_string(tablx_parser.feature_count()) + " records)");
    }
    const auto& tablx_header = tablx_parser.header();
    const uint64_t record_slots = tablx_header.version == 3
        ? tablx_header.nfeatures_v3 : tablx_header.nfeatures_v4;
    if (record_slots != 0) {
        return fail("discover", "target is not a pristine empty schema (" +
                    std::to_string(record_slots) + " historical record slots)");
    }

    table_path = resolved->table_path;
    tablx_path = resolved->tablx_path;
    return true;
}

bool GdbTableWriter::parse_table_layout(const std::string& tpath,
                                        const std::string& tablx_path) {
    FILE* fp = std::fopen(tpath.c_str(), "rb");
    if (!fp) {
        return fail("parse table", tpath + ": " + std::strerror(errno));
    }
    try {
            // 读 header
            uint8_t hdr[48];
            if (std::fread(hdr, 1, 48, fp) < 48) {
                return fail_and_close(fp, "parse table",
                                      tpath + ": truncated header");
            }

            uint32_t version = 0;
            std::memcpy(&version, hdr, sizeof(version));
            uint64_t field_desc_offset = 0;
            uint64_t table_record_count = 0;
            uint64_t declared_file_size = 0;

            if (version == 4) {
                std::memcpy(&field_desc_offset, hdr + 40, 8);
                std::memcpy(&table_record_count, hdr + 24, 8);
                std::memcpy(&declared_file_size, hdr + 32, 8);
            } else if (version == 3) {
                std::memcpy(&field_desc_offset, hdr + 32, 8);
                uint32_t count_v3 = 0;
                std::memcpy(&count_v3, hdr + 4, 4);
                table_record_count = count_v3;
                std::memcpy(&declared_file_size, hdr + 24, 8);
            } else {
                return fail_and_close(
                    fp, "parse table", tpath + ": unsupported table version");
            }

            // 读 section header 获取 section_length
            uint8_t sec_hdr[12];
            if (!seek_file(fp, field_desc_offset)) {
                const std::string reason = std::strerror(errno);
                return fail_and_close(fp, "seek field section",
                                      tpath + ": " + reason);
            }
            if (std::fread(sec_hdr, 1, 12, fp) < 12) {
                return fail_and_close(
                    fp, "parse table", tpath + ": truncated field section");
            }

            uint32_t section_length = 0;
            uint32_t section_version = 0;
            uint32_t geom_type_full = 0;
            std::memcpy(&section_length, sec_hdr, sizeof(section_length));
            std::memcpy(&section_version, sec_hdr + 4, sizeof(section_version));
            std::memcpy(&geom_type_full, sec_hdr + 8, sizeof(geom_type_full));
            (void)section_version;

            // 数据起始偏移
            uint64_t data_start = field_desc_offset + 4 + section_length;
            std::error_code file_size_error;
            const uint64_t actual_file_size = std::filesystem::file_size(
                tpath, file_size_error);
            if (file_size_error) {
                return fail_and_close(fp, "parse table", tpath + ": " +
                                      file_size_error.message());
            }
            if (table_record_count != 0 || actual_file_size != data_start ||
                declared_file_size != actual_file_size) {
                return fail_and_close(
                    fp, "parse table", tpath +
                    ": inconsistent non-empty table metadata");
            }

            // 提取图层 Z/M 能力（编码在 geom_type_full 的高位）
            bool layer_has_z = (geom_type_full >> 31) != 0;
            bool layer_has_m = ((geom_type_full >> 30) & 1) != 0;

            // 读取字段描述符区
            size_t field_section_size = 4 + section_length;
            std::vector<uint8_t> field_data(field_section_size);
            if (!seek_file(fp, field_desc_offset)) {
                const std::string reason = std::strerror(errno);
                return fail_and_close(fp, "seek field metadata",
                                      tpath + ": " + reason);
            }
            if (std::fread(field_data.data(), 1, field_section_size, fp) !=
                field_section_size) {
                return fail_and_close(fp, "read field metadata",
                                      tpath + ": truncated data");
            }

            // 解析字段
            BinaryReader br(field_data);
            br.skip(12);  // section_length(4) + section_version(4) + geom_type_full(4)
            uint16_t nfields = br.read_u16();

            std::vector<FieldDescriptor> fds;
            std::vector<bool> null_flags;
            int geom_idx = -1;
            double fxorig = 0, fyorig = 0, fxyscale = 0;
            double fzorig = 0, fzscale = 1;
            double fmorig = 0, fmscale = 1;

            bool parse_ok = true;
            for (uint16_t fi = 0; fi < nfields && parse_ok; ++fi) {
            FieldDescriptor fd;
            // name (always UTF-16 in modern FileGDB)
            uint8_t name_len = br.read_u8();
            fd.name = br.read_utf16(name_len);

            // alias
            uint8_t alias_len = br.read_u8();
            if (alias_len > 0) {
                fd.alias = br.read_utf16(alias_len);
            }
            // type
            fd.type = static_cast<FieldType>(br.read_u8());

            switch (fd.type) {
                case FieldType::Int16: case FieldType::Int32:
                case FieldType::Float32: case FieldType::Float64:
                case FieldType::DateTime: case FieldType::Date:
                case FieldType::Time: case FieldType::DateTimeWithOffset:
                case FieldType::Int64: {
                    fd.width = br.read_u8();
                    fd.flag = br.read_u8();
                    uint8_t default_len = br.read_u8();
                    if (default_len > 0 && (fd.flag & 4)) br.skip(default_len);
                    break;
                }
                case FieldType::String: {
                    fd.width = br.read_u32();
                    fd.flag = br.read_u8();
                    uint64_t default_len = br.read_varuint();
                    if (default_len > 0 && (fd.flag & 4)) br.skip(static_cast<size_t>(default_len));
                    break;
                }
                case FieldType::ObjectId: {
                    fd.width = br.read_u8();
                    fd.flag = br.read_u8();
                    break;
                }
                case FieldType::Geometry: {
                    br.read_u8();  // magic1
                    fd.flag = br.read_u8();
                    uint16_t wkt_len = br.read_u16();
                    br.skip(wkt_len);  // WKT string bytes
                    uint8_t gf = br.read_u8();
                    bool desc_has_m = (gf & 2) != 0;
                    bool desc_has_z = (gf & 4) != 0;
                    fd.xorig = br.read_f64();
                    fd.yorig = br.read_f64();
                    fd.xyscale = br.read_f64();
                    if (desc_has_m) { fd.morig = br.read_f64(); fd.mscale = br.read_f64(); }
                    if (desc_has_z) { fd.zorig = br.read_f64(); fd.zscale = br.read_f64(); }
                    br.read_f64();  // xytolerance
                    if (desc_has_m) br.read_f64();  // mtolerance
                    if (desc_has_z) br.read_f64();  // ztolerance
                    br.skip(32);  // bbox: xmin,ymin,xmax,ymax
                    if (layer_has_z) br.skip(16);  // zmin, zmax
                    if (layer_has_m) br.skip(16);  // mmin, mmax
                    br.read_u8();  // terminator
                    uint32_t nb_grid = br.read_u32();
                    br.skip(nb_grid * 8);  // grid_sizes
                    break;
                }
                case FieldType::Binary:
                case FieldType::Raster: {
                    fd.flag = br.read_u8();
                    if (fd.type == FieldType::Raster) {
                        uint8_t rn_len = br.read_u8();
                        if (rn_len > 0) br.skip(rn_len * 2);  // UTF-16 name
                        uint16_t rwkt_len = br.read_u16();
                        br.skip(rwkt_len);  // UTF-16 WKT
                        br.skip(2);  // magic3 + raster_type
                    }
                    break;
                }
                case FieldType::UUID_1: case FieldType::UUID_2:
                case FieldType::XML: {
                    fd.width = br.read_u8();
                    fd.flag = br.read_u8();
                    break;
                }
                default:
                    parse_ok = false;
                    break;
            }

            if (parse_ok) {
                if (fd.type == FieldType::Geometry) {
                    geom_idx = static_cast<int>(fds.size());
                    fxorig = fd.xorig;
                    fyorig = fd.yorig;
                    fxyscale = fd.xyscale;
                    fzorig = fd.zorig;
                    fzscale = fd.zscale;
                    fmorig = fd.morig;
                    fmscale = fd.mscale;
                }
                null_flags.push_back((fd.flag & 1) != 0);
                fds.push_back(std::move(fd));
            }
        }

        if (std::fclose(fp) != 0) {
            fp = nullptr;
            return fail("close table", tpath + ": " + std::strerror(errno));
        }
        fp = nullptr;

        if (!parse_ok || geom_idx < 0) {
            return fail("parse table", tpath + ": invalid spatial field metadata");
        }

        // 检查字段名是否匹配（排除 ObjectId 和 Geometry 字段）
        // GDAL 创建图层的字段名是用户指定的，与图层名无关
        // 我们只需确认存在 geometry 字段即可（已在上面检查 geom_idx >= 0）

        // 找到匹配的数据表！
        table_path_ = tpath;
        // 对应的 .gdbtablx
        tablx_path_ = tablx_path;

        field_descriptors_ = std::move(fds);
        nullable_flags_ = std::move(null_flags);
        geometry_field_index_ = geom_idx;
        geometry_base_type_ = filegdb_geometry_base_type(geom_type_full);
        xorig_ = fxorig;
        yorig_ = fyorig;
        xyscale_ = fxyscale;
        zorig_ = fzorig;
        zscale_ = fzscale;
        morig_ = fmorig;
        mscale_ = fmscale;
        data_start_offset_ = data_start;
        current_offset_ = data_start;

        // 构建用户字段索引 → 描述符字段索引的映射
        // 描述符顺序通常是: Geometry[0], ObjectId[1], name[2], population[3], area[4]
        // 用户字段顺序: name[0], population[1], area[2], geometry[3]
        // 需要按名称匹配（不能用顺序，因为描述符顺序与用户顺序不同）
        user_to_descriptor_.clear();
        objectid_descriptor_index_ = -1;

        // 找到 ObjectId 在描述符中的位置
        for (size_t i = 0; i < field_descriptors_.size(); ++i) {
            if (field_descriptors_[i].type == FieldType::ObjectId) {
                objectid_descriptor_index_ = static_cast<int>(i);
                break;
            }
        }

        // 按名称匹配用户字段到描述符字段
        if (!user_fields_.empty()) {
            for (size_t ui = 0; ui < user_fields_.size(); ++ui) {
                bool found = false;
                for (size_t di = 0; di < field_descriptors_.size(); ++di) {
                    if (field_descriptors_[di].name == user_fields_[ui].name) {
                        user_to_descriptor_.push_back(static_cast<int>(di));
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    std::cerr << "[writer] WARNING: user field '" << user_fields_[ui].name
                              << "' not found in descriptors\n";
                    user_to_descriptor_.push_back(-1);
                }
            }
        } else {
            // open_existing() 没有 user_fields_：按描述符顺序跳过 ObjectId，
            // Geometry 放最后
            for (size_t i = 0; i < field_descriptors_.size(); ++i) {
                if (field_descriptors_[i].type == FieldType::ObjectId) continue;
                if (field_descriptors_[i].type == FieldType::Geometry) continue;
                user_to_descriptor_.push_back(static_cast<int>(i));
            }
        }

        // 添加 Geometry 字段（用户通过 append_geometry 写入）
        if (geometry_field_index_ >= 0) {
            user_to_descriptor_.push_back(geometry_field_index_);
        }

        return true;

    } catch (const std::exception& error) {
        return fail_and_close(fp, "parse table",
                              tpath + ": " + error.what());
    }
}

// ── 行操作 ──

bool GdbTableWriter::begin_row() {
    if (!is_open_) return fail("begin row", "writer is not open");
    if (io_failed_) return fail("begin row", "writer has an earlier I/O failure");
    if (in_row_) {
        row_valid_ = false;
        return fail("begin row", "a row is already active");
    }
    row_buffer_.begin_row();
    std::fill(field_written_.begin(), field_written_.end(), false);
    in_row_ = true;
    row_valid_ = true;
    return true;
}

bool GdbTableWriter::validate_field_index(int field_index,
                                         int& descriptor_index) {
    if (!is_open_) return fail("write field", "writer is not open");
    if (!in_row_) return fail("write field", "no row is active");
    if (!row_valid_) return false;
    if (field_index < 0 ||
        static_cast<size_t>(field_index) >= user_to_descriptor_.size()) {
        row_valid_ = false;
        return fail("write field", "field index " +
                    std::to_string(field_index) + " is out of range");
    }
    descriptor_index = user_to_descriptor_[static_cast<size_t>(field_index)];
    if (descriptor_index < 0 ||
        static_cast<size_t>(descriptor_index) >= field_descriptors_.size()) {
        row_valid_ = false;
        return fail("write field", "field index " +
                    std::to_string(field_index) + " is not mapped");
    }
    return true;
}

bool GdbTableWriter::validate_field(int field_index, FieldType expected_type,
                                    int& descriptor_index) {
    if (!validate_field_index(field_index, descriptor_index)) return false;
    const FieldType actual = field_descriptors_[descriptor_index].type;
    if (actual != expected_type) {
        row_valid_ = false;
        return fail("write field", "field index " +
                    std::to_string(field_index) + " ('" +
                    field_descriptors_[descriptor_index].name +
                    "') has type " + field_type_name(actual) +
                    ", not " + field_type_name(expected_type));
    }
    return mark_field_written(descriptor_index);
}

bool GdbTableWriter::mark_field_written(int descriptor_index) {
    if (field_written_[descriptor_index]) {
        row_valid_ = false;
        return fail("write field", "field '" +
                    field_descriptors_[descriptor_index].name +
                    "' was written more than once");
    }
    field_written_[descriptor_index] = true;
    return true;
}

bool GdbTableWriter::reject_field_value(int descriptor_index,
                                        const std::string& message) {
    field_written_[descriptor_index] = false;
    row_valid_ = false;
    return fail("write field", message);
}

bool GdbTableWriter::set_null(int field_index) {
    int descriptor_index = -1;
    if (!validate_field_index(field_index, descriptor_index)) return false;
    if (!nullable_flags_[descriptor_index]) {
        row_valid_ = false;
        return fail("write field", "field '" +
                    field_descriptors_[descriptor_index].name +
                    "' is not nullable");
    }
    if (!mark_field_written(descriptor_index)) return false;
    row_buffer_.set_field(descriptor_index);
    row_buffer_.set_null();
    return true;
}

bool GdbTableWriter::append_i16(int field_index, int16_t value) {
    int descriptor_index = -1;
    if (!validate_field(field_index, FieldType::Int16, descriptor_index)) return false;
    row_buffer_.set_field(descriptor_index);
    row_buffer_.append_i16(value);
    return true;
}

bool GdbTableWriter::append_i32(int field_index, int32_t value) {
    int descriptor_index = -1;
    if (!validate_field(field_index, FieldType::Int32, descriptor_index)) return false;
    row_buffer_.set_field(descriptor_index);
    row_buffer_.append_i32(value);
    return true;
}

bool GdbTableWriter::append_i64(int field_index, int64_t value) {
    int descriptor_index = -1;
    if (!validate_field(field_index, FieldType::Int64, descriptor_index)) return false;
    row_buffer_.set_field(descriptor_index);
    row_buffer_.append_i64(value);
    return true;
}

bool GdbTableWriter::append_f32(int field_index, float value) {
    int descriptor_index = -1;
    if (!validate_field(field_index, FieldType::Float32, descriptor_index)) return false;
    if (!std::isfinite(value)) {
        return reject_field_value(descriptor_index,
                                  "floating-point values must be finite");
    }
    row_buffer_.set_field(descriptor_index);
    row_buffer_.append_f32(value);
    return true;
}

bool GdbTableWriter::append_f64(int field_index, double value) {
    int descriptor_index = -1;
    if (!validate_field(field_index, FieldType::Float64, descriptor_index)) return false;
    if (!std::isfinite(value)) {
        return reject_field_value(descriptor_index,
                                  "floating-point values must be finite");
    }
    row_buffer_.set_field(descriptor_index);
    row_buffer_.append_f64(value);
    return true;
}

bool GdbTableWriter::append_string(int field_index, const std::string& value) {
    int descriptor_index = -1;
    if (!validate_field(field_index, FieldType::String, descriptor_index)) return false;
    size_t codepoints = 0;
    if (!count_utf8_codepoints(value, codepoints)) {
        return reject_field_value(descriptor_index,
                                  "string is not valid UTF-8");
    }
    const uint32_t width = field_descriptors_[descriptor_index].width;
    if (width != 0 && codepoints > width) {
        return reject_field_value(
            descriptor_index, "string exceeds field width " +
            std::to_string(width) + " (got " +
            std::to_string(codepoints) + " characters)");
    }
    row_buffer_.set_field(descriptor_index);
    row_buffer_.append_string(value);
    return true;
}

bool GdbTableWriter::append_binary(int field_index,
                                   const std::vector<uint8_t>& value) {
    int descriptor_index = -1;
    if (!validate_field(field_index, FieldType::Binary, descriptor_index)) {
        return false;
    }
    row_buffer_.set_field(descriptor_index);
    row_buffer_.append_binary(value.data(), value.size());
    return true;
}

bool GdbTableWriter::append_xml(int field_index, const std::string& value) {
    int descriptor_index = -1;
    if (!validate_field(field_index, FieldType::XML, descriptor_index)) return false;
    size_t codepoints = 0;
    if (!count_utf8_codepoints(value, codepoints)) {
        return reject_field_value(descriptor_index, "XML is not valid UTF-8");
    }
    row_buffer_.set_field(descriptor_index);
    row_buffer_.append_string(value);
    return true;
}

bool GdbTableWriter::append_uuid(int field_index, const std::string& value) {
    int descriptor_index = -1;
    if (!validate_field_index(field_index, descriptor_index)) return false;
    const FieldType type = field_descriptors_[descriptor_index].type;
    if (type != FieldType::UUID_1 && type != FieldType::UUID_2) {
        return reject_field_value(descriptor_index, "field type must be GUID or GlobalID");
    }
    if (!mark_field_written(descriptor_index)) return false;
    uint8_t bytes[16];
    if (!parse_uuid(value, bytes)) {
        return reject_field_value(descriptor_index,
                                  "UUID must use canonical 8-4-4-4-12 hexadecimal form");
    }
    uuid_to_filegdb_order(bytes);
    row_buffer_.set_field(descriptor_index);
    row_buffer_.append_bytes(bytes, sizeof(bytes));
    return true;
}

bool GdbTableWriter::append_datetime(int field_index, double ole_date) {
    int descriptor_index = -1;
    if (!validate_field(field_index, FieldType::DateTime, descriptor_index)) return false;
    if (!std::isfinite(ole_date)) {
        return reject_field_value(descriptor_index, "date-time value must be finite");
    }
    row_buffer_.set_field(descriptor_index);
    row_buffer_.append_f64(ole_date);
    return true;
}

bool GdbTableWriter::append_date(int field_index, double ole_date) {
    int descriptor_index = -1;
    if (!validate_field(field_index, FieldType::Date, descriptor_index)) return false;
    if (!std::isfinite(ole_date) || std::trunc(ole_date) != ole_date) {
        return reject_field_value(descriptor_index, "date value must be a finite whole OLE day");
    }
    row_buffer_.set_field(descriptor_index);
    row_buffer_.append_f64(ole_date);
    return true;
}

bool GdbTableWriter::append_time(int field_index, double ole_time) {
    int descriptor_index = -1;
    if (!validate_field(field_index, FieldType::Time, descriptor_index)) return false;
    if (!std::isfinite(ole_time) || ole_time < 0.0 || ole_time >= 1.0) {
        return reject_field_value(descriptor_index, "time value must be in OLE range [0, 1)");
    }
    row_buffer_.set_field(descriptor_index);
    row_buffer_.append_f64(ole_time);
    return true;
}

bool GdbTableWriter::append_datetime_with_offset(
    int field_index, double ole_date, int16_t offset_minutes) {
    int descriptor_index = -1;
    if (!validate_field(field_index, FieldType::DateTimeWithOffset,
                        descriptor_index)) return false;
    if (!std::isfinite(ole_date)) {
        return reject_field_value(descriptor_index, "date-time value must be finite");
    }
    if (offset_minutes < -14 * 60 || offset_minutes > 14 * 60) {
        return reject_field_value(descriptor_index,
                                  "UTC offset must be between -840 and 840 minutes");
    }
    row_buffer_.set_field(descriptor_index);
    row_buffer_.append_f64(ole_date);
    row_buffer_.append_i16(offset_minutes);
    return true;
}

bool GdbTableWriter::append_geometry(int field_index) {
    int descriptor_index = -1;
    if (!validate_field(field_index, FieldType::Geometry, descriptor_index)) return false;
    if (geom_serializer_.blob_size() == 0) {
        row_valid_ = false;
        field_written_[descriptor_index] = false;
        const std::string reason = geom_serializer_.last_error().empty()
            ? "geometry has not been serialized"
            : geom_serializer_.last_error();
        return fail("write geometry", reason);
    }
    const uint8_t serialized_base = static_cast<uint8_t>(
        static_cast<uint32_t>(geom_serializer_.serialized_type()) & 0xFF);
    if (geometry_base_type_ != 0 && serialized_base != geometry_base_type_) {
        row_valid_ = false;
        field_written_[descriptor_index] = false;
        return fail("write geometry", "serialized geometry type " +
                    std::to_string(serialized_base) +
                    " does not match layer geometry type " +
                    std::to_string(geometry_base_type_));
    }
    row_buffer_.set_field(descriptor_index);
    row_buffer_.append_geometry(geom_serializer_.blob_data(), geom_serializer_.blob_size());
    return true;
}

bool GdbTableWriter::prepare_missing_fields() {
    for (size_t i = 0; i < field_descriptors_.size(); ++i) {
        if (field_descriptors_[i].type == FieldType::ObjectId ||
            field_written_[i]) {
            continue;
        }
        if (supports_schema_default(field_descriptors_[i].type) &&
            (field_descriptors_[i].flag & 4U) != 0) {
            row_valid_ = false;
            return fail("end row", "field '" +
                        field_descriptors_[i].name +
                        "' has a schema default; set it explicitly");
        }
        if (!nullable_flags_[i]) {
            row_valid_ = false;
            return fail("end row", "required field '" +
                        field_descriptors_[i].name + "' is missing");
        }
        row_buffer_.set_field(static_cast<int>(i));
        row_buffer_.set_null();
    }
    return true;
}

bool GdbTableWriter::end_row() {
    if (!is_open_) return fail("end row", "writer is not open");
    if (!in_row_) return fail("end row", "no row is active");
    if (!row_valid_) {
        in_row_ = false;
        return false;
    }
    if (!prepare_missing_fields()) {
        in_row_ = false;
        return false;
    }
    row_buffer_.finalize();

    const uint8_t* row_data = row_buffer_.data();
    const size_t row_size = row_buffer_.size();

    // 记录当前偏移（这行在 .gdbtable 中的位置）
    // current_offset_ = 已刷盘到文件的总字节数
    // write_buffer_pos_ = 缓冲区中尚未刷盘的字节数
    if (row_size > std::numeric_limits<uint32_t>::max()) {
        in_row_ = false;
        return fail("end row", "encoded row exceeds the FileGDB uint32 limit");
    }
    if (current_offset_ > std::numeric_limits<uint64_t>::max() -
                          write_buffer_pos_) {
        in_row_ = false;
        return fail("end row", "file offset overflow");
    }
    const uint64_t row_offset = current_offset_ + write_buffer_pos_;
    if (!append_encoded_row(row_data, row_size, row_offset)) {
        in_row_ = false;
        return false;
    }
    tablx_writer_.add_offset(row_offset);

    // 更新统计
    ++row_count_;
    ++buffered_rows_;
    if (row_size > max_row_size_) max_row_size_ = static_cast<uint32_t>(row_size);

    // 检查是否需要刷盘
    if (buffered_rows_ >= kMaxBufferRows || write_buffer_pos_ >= kMaxBufferBytes) {
        if (!internal_flush()) {
            in_row_ = false;
            return false;
        }
    }

    in_row_ = false;
    return true;
}

bool GdbTableWriter::append_encoded_row(const uint8_t* row_data,
                                        size_t row_size,
                                        uint64_t row_offset) {
    if (row_size > write_buffer_.size()) {
        if (!internal_flush()) return false;
        if (std::fwrite(row_data, 1, row_size, table_fp_) != row_size) {
            io_failed_ = true;
            return fail("write row at " + std::to_string(row_offset),
                        table_path_ + ": " + std::strerror(errno));
        }
        current_offset_ += row_size;
        return true;
    }
    if (write_buffer_pos_ + row_size > write_buffer_.size() &&
        !internal_flush()) {
        return false;
    }
    std::memcpy(write_buffer_.data() + write_buffer_pos_, row_data, row_size);
    write_buffer_pos_ += row_size;
    return true;
}

// ── 刷盘 ──

bool GdbTableWriter::internal_flush() {
    if (write_buffer_pos_ == 0) return true;
    if (!table_fp_) return fail("flush", "table file is not open");

    if (std::fwrite(write_buffer_.data(), 1, write_buffer_pos_, table_fp_) !=
        write_buffer_pos_) {
        io_failed_ = true;
        return fail("flush", table_path_ + ": " + std::strerror(errno));
    }
    current_offset_ += write_buffer_pos_;  // 累加已刷盘的字节数
    write_buffer_pos_ = 0;
    buffered_rows_ = 0;
    return true;
}

bool GdbTableWriter::flush() {
    if (!is_open_) return fail("flush", "writer is not open");
    if (!internal_flush()) return false;
    if (std::fflush(table_fp_) != 0) {
        io_failed_ = true;
        return fail("flush", table_path_ + ": " + std::strerror(errno));
    }
    return true;
}

// ── 关闭 ──

bool GdbTableWriter::close() {
    if (!is_open_) return true;
    bool ok = !io_failed_;
    if (in_row_) {
        fail("close", "a row is still active");
        ok = false;
        in_row_ = false;
    }

    // 1. 刷盘
    if (ok && !internal_flush()) ok = false;

    // 2. 更新 .gdbtable 头部
    if (ok && !update_table_header()) ok = false;

    // 3. 写 .gdbtablx
    std::string tablx_error;
    if (ok && !tablx_writer_.write(tablx_path_, &tablx_error)) {
        fail("write tablx", tablx_path_ + ": " + tablx_error);
        ok = false;
    }

    // 4. 关闭文件
    if (table_fp_) {
        if (std::fclose(table_fp_) != 0) {
            fail("close table", table_path_ + ": " + std::strerror(errno));
            ok = false;
        }
        table_fp_ = nullptr;
    }

    is_open_ = false;
    return ok;
}

// ── 更新 .gdbtable 头部 ──
bool GdbTableWriter::update_table_header() {
    if (!table_fp_) return fail("update header", "table file is not open");
    uint32_t version = 0;
    uint64_t file_size = 0;
    if (!read_table_header(version, file_size)) return false;
    bool written = false;
    if (version == 4) written = write_v4_header(file_size);
    else if (version == 3) written = write_v3_header(file_size);
    else return fail("update header", table_path_ + ": unsupported version");
    if (!written) return false;
    if (std::fflush(table_fp_) != 0) {
        return fail("flush header", table_path_ + ": " +
                    std::strerror(errno));
    }
    return true;
}

bool GdbTableWriter::read_table_header(uint32_t& version,
                                       uint64_t& file_size) {
    if (std::fflush(table_fp_) != 0) {
        return fail("flush before header", table_path_ + ": " +
                    std::strerror(errno));
    }
    if (!seek_file(table_fp_, current_offset_)) {
        return fail("seek table end", table_path_ + ": " +
                    std::strerror(errno));
    }
    if (!tell_file(table_fp_, file_size)) {
        return fail("tell table size", table_path_ + ": " +
                    std::strerror(errno));
    }

    // 读 version
    if (!seek_file(table_fp_, 0)) {
        return fail("seek header", table_path_ + ": " +
                    std::strerror(errno));
    }
    uint8_t hdr[48];
    if (std::fread(hdr, 1, sizeof(hdr), table_fp_) != sizeof(hdr)) {
        return fail("read header", table_path_ + ": " +
                    std::strerror(errno));
    }
    std::memcpy(&version, hdr, sizeof(version));
    return true;
}

bool GdbTableWriter::write_v4_header(uint64_t file_size) {
    const uint64_t nfeatures = row_count_;
    if (!seek_file(table_fp_, 24) ||
        std::fwrite(&nfeatures, 8, 1, table_fp_) != 1 ||
        std::fwrite(&file_size, 8, 1, table_fp_) != 1 ||
        !seek_file(table_fp_, 8) ||
        std::fwrite(&max_row_size_, 4, 1, table_fp_) != 1) {
        return fail("write v4 header", table_path_ + ": " +
                    std::strerror(errno));
    }
    return true;
}

bool GdbTableWriter::write_v3_header(uint64_t file_size) {
    if (row_count_ > std::numeric_limits<uint32_t>::max()) {
        return fail("write v3 header", "record count exceeds uint32");
    }
    const uint32_t nfeatures = static_cast<uint32_t>(row_count_);
    if (!seek_file(table_fp_, 4) ||
        std::fwrite(&nfeatures, 4, 1, table_fp_) != 1 ||
        std::fwrite(&max_row_size_, 4, 1, table_fp_) != 1 ||
        !seek_file(table_fp_, 24) ||
        std::fwrite(&file_size, 8, 1, table_fp_) != 1) {
        return fail("write v3 header", table_path_ + ": " +
                    std::strerror(errno));
    }
    return true;
}

}  // namespace writer
}  // namespace explorgdb
