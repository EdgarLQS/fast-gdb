// src/edgar/explorgdb/gdb_table.cpp
// .gdbtable 二进制解析实现 — 最复杂的 GDB 文件解析器
//
// 解析流程概览：
//
// 1. parse_header() — 读取文件头部
//    v3: version(4) + nfeatures(4) + largest_size(4) + role(4) + unknown(8) + file_size(8) + field_desc_offset(8)
//    v4: version(4) + has_deleted(4) + largest_size(4) + role(4) + padding(8) + nfeatures(8) + file_size(8) + field_desc_offset(8)
//
// 2. parse_fields() — 读取字段描述符区
//    Section Header: length(4) + section_version(4) + geom_type_full(4) + nfields(2)
//    per field: name_len(1) + name(utf16) + alias_len(1) + alias(utf16) + type(1) + type-specific payload
//
// 3. parse_records() — 读取要素记录
//    per record: blob_len(4) + nullable_flags(bitmap) + field_values...
//    ObjectId (type=6) 不读取数据，直接返回 FID+1

#include "gdb_table.h"
#include "binary_reader.h"
#include "gdb_tablx.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

namespace explorgdb {

GdbTableParser::GdbTableParser(const std::string& file_path)
    : file_path_(file_path) {}

GdbTableParser::~GdbTableParser() {
    close_file();
}

// ── 按需读取模式：打开文件并读取元数据（对标 GDAL FileGDBTable::Open）──
// 只读 header（48 字节）+ 字段描述区（几 KB），不加载记录数据
// 内存占用从 1.4 GB（10M）降到 < 1 MB
bool GdbTableParser::open() {
    // 如果已打开，先关闭
    if (fd_ >= 0) close_file();

    // 1. 打开文件（不加载到内存）
    fd_ = ::open(file_path_.c_str(), O_RDONLY);
    if (fd_ < 0) {
        std::cerr << "Failed to open: " << file_path_ << "\n";
        return false;
    }

    // 2. 获取文件大小
    struct stat st;
    if (fstat(fd_, &st) < 0) {
        std::cerr << "Failed to stat: " << file_path_ << "\n";
        close_file();
        return false;
    }
    file_size_ = static_cast<size_t>(st.st_size);

    // 2.5 mmap 映射文件（消除 peek_blob 的大量 pread 系统调用）
    if (file_size_ > 0) {
        void* ptr = mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (ptr != MAP_FAILED) {
            mapped_data_ = static_cast<uint8_t*>(ptr);
            // 提示内核：访问模式为顺序读取（减少预读浪费）
            madvise(mapped_data_, file_size_, MADV_SEQUENTIAL);
        }
        // mmap 失败时静默降级到 pread（mapped_data_ 保持 nullptr）
    }

    // 3. 读取 header（最多 48 字节）
    uint8_t header_buf[48];
    size_t header_size = (file_size_ < 48) ? file_size_ : 48;
    if (!read_at(0, header_buf, header_size)) {
        std::cerr << "Failed to read header\n";
        close_file();
        return false;
    }

    // 解析 header（复用现有逻辑）
    BinaryReader br(header_buf, header_size);
    header_.version = br.read_u32();

    if (header_.version == 3) {
        header_.nfeatures_v3 = br.read_u32();
        header_.largest_size_record = br.read_u32();
        header_.unknown_role = br.read_u32();
        header_.unknown_16 = br.read_u32();
        header_.unknown_20 = br.read_u32();
        header_.file_size = br.read_u64();
        header_.field_desc_offset = br.read_u64();
    } else if (header_.version == 4) {
        header_.has_deleted_features = br.read_u32();
        header_.largest_size_record = br.read_u32();
        header_.unknown_role = br.read_u32();
        br.skip(4);  // padding
        header_.nfeatures_v4 = br.read_u64();
        header_.file_size = br.read_u64();
        header_.field_desc_offset = br.read_u64();
    } else {
        std::cerr << "Unknown .gdbtable version: " << header_.version << "\n";
        close_file();
        return false;
    }

    // 加载字段描述符区（通常只有几 KB，位于记录数据之后）
    return ensure_fields_loaded();
}

// ── 确保字段已加载（延迟加载）──
bool GdbTableParser::ensure_fields_loaded() {
    // 如果字段已加载，直接返回
    if (!fields_.empty()) return true;

    // 必须已打开文件
    if (fd_ < 0) {
        std::cerr << "File not open\n";
        return false;
    }

    // 读取字段描述区
    if (header_.field_desc_offset >= file_size_) {
        std::cerr << "field_desc_offset out of bounds\n";
        return false;
    }

    // 先读 14 字节获取 section header
    uint8_t section_header[14];
    if (!read_at(header_.field_desc_offset, section_header, 14)) {
        std::cerr << "Failed to read field section header\n";
        return false;
    }

    BinaryReader br_sec(section_header, 14);
    uint32_t section_length = br_sec.read_u32();
    uint32_t section_version = br_sec.read_u32();
    uint32_t geom_type_full = br_sec.read_u32();
    header_.geom_type_full = geom_type_full;
    (void)section_version;

    // 提取图层 Z/M 能力
    bool layer_has_z = (geom_type_full >> 24) & (1 << 7);
    bool layer_has_m = (geom_type_full >> 24) & (1 << 6);

    // section_length 是字段区第一个 4 字节之后的数据长度
    // 总字段区大小 = 4 (section_length 字段本身) + section_length
    size_t field_section_size = static_cast<size_t>(4) + section_length;

    // 安全检查：确保不超出文件范围
    if (header_.field_desc_offset + field_section_size > file_size_) {
        std::cerr << "Field section extends beyond file: offset=" << header_.field_desc_offset
                  << " size=" << field_section_size << " file_size=" << file_size_ << "\n";
        return false;
    }

    std::vector<uint8_t> field_buf(field_section_size);
    if (!read_at(header_.field_desc_offset, field_buf.data(), field_section_size)) {
        std::cerr << "Failed to read field section (" << field_section_size << " bytes)\n";
        return false;
    }

    // 解析字段（复用现有逻辑）
    BinaryReader br_fields(field_buf);
    br_fields.skip(12);  // section_length(4) + section_version(4) + geom_type_full(4)
    uint16_t nfields = br_fields.read_u16();

    fields_.clear();
    fields_.reserve(nfields);

    for (uint16_t i = 0; i < nfields; ++i) {
        if (!br_fields.can_read(4)) break;
        parse_field_descriptor(br_fields, layer_has_z, layer_has_m);
    }

    // 缓存几何字段位置
    geometry_field_index_ = -1;
    geometry_nullable_bit_index_ = -1;
    int nullable_bit_index = 0;
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i].type == FieldType::Geometry) {
            geometry_field_index_ = static_cast<int>(i);
            geometry_nullable_bit_index_ = nullable_bit_index;
            break;
        }
        if (fields_[i].flag & 1) nullable_bit_index++;
    }

    return true;
}

// ── 关闭文件描述符 ──
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

// ── 底层读取：从指定偏移读取指定字节数（对标 GDAL VSIFReadL）──
// 优先使用 mmap+memcpy（零系统调用），失败时降级到 pread
bool GdbTableParser::read_at(uint64_t offset, void* buffer, size_t size) const {
    if (offset + size > file_size_) return false;

    // 快速路径：mmap 映射成功 → memcpy（无系统调用）
    if (mapped_data_) {
        std::memcpy(buffer, mapped_data_ + offset, size);
        return true;
    }

    // 慢速路径：pread fallback
    if (fd_ < 0) return false;
    ssize_t bytes_read = pread(fd_, buffer, size, static_cast<off_t>(offset));
    return bytes_read == static_cast<ssize_t>(size);
}

// ── 将整个文件加载到内存 ──
bool GdbTableParser::load_file() {
    std::ifstream ifs(file_path_, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) return false;

    auto file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    file_data_.resize(file_size);
    ifs.read(reinterpret_cast<char*>(file_data_.data()), file_size);
    return true;
}

// ── 加载配套的 .gdbtablx 偏移表 ──
bool GdbTableParser::load_tablx(const std::string& tablx_path) {
    GdbTablxParser tablx(tablx_path);
    if (!tablx.parse()) return false;

    feature_offsets_ = tablx.offsets();
    return true;
}

// ── 第一遍: 解析表头部 ──
bool GdbTableParser::parse_header() {
    if (file_data_.empty()) {
        if (!load_file()) return false;
    }

    BinaryReader br(file_data_);
    header_.version = br.read_u32();

    if (header_.version == 3) {
        // v3 (FGDB 9.x): 40 字节头部
        header_.nfeatures_v3 = br.read_u32();
        header_.largest_size_record = br.read_u32();
        header_.unknown_role = br.read_u32();
        header_.unknown_16 = br.read_u32();
        header_.unknown_20 = br.read_u32();
        header_.file_size = br.read_u64();
        header_.field_desc_offset = br.read_u64();
    } else if (header_.version == 4) {
        // v4 (FGDB 10.x / ArcGIS Pro): 48 字节头部
        header_.has_deleted_features = br.read_u32();
        header_.largest_size_record = br.read_u32();
        header_.unknown_role = br.read_u32();
        br.skip(4);  // 4 字节 padding/保留
        header_.nfeatures_v4 = br.read_u64();
        header_.file_size = br.read_u64();
        header_.field_desc_offset = br.read_u64();
    } else {
        std::cerr << "Unknown .gdbtable version: " << header_.version << "\n";
        return false;
    }

    return true;
}

// ── 统计 nullable 字段数 ──
// nullable 由 field.flag 的 bit 0 决定
// 用于计算记录中 nullable_flags 位图的字节数: (count + 7) / 8
int GdbTableParser::nullable_field_count() const {
    int count = 0;
    for (const auto& fd : fields_) {
        if (fd.flag & 1) count++;
    }
    return count;
}

// ── 第二遍: 解析字段描述符区 ──
bool GdbTableParser::parse_fields() {
    if (header_.version == 0) {
        if (!parse_header()) return false;
    }

    BinaryReader br(file_data_);
    br.seek(header_.field_desc_offset);

    // ── Section Header ──
    uint32_t section_length = br.read_u32();
    uint32_t section_version = br.read_u32();
    uint32_t geom_type_full = br.read_u32();
    header_.geom_type_full = geom_type_full;
    (void)section_version; (void)section_length;

    // 从 geom_type_full 的最高字节提取图层的 Z/M 能力
    // bit 7 (0x80): 图层包含 Z 坐标
    // bit 6 (0x40): 图层包含 M 坐标
    bool layer_has_z = (geom_type_full >> 24) & (1 << 7);
    bool layer_has_m = (geom_type_full >> 24) & (1 << 6);

    // 字段数量: 2 字节无符号（不是 VarInt）
    uint16_t nfields = br.read_u16();

    fields_.clear();
    fields_.reserve(nfields);

    // 逐个解析字段描述符
    for (uint16_t i = 0; i < nfields; ++i) {
        if (!br.can_read(4)) break;
        parse_field_descriptor(br, layer_has_z, layer_has_m);
    }

    // 缓存几何字段位置（避免 peek_geometry_blob 每次遍历 schema）
    geometry_field_index_ = -1;
    geometry_nullable_bit_index_ = -1;
    int nullable_bit_index = 0;
    for (size_t i = 0; i < fields_.size(); ++i) {
        if (fields_[i].type == FieldType::Geometry) {
            geometry_field_index_ = static_cast<int>(i);
            geometry_nullable_bit_index_ = nullable_bit_index;
            break;
        }
        if (fields_[i].flag & 1) nullable_bit_index++;
    }

    return true;
}

// ── 解析单个字段描述符 ──
// 结构因字段类型而异，最复杂的是 Geometry 类型
void GdbTableParser::parse_field_descriptor(BinaryReader& br, bool layer_has_z, bool layer_has_m) {

    FieldDescriptor fd;

    // ── 公共前缀: 名称 + 别名 + 类型 ──
    uint8_t name_len = br.read_u8();
    fd.name = br.read_utf16(name_len);

    uint8_t alias_len = br.read_u8();
    fd.alias = br.read_utf16(alias_len);

    fd.type = static_cast<FieldType>(br.read_u8());

    // ── 类型专属数据 ──
    switch (fd.type) {
        // ── 简单固定宽度类型 ──
        case FieldType::Int16:
        case FieldType::Int32:
        case FieldType::Float32:
        case FieldType::Float64:
        case FieldType::DateTime:
        case FieldType::Date:
        case FieldType::Time:
        case FieldType::DateTimeWithOffset:
        case FieldType::Int64: {
            fd.width = br.read_u8();
            fd.flag = br.read_u8();
            uint8_t default_len = br.read_u8();
            // 如果有默认值，跳过它（不解析具体内容）
            if (default_len > 0 && (fd.flag & 4)) {
                for (int i = 0; i < default_len; ++i) br.read_u8();
            }
            break;
        }

        // ── String 类型 ──
        case FieldType::String: {
            fd.width = br.read_u32();    // 最大宽度（VarInt）
            fd.flag = br.read_u8();
            uint64_t default_len = br.read_varuint();
            if (default_len > 0 && (fd.flag & 4)) {
                for (uint64_t i = 0; i < default_len; ++i) br.read_u8();
            }
            break;
        }

        // ── ObjectId 类型 ──
        case FieldType::ObjectId: {
            fd.width = br.read_u8();
            fd.flag = br.read_u8();  // 预期为 2（implicit，不在记录中存储）
            break;
        }

        // ── Geometry 类型（最复杂） ──
        case FieldType::Geometry: {
            fd.wkt = "";
            br.read_u8();  // magic1 = 0（未知用途的标记字节）
            fd.flag = br.read_u8();
            uint16_t wkt_len = br.read_u16();  // WKT 字节长度（不是字符数！）
            fd.wkt = br.read_utf16(wkt_len / 2);  // UTF-16 每字符 2 字节

            // 几何标志字节
            uint8_t geom_flags = br.read_u8();
            bool has_m = (geom_flags & 2) != 0;
            bool has_z = (geom_flags & 4) != 0;

            // 坐标原点和分辨率 — 这 3 个值始终存在，与 has_z/has_m 无关
            fd.xorig = br.read_f64();
            fd.yorig = br.read_f64();
            fd.xyscale = br.read_f64();

            // M 坐标原点和分辨率 — 条件存在
            if (has_m) { fd.morig = br.read_f64(); fd.mscale = br.read_f64(); }

            // Z 坐标原点和分辨率 — 条件存在
            if (has_z) { fd.zorig = br.read_f64(); fd.zscale = br.read_f64(); }

            // 容差
            fd.xytolerance = br.read_f64();
            if (has_m) fd.mtolerance = br.read_f64();
            if (has_z) fd.ztolerance = br.read_f64();

            // 数据范围框（bbox）
            fd.xmin = br.read_f64();
            fd.ymin = br.read_f64();
            fd.xmax = br.read_f64();
            fd.ymax = br.read_f64();

            // zmin/zmax/mmin/mmax — 取决于图层的 Z/M 能力（不是 geom_flags！）
            if (layer_has_z) { fd.zmin = br.read_f64(); fd.zmax = br.read_f64(); }
            if (layer_has_m) { fd.mmin = br.read_f64(); fd.mmax = br.read_f64(); }

            br.read_u8();  // terminator = 0（标记字节）

            // 网格大小数组
            uint32_t nb_grid_sizes = br.read_u32();
            fd.grid_sizes.resize(nb_grid_sizes);
            for (uint32_t i = 0; i < nb_grid_sizes; ++i) {
                fd.grid_sizes[i] = br.read_f64();
            }
            break;
        }

        // ── Binary 和 Raster 类型 ──
        case FieldType::Binary:
        case FieldType::Raster: {
            fd.flag = br.read_u8();
            if (fd.type == FieldType::Raster) {
                uint8_t raster_name_len = br.read_u8();
                br.read_utf16(raster_name_len);
                uint16_t wkt_len = br.read_u16();  // 字节长度
                br.read_utf16(wkt_len / 2);
                br.read_u8();  // magic3
                br.read_u8();  // raster_type
            }
            break;
        }

        // ── UUID 和 XML 类型 ──
        case FieldType::UUID_1:
        case FieldType::UUID_2:
        case FieldType::XML: {
            fd.width = br.read_u8();
            fd.flag = br.read_u8();
            break;
        }

        // ── 未知类型 ──
        default: {
            std::cerr << "Unknown field type " << static_cast<int>(fd.type)
                      << " for field '" << fd.name << "'\n";
            break;
        }
    }

    fields_.push_back(fd);
}

// ── 第三遍: 解析要素记录 ──
bool GdbTableParser::parse_records() {
    if (fields_.empty()) {
        if (!parse_fields()) return false;
    }
    if (feature_offsets_.empty()) {
        std::cerr << "No feature offsets loaded (need .gdbtablx)\n";
        return false;
    }

    records_.clear();

    // 遍历每个 FID，从 .gdbtablx 给出的偏移处读取记录
    for (uint32_t fid = 0; fid < feature_offsets_.size(); ++fid) {
        uint64_t offset = feature_offsets_[fid];
        if (offset == 0) continue;  // null 要素（不存在或已删除）
        if (offset >= file_data_.size()) continue;  // 偏移越界

        FeatureRecord rec;
        rec.fid = fid;
        parse_record_at_offset(offset, rec);
        records_.push_back(rec);
    }

    return true;
}

// ── 按 FID 读取单条记录（按需读取，对标 GDAL SelectRow + GetFieldValue）──
bool GdbTableParser::read_record_by_fid(uint32_t fid, FeatureRecord& rec) {
    // 自动 open（如果尚未打开）
    if (fd_ < 0 && file_data_.empty()) {
        if (!open()) return false;
    }

    // 确保字段已加载（延迟加载）
    if (!ensure_fields_loaded()) return false;

    if (feature_offsets_.empty()) {
        std::cerr << "No feature offsets loaded (need .gdbtablx)\n";
        return false;
    }
    if (fid >= feature_offsets_.size()) return false;
    uint64_t offset = feature_offsets_[fid];
    if (offset == 0) return false;  // null 要素

    try {
        rec.field_values.clear();
        rec.nullable_flags.clear();
        rec.blob_len = 0;
        rec.fid = fid;

        // 按需读取模式（fd_ >= 0）
        if (fd_ >= 0) {
            if (offset >= file_size_) return false;

            // 读取 blob_len（4 字节）
            uint8_t len_buf[4];
            if (!read_at(offset, len_buf, 4)) return false;
            BinaryReader br_len(len_buf, 4);
            uint32_t blob_len = br_len.read_u32();
            rec.blob_len = blob_len;

            if (blob_len == 0) return true;  // 空记录
            if (offset + 4 + blob_len > file_size_) return false;

            // 按需增长 row_buffer_（只增不减）
            if (row_buffer_.size() < blob_len)
                row_buffer_.resize(blob_len);

            // 读取整行到 row_buffer_
            if (!read_at(offset + 4, row_buffer_.data(), blob_len)) return false;

            // 在 row_buffer_ 上解析
            BinaryReader br(row_buffer_.data(), blob_len);

            // ── 读取 nullable 位图 ──
            int n_nullable = nullable_field_count();
            if (n_nullable > 0) {
                int nbytes = (n_nullable + 7) / 8;
                rec.nullable_flags = br.read_bytes(nbytes);
            }

            // ── 逐个读取字段值 ──
            rec.field_values.reserve(fields_.size());  // 预分配避免 realloc
            int nullable_bit_index = 0;
            for (size_t i = 0; i < fields_.size(); ++i) {
                bool is_nullable = (fields_[i].flag & 1) != 0;
                bool is_null = false;

                if (is_nullable) {
                    int byte_idx = nullable_bit_index / 8;
                    int bit_idx = nullable_bit_index % 8;
                    is_null = byte_idx < rec.nullable_flags.size() &&
                              ((rec.nullable_flags[byte_idx] >> bit_idx) & 1);
                    nullable_bit_index++;
                }

                if (is_null) {
                    rec.field_values.push_back(nullptr);
                    continue;
                }

                // 根据字段类型读取值
                switch (fields_[i].type) {
                    case FieldType::ObjectId:
                        rec.field_values.push_back(static_cast<int32_t>(rec.fid + 1));
                        break;
                    case FieldType::Int16:
                        rec.field_values.push_back(static_cast<int32_t>(br.read_i16()));
                        break;
                    case FieldType::Int32:
                        rec.field_values.push_back(br.read_i32());
                        break;
                    case FieldType::Int64:
                        rec.field_values.push_back(br.read_i64());
                        break;
                    case FieldType::Float32:
                        rec.field_values.push_back(static_cast<double>(br.read_f32()));
                        break;
                    case FieldType::Float64:
                        rec.field_values.push_back(br.read_f64());
                        break;
                    case FieldType::String:
                    case FieldType::XML: {
                        uint64_t len = br.read_varuint();
                        if (len > 100000) {
                            std::cerr << "FID " << rec.fid << ": field '" << fields_[i].name
                                      << "' string len " << len << " too large, skipping\n";
                            br.skip(len);
                            rec.field_values.push_back("<string too large>");
                        } else {
                            std::string s(static_cast<size_t>(len), '\0');
                            for (uint64_t j = 0; j < len; ++j) {
                                s[j] = static_cast<char>(br.read_u8());
                            }
                            rec.field_values.push_back(s);
                        }
                        break;
                    }
                    case FieldType::DateTime:
                    case FieldType::Date:
                    case FieldType::Time: {
                        double d = br.read_f64();
                        rec.field_values.push_back(d);
                        break;
                    }
                    case FieldType::DateTimeWithOffset: {
                        double d = br.read_f64();
                        int16_t offset_min = br.read_i16();
                        (void)offset_min;
                        rec.field_values.push_back(d);
                        break;
                    }
                    case FieldType::Binary: {
                        uint64_t len = br.read_varuint();
                        if (len > 1000000) {
                            std::cerr << "FID " << rec.fid << ": field '" << fields_[i].name
                                      << "' binary len " << len << " too large, skipping\n";
                            br.skip(len);
                            rec.field_values.push_back("<binary too large>");
                        } else {
                            rec.field_values.push_back(br.read_bytes(len));
                        }
                        break;
                    }
                    case FieldType::Geometry: {
                        uint64_t geom_len = br.read_varuint();
                        size_t blob_off = br.tell();
                        size_t geom_size = static_cast<size_t>(geom_len);
                        if (blob_off + geom_size > blob_len) {
                            std::cerr << "FID " << rec.fid << ": field '" << fields_[i].name
                                      << "' geom len " << geom_len << " out of bounds\n";
                            rec.field_values.push_back("<geom out of bounds>");
                            br.skip(geom_len);
                            break;
                        }
                        if (geom_size == 0) {
                            rec.field_values.push_back("POINT EMPTY");
                        } else {
                            try {
                                auto decoder = make_geom_decoder(fields_[i]);
                                auto geom = decoder.decode(row_buffer_.data() + blob_off, geom_size);
                                rec.field_values.push_back(geom.wkt);
                            } catch (const std::exception& e) {
                                std::cerr << "FID " << rec.fid << ": field '" << fields_[i].name
                                          << "' geom decode error: " << e.what() << "\n";
                                rec.field_values.push_back("<geom decode error>");
                            }
                        }
                        br.skip(geom_len);
                        break;
                    }
                    case FieldType::UUID_1:
                    case FieldType::UUID_2: {
                        auto bytes = br.read_bytes(16);
                        char uuid_buf[33];
                        for (int j = 0; j < 16; ++j) {
                            snprintf(uuid_buf + j * 2, 3, "%02x", bytes[j]);
                        }
                        uuid_buf[32] = '\0';
                        rec.field_values.push_back(std::string(uuid_buf, 32));
                        break;
                    }
                    default:
                        rec.field_values.push_back(nullptr);
                        break;
                }
            }
        } else {
            // 全量加载模式（file_data_ 已加载，向后兼容）
            if (offset >= file_data_.size()) return false;
            parse_record_at_offset(offset, rec);
        }
    } catch (const std::exception& e) {
        std::cerr << "FID " << fid << " @offset " << offset
                  << ": parse error: " << e.what() << "\n";
        return false;
    }
    return true;
}

// ── 轻量 peek：返回几何字段的 raw blob（按需读取，对标 GDAL SelectRow）──
// 优化：使用 parse_fields() 缓存的 geometry_field_index_ 跳过循环前半部分
// blob_data 指向 row_buffer_ 内部，下次调用会被覆盖
bool GdbTableParser::peek_geometry_blob(uint32_t fid, const uint8_t*& blob_data, size_t& blob_size) {
    // 自动 open（如果尚未打开）
    if (fd_ < 0 && file_data_.empty()) {
        if (!open()) return false;
    }

    // 确保字段已加载（延迟加载）
    if (!ensure_fields_loaded()) return false;

    if (feature_offsets_.empty()) return false;
    if (fid >= feature_offsets_.size()) return false;
    uint64_t offset = feature_offsets_[fid];
    if (offset == 0) return false;

    // 按需读取模式（fd_ >= 0）
    if (fd_ >= 0) {
        if (offset >= file_size_) return false;

        // 读取 blob_len（4 字节）
        uint8_t len_buf[4];
        if (!read_at(offset, len_buf, 4)) return false;
        BinaryReader br_len(len_buf, 4);
        uint32_t blob_len = br_len.read_u32();

        if (blob_len == 0) return false;
        if (offset + 4 + blob_len > file_size_) return false;

        // 按需增长 row_buffer_（只增不减，对标 GDAL m_abyBuffer）
        if (row_buffer_.size() < blob_len)
            row_buffer_.resize(blob_len);

        // 读取整行到 row_buffer_
        if (!read_at(offset + 4, row_buffer_.data(), blob_len)) return false;

        // 在 row_buffer_ 上解析
        BinaryReader br(row_buffer_.data(), blob_len);

        // 读取 nullable 位图
        int n_nullable = nullable_field_count();
        const uint8_t* nullable_ptr = nullptr;
        if (n_nullable > 0) {
            int nbytes = (n_nullable + 7) / 8;
            nullable_ptr = br.data() + br.tell();
            br.skip(nbytes);
        }

        // 使用缓存的几何字段索引
        if (geometry_field_index_ >= 0) {
            // 快速路径：跳过几何字段前的所有字段值
            int nullable_bit_index = geometry_nullable_bit_index_;
            for (int i = 0; i < geometry_field_index_; ++i) {
                bool is_nullable = (fields_[i].flag & 1) != 0;
                bool is_null = false;
                if (is_nullable) {
                    int byte_idx = nullable_bit_index / 8;
                    int bit_idx = nullable_bit_index % 8;
                    is_null = nullable_ptr && ((nullable_ptr[byte_idx] >> bit_idx) & 1);
                    nullable_bit_index++;
                }
                if (is_null) continue;

                // 跳过字段值
                switch (fields_[i].type) {
                    case FieldType::ObjectId: break;
                    case FieldType::Int16: br.skip(2); break;
                    case FieldType::Int32: br.skip(4); break;
                    case FieldType::Int64: br.skip(8); break;
                    case FieldType::Float32: br.skip(4); break;
                    case FieldType::Float64: br.skip(8); break;
                    case FieldType::DateTime: case FieldType::Date: case FieldType::Time:
                    case FieldType::DateTimeWithOffset: br.skip(8); break;
                    case FieldType::String: case FieldType::XML: {
                        uint64_t len = br.read_varuint();
                        br.skip(len);
                        break;
                    }
                    case FieldType::Binary: case FieldType::Raster: {
                        uint64_t len = br.read_varuint();
                        br.skip(len);
                        break;
                    }
                    case FieldType::UUID_1: case FieldType::UUID_2: br.skip(16); break;
                    default: return false;
                }
            }

            // 现在到达几何字段
            bool is_geo_nullable = (fields_[geometry_field_index_].flag & 1) == 0 ||
                (nullable_ptr && ((nullable_ptr[geometry_nullable_bit_index_ / 8] >> (geometry_nullable_bit_index_ % 8)) & 1) == 0);
            if (!is_geo_nullable) return false;

            uint64_t geom_len = br.read_varuint();
            blob_data = br.data() + br.tell();
            blob_size = static_cast<size_t>(geom_len);
            if (blob_data + blob_size > row_buffer_.data() + blob_len) return false;
            return true;
        }

        // 回退路径：无缓存
        int nullable_bit_index = 0;
        for (size_t i = 0; i < fields_.size(); ++i) {
            bool is_nullable = (fields_[i].flag & 1) != 0;
            bool is_null = false;

            if (is_nullable) {
                int byte_idx = nullable_bit_index / 8;
                int bit_idx = nullable_bit_index % 8;
                is_null = nullable_ptr &&
                          ((nullable_ptr[byte_idx] >> bit_idx) & 1);
                nullable_bit_index++;
            }

            if (is_null) continue;

            if (fields_[i].type == FieldType::Geometry) {
                uint64_t geom_len = br.read_varuint();
                blob_data = br.data() + br.tell();
                blob_size = static_cast<size_t>(geom_len);
                if (blob_data + blob_size > row_buffer_.data() + blob_len) return false;
                return true;
            }

            // 跳过非几何字段
            switch (fields_[i].type) {
                case FieldType::ObjectId: br.skip(0); break;
                case FieldType::Int16: br.skip(2); break;
                case FieldType::Int32: br.skip(4); break;
                case FieldType::Int64: br.skip(8); break;
                case FieldType::Float32: br.skip(4); break;
                case FieldType::Float64: br.skip(8); break;
                case FieldType::DateTime: case FieldType::Date: case FieldType::Time:
                case FieldType::DateTimeWithOffset: br.skip(8); break;
                case FieldType::String: case FieldType::XML: {
                    uint64_t len = br.read_varuint();
                    br.skip(len);
                    break;
                }
                case FieldType::Binary: case FieldType::Raster: {
                    uint64_t len = br.read_varuint();
                    br.skip(len);
                    break;
                }
                case FieldType::UUID_1: case FieldType::UUID_2: br.skip(16); break;
                default: return false;
            }
        }
        return false;
    }

    // 全量加载模式（file_data_ 已加载，向后兼容）
    if (offset >= file_data_.size()) return false;

    BinaryReader br(file_data_.data() + offset, file_data_.size() - offset);
    br.read_u32();  // blob_len

    // 读取 nullable 位图
    int n_nullable = nullable_field_count();
    const uint8_t* nullable_ptr = nullptr;
    if (n_nullable > 0) {
        int nbytes = (n_nullable + 7) / 8;
        nullable_ptr = br.data() + br.tell();
        br.skip(nbytes);
    }

    // 使用缓存的几何字段索引
    if (geometry_field_index_ >= 0) {
        // 快速路径：跳过几何字段前的所有字段值
        int nullable_bit_index = geometry_nullable_bit_index_;
        for (int i = 0; i < geometry_field_index_; ++i) {
            bool is_nullable = (fields_[i].flag & 1) != 0;
            bool is_null = false;
            if (is_nullable) {
                int byte_idx = nullable_bit_index / 8;
                int bit_idx = nullable_bit_index % 8;
                is_null = nullable_ptr && ((nullable_ptr[byte_idx] >> bit_idx) & 1);
                nullable_bit_index++;
            }
            if (is_null) continue;

            // 跳过字段值（不判断类型，只根据已知 schema 跳过固定/可变长度）
            switch (fields_[i].type) {
                case FieldType::ObjectId: break;
                case FieldType::Int16: br.skip(2); break;
                case FieldType::Int32: br.skip(4); break;
                case FieldType::Int64: br.skip(8); break;
                case FieldType::Float32: br.skip(4); break;
                case FieldType::Float64: br.skip(8); break;
                case FieldType::DateTime: case FieldType::Date: case FieldType::Time:
                case FieldType::DateTimeWithOffset: br.skip(8); break;
                case FieldType::String: case FieldType::XML: {
                    uint64_t len = br.read_varuint();
                    br.skip(len);
                    break;
                }
                case FieldType::Binary: case FieldType::Raster: {
                    uint64_t len = br.read_varuint();
                    br.skip(len);
                    break;
                }
                case FieldType::UUID_1: case FieldType::UUID_2: br.skip(16); break;
                default: return false;
            }
        }

        // 现在到达几何字段
        bool is_geo_nullable = (fields_[geometry_field_index_].flag & 1) == 0 ||
            (nullable_ptr && ((nullable_ptr[geometry_nullable_bit_index_ / 8] >> (geometry_nullable_bit_index_ % 8)) & 1) == 0);
        if (!is_geo_nullable) return false;

        uint64_t geom_len = br.read_varuint();
        blob_data = br.data() + br.tell();
        blob_size = static_cast<size_t>(geom_len);
        if (blob_data + blob_size > file_data_.data() + file_data_.size()) return false;
        return true;
    }

    // 回退路径：无缓存（几何字段未在 parse_fields 中找到）
    int nullable_bit_index = 0;
    for (size_t i = 0; i < fields_.size(); ++i) {
        bool is_nullable = (fields_[i].flag & 1) != 0;
        bool is_null = false;

        if (is_nullable) {
            int byte_idx = nullable_bit_index / 8;
            int bit_idx = nullable_bit_index % 8;
            is_null = nullable_ptr &&
                      ((nullable_ptr[byte_idx] >> bit_idx) & 1);
            nullable_bit_index++;
        }

        if (is_null) continue;

        if (fields_[i].type == FieldType::Geometry) {
            uint64_t geom_len = br.read_varuint();
            blob_data = br.data() + br.tell();
            blob_size = static_cast<size_t>(geom_len);
            if (blob_data + blob_size > file_data_.data() + file_data_.size()) return false;
            return true;
        }

        // 跳过非几何字段
        switch (fields_[i].type) {
            case FieldType::ObjectId: br.skip(0); break;
            case FieldType::Int16: br.skip(2); break;
            case FieldType::Int32: br.skip(4); break;
            case FieldType::Int64: br.skip(8); break;
            case FieldType::Float32: br.skip(4); break;
            case FieldType::Float64: br.skip(8); break;
            case FieldType::DateTime: case FieldType::Date: case FieldType::Time:
            case FieldType::DateTimeWithOffset: br.skip(8); break;
            case FieldType::String: case FieldType::XML: {
                uint64_t len = br.read_varuint();
                br.skip(len);
                break;
            }
            case FieldType::Binary: case FieldType::Raster: {
                uint64_t len = br.read_varuint();
                br.skip(len);
                break;
            }
            case FieldType::UUID_1: case FieldType::UUID_2: br.skip(16); break;
            default: return false;
        }
    }
    return false;
}

// ── 在指定偏移处解析单条记录 ──
void GdbTableParser::parse_record_at_offset(size_t offset, FeatureRecord& rec) {
    // 创建切片视图（从 offset 开始到文件末尾）
    BinaryReader br(file_data_.data() + offset, file_data_.size() - offset);

    rec.blob_len = br.read_u32();

    // ── 读取 nullable 位图 ──
    // 每个 nullable 字段占 1 bit，按字节对齐
    int n_nullable = nullable_field_count();
    if (n_nullable > 0) {
        int nbytes = (n_nullable + 7) / 8;
        rec.nullable_flags = br.read_bytes(nbytes);
    }

    // ── 逐个读取字段值 ──
    rec.field_values.reserve(fields_.size());  // 预分配避免 realloc
    int nullable_bit_index = 0;
    for (size_t i = 0; i < fields_.size(); ++i) {
        bool is_nullable = (fields_[i].flag & 1) != 0;
        bool is_null = false;

        // 检查当前字段在位图中是否为 null
        if (is_nullable) {
            int byte_idx = nullable_bit_index / 8;
            int bit_idx = nullable_bit_index % 8;
            is_null = byte_idx < rec.nullable_flags.size() &&
                      ((rec.nullable_flags[byte_idx] >> bit_idx) & 1);
            nullable_bit_index++;
        }

        if (is_null) {
            rec.field_values.push_back(nullptr);
            continue;
        }

        // 根据字段类型读取对应宽度和格式的值
        switch (fields_[i].type) {
            case FieldType::ObjectId:
                // ObjectId 不存储在记录数据中，隐式推导为 FID+1
                rec.field_values.push_back(static_cast<int32_t>(rec.fid + 1));
                break;
            case FieldType::Int16:
                rec.field_values.push_back(static_cast<int32_t>(br.read_i16()));
                break;
            case FieldType::Int32:
                rec.field_values.push_back(br.read_i32());
                break;
            case FieldType::Int64:
                rec.field_values.push_back(br.read_i64());
                break;
            case FieldType::Float32:
                rec.field_values.push_back(static_cast<double>(br.read_f32()));
                break;
            case FieldType::Float64:
                rec.field_values.push_back(br.read_f64());
                break;
            case FieldType::String:
            case FieldType::XML: {
                uint64_t len = br.read_varuint();
                if (len > 100000) {
                    std::cerr << "FID " << rec.fid << ": field '" << fields_[i].name
                              << "' string len " << len << " too large, skipping\n";
                    br.skip(len);
                    rec.field_values.push_back("<string too large>");
                } else {
                    std::string s(static_cast<size_t>(len), '\0');
                    for (uint64_t j = 0; j < len; ++j) {
                        s[j] = static_cast<char>(br.read_u8());
                    }
                    rec.field_values.push_back(s);
                }
                break;
            }
            case FieldType::DateTime:
            case FieldType::Date:
            case FieldType::Time: {
                double d = br.read_f64();
                rec.field_values.push_back(d);
                break;
            }
            case FieldType::DateTimeWithOffset: {
                double d = br.read_f64();
                int16_t offset_min = br.read_i16();  // 时区偏移（分钟）
                (void)offset_min;
                rec.field_values.push_back(d);
                break;
            }
            case FieldType::Binary: {
                uint64_t len = br.read_varuint();
                if (len > 1000000) {
                    std::cerr << "FID " << rec.fid << ": field '" << fields_[i].name
                              << "' binary len " << len << " too large, skipping\n";
                    br.skip(len);
                    rec.field_values.push_back("<binary too large>");
                } else {
                    rec.field_values.push_back(br.read_bytes(len));
                }
                break;
            }
            case FieldType::Geometry: {
                uint64_t geom_len = br.read_varuint();
                size_t blob_off = offset + br.tell();
                size_t blob_size = static_cast<size_t>(geom_len);
                if (blob_off + blob_size > file_data_.size()) {
                    std::cerr << "FID " << rec.fid << ": field '" << fields_[i].name
                              << "' geom len " << geom_len << " out of bounds\n";
                    rec.field_values.push_back("<geom out of bounds>");
                    break;
                }
                if (blob_size == 0) {
                    rec.field_values.push_back("POINT EMPTY");
                } else {
                    try {
                        auto decoder = make_geom_decoder(fields_[i]);
                        // 直接调用 decode()，因为 blob 数据已从 geom_len varuint 之后开始
                        auto geom = decoder.decode(
                            file_data_.data() + blob_off, blob_size);
                        rec.field_values.push_back(geom.wkt);
                    } catch (const std::exception& e) {
                        std::cerr << "FID " << rec.fid << ": field '" << fields_[i].name
                                  << "' geom decode error: " << e.what() << "\n";
                        rec.field_values.push_back("<geom decode error>");
                    }
                }
                br.skip(geom_len);
                break;
            }
            case FieldType::UUID_1:
            case FieldType::UUID_2: {
                auto bytes = br.read_bytes(16);
                char uuid_buf[33];
                for (int j = 0; j < 16; ++j) {
                    snprintf(uuid_buf + j * 2, 3, "%02x", bytes[j]);
                }
                uuid_buf[32] = '\0';
                rec.field_values.push_back(std::string(uuid_buf, 32));
                break;
            }
            default:
                rec.field_values.push_back(nullptr);
                break;
        }
    }
}

// ── 创建几何解码器 ──
GdbGeomDecoder GdbTableParser::make_geom_decoder(const FieldDescriptor& fd) const {
    bool layer_has_z = (header_.geom_type_full >> 24) & (1 << 7);
    bool layer_has_m = (header_.geom_type_full >> 24) & (1 << 6);
    return GdbGeomDecoder(
        fd.xorig, fd.yorig, fd.xyscale,
        fd.zorig, fd.zscale,
        fd.morig, fd.mscale,
        layer_has_z, layer_has_m);
}

} // namespace explorgdb
