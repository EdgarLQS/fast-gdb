// src/edgar/explorgdb/writer/gdb_table_writer.cpp
// .gdbtable 直接写入器实现
//
// 核心策略：
//   1. GDAL 只做 schema（建目录、建图层、建字段），创建空 .gdb
//   2. 我们发现数据表文件（a00000001.gdbtable）和数据起始偏移
//   3. 用 RowBuffer 编码每行，直接追加到 .gdbtable
//   4. 用 TablxWriter 记录行偏移，close 时写入 .gdbtablx
//   5. 更新 .gdbtable 头部（记录数 + 文件大小）

#include "gdb_table_writer.h"
#include "varint_encoder.h"
#include "../binary_reader.h"
#include "../gdb_catalog.h"

#include "gdal.h"
#include "gdal_priv.h"
#include "ogr_api.h"
#include "ogr_srs_api.h"
#include "ogrsf_frmts.h"
#include "cpl_conv.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <filesystem>

namespace explorgdb {
namespace writer {

// ── 构造 / 析构 ──

GdbTableWriter::GdbTableWriter() {
    write_buffer_.resize(kMaxBufferBytes);
}

GdbTableWriter::~GdbTableWriter() {
    if (is_open_) close();
}

// ── 创建新 .gdb（GDAL 建 schema）──
bool GdbTableWriter::create_new(const std::string& gdb_path,
                                 const std::string& layer_name,
                                 const std::vector<WriterField>& fields,
                                 const std::string& wkt_srs,
                                 int geom_type) {
    // 保存用户字段定义（用于后续按名称匹配描述符）
    user_fields_ = fields;

    // 用 GDAL 创建空 .gdb
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!driver) {
        std::cerr << "OpenFileGDB driver not available\n";
        return false;
    }

    // 创建 .gdb 目录
    char** options = nullptr;
    GDALDataset* ds = driver->Create(gdb_path.c_str(), 0, 0, 0, GDT_Unknown, options);
    if (!ds) {
        std::cerr << "Failed to create: " << gdb_path << "\n";
        return false;
    }

    // 创建图层
    OGRSpatialReference* srs = nullptr;
    if (!wkt_srs.empty()) {
        srs = new OGRSpatialReference();
        const char* wkt_ptr = wkt_srs.c_str();
        srs->importFromWkt(&wkt_ptr);
    }

    OGRLayer* layer = ds->CreateLayer(layer_name.c_str(), srs,
                                       static_cast<OGRwkbGeometryType>(geom_type), nullptr);
    if (!layer) {
        std::cerr << "Failed to create layer: " << layer_name << "\n";
        GDALClose(ds);
        if (srs) srs->Dereference();
        return false;
    }

    // 添加字段
    for (const auto& f : fields) {
        OGRFieldDefn ogr_field(f.name.c_str(), OFTString);  // 默认 String

        switch (f.type) {
            case FieldType::Int16:
                ogr_field.SetType(OFTInteger);
                ogr_field.SetSubType(OFSTInt16);
                break;
            case FieldType::Int32:
                ogr_field.SetType(OFTInteger);
                break;
            case FieldType::Int64:
                ogr_field.SetType(OFTInteger64);
                break;
            case FieldType::Float32:
                ogr_field.SetType(OFTReal);
                ogr_field.SetSubType(OFSTFloat32);
                break;
            case FieldType::Float64:
                ogr_field.SetType(OFTReal);
                break;
            case FieldType::String:
                ogr_field.SetType(OFTString);
                if (f.max_width > 0) ogr_field.SetWidth(f.max_width);
                break;
            default:
                ogr_field.SetType(OFTString);
                break;
        }

        ogr_field.SetNullable(f.nullable);
        layer->CreateField(&ogr_field);
    }

    if (srs) srs->Dereference();

    // 关闭 GDAL 数据集（flush 到磁盘）
    GDALClose(ds);

    // 现在用 open_existing 打开（我们接管数据写入）
    return open_existing(gdb_path, layer_name);
}

// ── 打开已有 .gdb ──
bool GdbTableWriter::open_existing(const std::string& gdb_path, const std::string& layer_name) {
    gdb_path_ = gdb_path;
    layer_name_ = layer_name;

    if (!discover_table_layout()) {
        std::cerr << "Failed to discover table layout\n";
        return false;
    }

    // 打开 .gdbtable 进行追加写入
    table_fp_ = std::fopen(table_path_.c_str(), "r+b");
    if (!table_fp_) {
        std::cerr << "Failed to open for writing: " << table_path_ << "\n";
        return false;
    }

    // 定位到数据区末尾
    if (std::fseek(table_fp_, static_cast<long>(current_offset_), SEEK_SET) != 0) {
        std::cerr << "Failed to seek to data start\n";
        std::fclose(table_fp_);
        table_fp_ = nullptr;
        return false;
    }

    // 初始化 RowBuffer（使用描述符字段数量和 nullable flags）
    int num_desc_fields = static_cast<int>(field_descriptors_.size());
    row_buffer_.init(num_desc_fields, nullable_flags_);
    if (objectid_descriptor_index_ >= 0) {
        row_buffer_.mark_objectid(objectid_descriptor_index_);
    }

    // 初始化 GeometrySerializer
    geom_serializer_.reset(xorig_, yorig_, xyscale_);

    is_open_ = true;
    return true;
}

// ── 发现数据表布局 ──
// 扫描 .gdb 目录，找到匹配字段名的数据表，解析字段描述符获取坐标系参数
bool GdbTableWriter::discover_table_layout() {
    namespace fs = std::filesystem;

    // 1. 枚举 .gdb 目录中的 .gdbtable 文件
    std::vector<std::string> table_files;
    for (const auto& entry : fs::directory_iterator(gdb_path_)) {
        if (entry.path().extension() == ".gdbtable") {
            table_files.push_back(entry.path().string());
        }
    }
    std::sort(table_files.begin(), table_files.end());

    if (table_files.empty()) {
        std::cerr << "No .gdbtable files found in " << gdb_path_ << "\n";
        return false;
    }

    // 2. 对每个 .gdbtable，解析字段描述符，找到匹配 layer_name 的表
    for (const auto& tpath : table_files) {
        // 跳过系统表（a00000000）
        if (tpath.find("a00000000") != std::string::npos) continue;

        try {
            FILE* fp = std::fopen(tpath.c_str(), "rb");
            if (!fp) continue;

            // 读 header
            uint8_t hdr[48];
            if (std::fread(hdr, 1, 48, fp) < 48) { std::fclose(fp); continue; }

            uint32_t version = *reinterpret_cast<uint32_t*>(hdr);
            uint64_t field_desc_offset = 0;

            if (version == 4) {
                std::memcpy(&field_desc_offset, hdr + 40, 8);
            } else if (version == 3) {
                std::memcpy(&field_desc_offset, hdr + 32, 8);
            } else {
                std::fclose(fp); continue;
            }

            // 读 section header 获取 section_length
            uint8_t sec_hdr[12];
            std::fseek(fp, static_cast<long>(field_desc_offset), SEEK_SET);
            if (std::fread(sec_hdr, 1, 12, fp) < 12) { std::fclose(fp); continue; }

            uint32_t section_length = *reinterpret_cast<uint32_t*>(sec_hdr);
            uint32_t section_version = *reinterpret_cast<uint32_t*>(sec_hdr + 4);
            uint32_t geom_type_full = *reinterpret_cast<uint32_t*>(sec_hdr + 8);
            (void)section_version;

            // 数据起始偏移
            uint64_t data_start = field_desc_offset + 4 + section_length;

            // 提取图层 Z/M 能力（编码在 geom_type_full 的高位）
            bool layer_has_z = (geom_type_full >> 31) != 0;
            bool layer_has_m = ((geom_type_full >> 30) & 1) != 0;

            // 读取字段描述符区
            size_t field_section_size = 4 + section_length;
            std::vector<uint8_t> field_data(field_section_size);
            std::fseek(fp, static_cast<long>(field_desc_offset), SEEK_SET);
            std::fread(field_data.data(), 1, field_section_size, fp);

            // 解析字段
            BinaryReader br(field_data);
            br.skip(12);  // section_length(4) + section_version(4) + geom_type_full(4)
            uint16_t nfields = br.read_u16();

            std::vector<FieldDescriptor> fds;
            std::vector<bool> null_flags;
            int geom_idx = -1;
            double fxorig = 0, fyorig = 0, fxyscale = 0;

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
                    fd.width = static_cast<uint8_t>(br.read_u32());
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
                    bool has_m = (gf & 2) != 0;
                    bool has_z = (gf & 4) != 0;
                    fd.xorig = br.read_f64();
                    fd.yorig = br.read_f64();
                    fd.xyscale = br.read_f64();
                    if (has_m) { br.read_f64(); br.read_f64(); }  // morig, mscale
                    if (has_z) { br.read_f64(); br.read_f64(); }  // zorig, zscale
                    br.read_f64();  // xytolerance
                    if (has_m) br.read_f64();
                    if (has_z) br.read_f64();
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
                }
                null_flags.push_back((fd.flag & 1) != 0);
                fds.push_back(std::move(fd));
            }
        }

        std::fclose(fp);

        if (!parse_ok || geom_idx < 0) continue;

        // 检查字段名是否匹配（排除 ObjectId 和 Geometry 字段）
        // GDAL 创建图层的字段名是用户指定的，与图层名无关
        // 我们只需确认存在 geometry 字段即可（已在上面检查 geom_idx >= 0）

        // 找到匹配的数据表！
        table_path_ = tpath;
        // 对应的 .gdbtablx
        tablx_path_ = tpath.substr(0, tpath.size() - 9) + ".gdbtablx";

        field_descriptors_ = std::move(fds);
        nullable_flags_ = std::move(null_flags);
        geometry_field_index_ = geom_idx;
        xorig_ = fxorig;
        yorig_ = fyorig;
        xyscale_ = fxyscale;
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

        } catch (const std::exception& e) {
            // 解析失败，跳过这个表
            std::cerr << "[writer] Skip table " << tpath << ": " << e.what() << "\n";
            continue;
        }
    }

    std::cerr << "No matching data table found for layer: " << layer_name_ << "\n";
    return false;
}

// ── 行操作 ──

void GdbTableWriter::begin_row() {
    row_buffer_.begin_row();
    in_row_ = true;
}

void GdbTableWriter::set_null(int field_index) {
    row_buffer_.set_field(user_to_descriptor_[field_index]);
    row_buffer_.set_null();
}

void GdbTableWriter::append_i16(int field_index, int16_t value) {
    row_buffer_.set_field(user_to_descriptor_[field_index]);
    row_buffer_.append_i16(value);
}

void GdbTableWriter::append_i32(int field_index, int32_t value) {
    row_buffer_.set_field(user_to_descriptor_[field_index]);
    row_buffer_.append_i32(value);
}

void GdbTableWriter::append_i64(int field_index, int64_t value) {
    row_buffer_.set_field(user_to_descriptor_[field_index]);
    row_buffer_.append_i64(value);
}

void GdbTableWriter::append_f32(int field_index, float value) {
    row_buffer_.set_field(user_to_descriptor_[field_index]);
    row_buffer_.append_f32(value);
}

void GdbTableWriter::append_f64(int field_index, double value) {
    row_buffer_.set_field(user_to_descriptor_[field_index]);
    row_buffer_.append_f64(value);
}

void GdbTableWriter::append_string(int field_index, const std::string& value) {
    row_buffer_.set_field(user_to_descriptor_[field_index]);
    row_buffer_.append_string(value);
}

void GdbTableWriter::append_geometry(int field_index) {
    row_buffer_.set_field(user_to_descriptor_[field_index]);
    row_buffer_.append_geometry(geom_serializer_.blob_data(), geom_serializer_.blob_size());
}

void GdbTableWriter::end_row() {
    row_buffer_.finalize();

    const uint8_t* row_data = row_buffer_.data();
    size_t row_size = row_buffer_.size();

    // 记录当前偏移（这行在 .gdbtable 中的位置）
    // current_offset_ = 已刷盘到文件的总字节数
    // write_buffer_pos_ = 缓冲区中尚未刷盘的字节数
    uint64_t row_offset = current_offset_ + write_buffer_pos_;
    tablx_writer_.add_offset(row_offset);

    // 追加到写缓冲区
    if (write_buffer_pos_ + row_size > write_buffer_.size()) {
        // 缓冲区不够，先刷盘
        internal_flush();
    }

    std::memcpy(write_buffer_.data() + write_buffer_pos_, row_data, row_size);
    write_buffer_pos_ += row_size;

    // 更新统计
    ++row_count_;
    ++buffered_rows_;
    if (row_size > max_row_size_) max_row_size_ = static_cast<uint32_t>(row_size);

    // 检查是否需要刷盘
    if (buffered_rows_ >= kMaxBufferRows || write_buffer_pos_ >= kMaxBufferBytes) {
        internal_flush();
    }

    in_row_ = false;
}

// ── 刷盘 ──

void GdbTableWriter::internal_flush() {
    if (write_buffer_pos_ == 0 || !table_fp_) return;

    std::fwrite(write_buffer_.data(), 1, write_buffer_pos_, table_fp_);
    current_offset_ += write_buffer_pos_;  // 累加已刷盘的字节数
    write_buffer_pos_ = 0;
    buffered_rows_ = 0;
}

void GdbTableWriter::flush() {
    internal_flush();
    if (table_fp_) std::fflush(table_fp_);
}

// ── 关闭 ──

void GdbTableWriter::close() {
    if (!is_open_) return;

    // 1. 刷盘
    internal_flush();

    // 2. 更新 .gdbtable 头部
    update_table_header();

    // 3. 写 .gdbtablx
    tablx_writer_.write(tablx_path_);

    // 4. 关闭文件
    if (table_fp_) {
        std::fclose(table_fp_);
        table_fp_ = nullptr;
    }

    is_open_ = false;
}

// ── 更新 .gdbtable 头部 ──
bool GdbTableWriter::update_table_header() {
    if (!table_fp_) return false;

    // 获取文件大小
    std::fseek(table_fp_, 0, SEEK_END);
    uint64_t file_size = static_cast<uint64_t>(std::ftell(table_fp_));

    // 读 version
    std::fseek(table_fp_, 0, SEEK_SET);
    uint8_t hdr[48];
    std::fread(hdr, 1, 48, table_fp_);
    uint32_t version = *reinterpret_cast<uint32_t*>(hdr);

    if (version == 4) {
        // v4: offset 24 = nfeatures_v4 (uint64), offset 32 = file_size (uint64)
        uint64_t nfeatures = row_count_;
        std::fseek(table_fp_, 24, SEEK_SET);
        std::fwrite(&nfeatures, 8, 1, table_fp_);
        std::fwrite(&file_size, 8, 1, table_fp_);

        // offset 8 = largest_size_record (uint32)
        std::fseek(table_fp_, 8, SEEK_SET);
        std::fwrite(&max_row_size_, 4, 1, table_fp_);
    } else if (version == 3) {
        // v3: offset 4 = nfeatures_v3 (uint32), offset 8 = largest_size_record (uint32)
        // offset 24 = file_size (uint64)
        uint32_t nf32 = static_cast<uint32_t>(row_count_);
        std::fseek(table_fp_, 4, SEEK_SET);
        std::fwrite(&nf32, 4, 1, table_fp_);
        std::fwrite(&max_row_size_, 4, 1, table_fp_);
        std::fseek(table_fp_, 24, SEEK_SET);
        std::fwrite(&file_size, 8, 1, table_fp_);
    }

    std::fflush(table_fp_);
    return true;
}

// ── 更新系统表 ──
// a00000000.gdbtable 中的 Items 表记录了每个图层的要素数
// 简化处理：暂不更新（GDAL 打开时会自行计算）
bool GdbTableWriter::update_system_tables() {
    // TODO: 更新 a00000000.gdbtable 中的图层记录数
    // 对于验证阶段，GDAL 通过 tablx 计算要素数，可以暂不更新
    return true;
}

int GdbTableWriter::find_geometry_field_index() const {
    for (size_t i = 0; i < field_descriptors_.size(); ++i) {
        if (field_descriptors_[i].type == FieldType::Geometry)
            return static_cast<int>(i);
    }
    return -1;
}

}  // namespace writer
}  // namespace explorgdb
