// src/edgar/explorgdb/explorgdb_types.h
// 公共类型定义 — 所有 GDB 结构探索模块共享的数据类型
//
// 本文件定义了 FileGDB 二进制格式中涉及的所有数据结构和枚举。
// 按逻辑层次分为：
//   1. 字段类型枚举（17 种 GDB 字段类型）
//   2. 字段描述符（一个列的元数据，含几何专用字段）
//   3. 要素记录（一行数据）
//   4. 目录条目（.gdb 目录中的一个文件）
//   5. 表/Tablx 头部（版本相关的文件头结构）
//   6. 索引条目（.gdbindexes 文件中的单个索引描述）

#ifndef EXPLORGDB_TYPES_H
#define EXPLORGDB_TYPES_H

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

namespace explorgdb {

// ────────────────────────────────────────────────
// 1. 字段类型枚举（17 种）
// ────────────────────────────────────────────────
// FileGDB 为每个列分配一个类型 ID，编码在字段描述符中。
// 不同字段类型在记录中的存储方式不同：
//   - 固定宽度类型（Int16/Int32/Float32/Float64/Int64）：直接按宽度读取
//   - 变长类型（String/XML/Binary）：先用 Varuint 读长度，再读内容
//   - 特殊类型（ObjectId/Geometry/Raster）：有专用解析逻辑
enum class FieldType : uint8_t {
    Int16 = 0,              // 2 字节有符号整数
    Int32 = 1,              // 4 字节有符号整数
    Float32 = 2,            // 4 字节 IEEE 754 浮点
    Float64 = 3,            // 8 字节 IEEE 754 浮点
    String = 4,             // 变长 UTF-8 字符串（Varuint 长度前缀）
    DateTime = 5,           // OLE Automation Date（double，自 1899-12-30 起的天数）
    ObjectId = 6,           // 对象 ID（隐式存储，不占记录字节，值 = FID + 1）
    Geometry = 7,           // 几何数据（复杂描述符 + 二进制 blob）
    Binary = 8,             // 变长二进制数据（Varuint 长度前缀）
    Raster = 9,             // 栅格类型
    UUID_1 = 10,            // UUID（16 字节，类型 1）
    UUID_2 = 11,            // UUID（16 字节，类型 2）
    XML = 12,               // XML 文本（Varuint 长度前缀）
    Int64 = 13,             // 8 字节有符号整数
    Date = 14,              // 日期（double 的日期部分）
    Time = 15,              // 时间（double 的时间部分）
    DateTimeWithOffset = 16 // 带时区偏移的日期时间（double + int16 分钟偏移）
};

// 获取字段类型的人类可读名称（用于 CLI 输出和日志）
const char* field_type_name(FieldType t);

// ────────────────────────────────────────────────
// 2. 字段描述符 — 一个列的完整元数据
// ────────────────────────────────────────────────
// 位于 .gdbtable 文件的 field_desc_offset 位置。
// 每个字段包含：名称、别名、类型、宽度、标志，以及几何字段的大量坐标元数据。
//
// flag 位的含义：
//   bit 0 (0x01) — nullable: 该字段可以为 null
//   bit 2 (0x04) — has_default: 该字段有默认值
struct FieldDescriptor {
    std::string name;        // 列名（UTF-16LE 编码）
    std::string alias;       // 列别名（UTF-16LE 编码，可为空）
    FieldType type;          // 字段类型（见 FieldType 枚举）
    uint32_t width = 0;      // 字段宽度（String 类型表示最大字符数，数值类型表示字节数）
    uint8_t flag = 0;        // 字段标志位（见上方说明）
    std::string default_value; // 默认值（仅当 flag & 0x04 时存在）

    // ── 几何字段专用属性 ──
    // 当 type == Geometry 时，字段描述符包含大量坐标系和范围元数据。
    // 非几何字段时这些字段保持默认值 0。

    std::string wkt;           // 几何类型 WKT 字符串（如 "POLYGON"、"LINESTRING"）
    double xorig = 0, yorig = 0;  // X/Y 坐标原点（坐标变换基准点）
    double xyscale = 0;        // X/Y 分辨率（网格间距，通常 ~1e7 ~ 1e8）
    double xytolerance = 0;    // X/Y 容差（坐标 snapping 精度）

    // Z 坐标元数据（仅当图层包含 Z 时填充）
    double zorig = 0, zscale = 0, ztolerance = 0;

    // M 坐标元数据（仅当图层包含 M 时填充）
    double morig = 0, mscale = 0, mtolerance = 0;

    // 数据范围框（bounding box）
    double xmin = 0, ymin = 0, xmax = 0, ymax = 0;
    double zmin = 0, zmax = 0;  // 仅当图层有 Z
    double mmin = 0, mmax = 0;  // 仅当图层有 M

    // 网格大小数组（用于空间索引的网格划分）
    std::vector<double> grid_sizes;
};

// ────────────────────────────────────────────────
// 3. 要素记录 — 一行数据
// ────────────────────────────────────────────────
// 每个记录包含：要素 ID、blob 总长度、nullable 位图、各字段值。
// field_values 的顺序与字段描述符完全一致。
//
// FieldValue 使用 std::variant 表示可能的值类型：
//   nullptr_t         — null 值（当 nullable flag 对应位为 1 时）
//   int32_t           — Int16/Int32 值
//   int64_t           — Int64 值
//   double            — Float32/Float64/DateTime/Date/Time 值
//   DateTimeOffsetValue — DateTimeWithOffset 的时间值和 UTC 偏移分钟
//   std::string       — String/XML/UUID 值
//   std::vector<uint8_t> — Binary/Geometry 值
struct DateTimeOffsetValue {
    double date = 0.0;
    int16_t offset_minutes = 0;
};

using FieldValue = std::variant<std::nullptr_t, int32_t, int64_t, double,
                                DateTimeOffsetValue, std::string,
                                std::vector<uint8_t>>;

struct FeatureRecord {
    uint32_t fid = 0;                          // 要素 ID（0 起始索引）
    uint32_t blob_len = 0;                     // 记录 blob 总长度（4 字节前缀）
    std::vector<uint8_t> nullable_flags;       // nullable 位图（1 bit per nullable 字段）
    std::vector<FieldValue> field_values;      // 各字段值（与字段描述符顺序一致）
};

// ────────────────────────────────────────────────
// 4. 几何 Blob（解码后）— 预留结构
// ────────────────────────────────────────────────
// 当前解析器跳过几何 blob 内容（仅读取长度），
// 此结构为未来几何解码预留。
struct GeometryBlob {
    uint64_t geom_len = 0;
    uint64_t geom_type = 0;
    // 简单点几何的坐标
    std::vector<std::pair<double, double>> points;
    // 多部件几何（折线、多边形、多点、multipatch）
    struct Part {
        std::vector<double> x;
        std::vector<double> y;
        std::vector<double> z;  // 无 Z 时为空
        std::vector<double> m;  // 无 M 时为空
    };
    std::vector<Part> parts;
};

// ────────────────────────────────────────────────
// 5. 目录条目 — .gdb 目录中的一个文件
// ────────────────────────────────────────────────
// 用于 GdbCatalog 对 .gdb 目录的文件枚举结果。
// filename 格式为 aXXXXXXXX.<ext>，其中 XXXXXXXX 是 8 位十六进制数字。
// numeric_id 从文件名中提取（如 a00000001.gdbtable → id=1）。
struct CatalogEntry {
    std::string filename;      // 文件名（如 "a00000001.gdbtable"）
    std::string extension;     // 扩展名（".gdbtable"、".gdbtablx"、".spx"、".atx"、".gdbindexes"）
    uint32_t numeric_id = 0;   // 从 aXXXXXXXX 提取的数字 ID
    uint64_t file_size = 0;    // 文件大小（字节）
};

// ────────────────────────────────────────────────
// 6. GDB 目录头部 — 8 字节 magic 文件
// ────────────────────────────────────────────────
// .gdb 目录中的 "gdb" 文件，包含：
//   偏移 0: uint32 version — 目录版本（预期 = 5）
//   偏移 4: uint32 magic   — 魔数（预期 = 0xDEADBEEF，小端读取为 0xEFBEADDE）
struct GdbDirectoryHeader {
    uint32_t version = 0;    // 目录版本号，预期为 5
    uint32_t magic = 0;      // 魔数，预期为 0xDEADBEEF
};

// ────────────────────────────────────────────────
// 7. GDB 时间戳文件 — 384 字节
// ────────────────────────────────────────────────
// .gdb 目录中的 "timestamps" 文件，384 字节原始数据。
// 具体结构尚未完全解析，目前保留原始字节。
struct GdbTimestamps {
    uint8_t raw[384] = {0};
};

// ────────────────────────────────────────────────
// 8. 表头部 — .gdbtable 文件头（版本相关）
// ────────────────────────────────────────────────
// 版本 3（FGDB 9.x）和版本 4（FGDB 10.x / ArcGIS Pro）结构不同：
//
// v3 (40 字节头部):
//   [0]  version(4) = 3
//   [4]  nfeatures_v3(4) — 要素数量
//   [8]  largest_size_record(4) — 最大记录大小
//   [12] unknown_role(4) — 始终为 5
//   [16] unknown_16(4)
//   [20] unknown_20(4)
//   [24] file_size(8)
//   [32] field_desc_offset(8) — 字段描述符区的偏移
//
// v4 (48 字节头部):
//   [0]  version(4) = 4
//   [4]  has_deleted_features(4) — 是否包含已删除要素
//   [8]  largest_size_record(4)
//   [12] unknown_role(4) — 始终为 5
//   [16] padding(4) — 保留/对齐
//   [20] padding(4) — 保留/对齐
//   [24] nfeatures_v4(8) — 要素数量（64-bit）
//   [32] file_size(8)
//   [40] field_desc_offset(8)
struct TableHeader {
    uint32_t version = 0;         // 3 = FGDB 9.x, 4 = FGDB 10.x/ArcGIS Pro

    // v3 特有字段
    uint32_t nfeatures_v3 = 0;

    // v4 特有字段
    uint32_t has_deleted_features = 0;
    uint64_t nfeatures_v4 = 0;

    // 共有字段
    uint32_t largest_size_record = 0;
    uint32_t unknown_role = 0;    // 始终为 5，含义未知
    uint32_t unknown_16 = 0;
    uint32_t unknown_20 = 0;
    uint64_t file_size = 0;
    uint64_t field_desc_offset = 0;

    // Section Header 中的几何类型字段（parse_fields 时读取）
    uint32_t geom_type_full = 0;
};

// ────────────────────────────────────────────────
// 9. Tablx 头部 — .gdbtablx 文件头（版本相关）
// ────────────────────────────────────────────────
// 版本 3（FGDB 9.x，16 字节头部）:
//   [0]  version(4) = 3
//   [4]  n1024blocks_v3(4) — 1024-要素块的数量
//   [8]  nfeatures_v3(4) — 要素数量
//   [12] size_tablx_offsets(4) — 偏移条目宽度（4/5/6）
//
// 版本 4（FGDB 10.x，24 字节头部）:
//   [0]  version(4) = 4
//   [4]  unknown_v4(4)
//   [8]  size_tablx_offsets(4)
//   [12] padding(8) — 保留/对齐
//   [20] ... (后续变长区)
struct TablxHeader {
    uint32_t version = 0;

    // v3 特有
    uint32_t n1024blocks_v3 = 0;
    uint32_t nfeatures_v3 = 0;

    // v4 特有
    uint32_t unknown_v4 = 0;

    // 共有
    uint32_t size_tablx_offsets = 0;  // 偏移条目宽度: 4、5 或 6 字节
    uint64_t nfeatures_v4 = 0;        // v4 要素数量
    uint32_t sizeof_varying_section = 0;
};

// ────────────────────────────────────────────────
// 10. 索引条目 — .gdbindexes 文件中的单个索引描述
// ────────────────────────────────────────────────
// 每个索引条目包含：名称、3~4 个魔数值、列名。
// 魔数元组 (magic2, magic3) 决定索引类型：
//   (2, 0)    — 常见索引类型
//   (4, 0)    — 另一种索引类型
//   (16, 65535) — 第三种索引类型
// 已知魔数时有额外 magic4 字段，未知时 magic2 兼作列名长度。
struct IndexEntry {
    std::string name;       // 索引名称（UTF-16LE 编码）
    uint16_t magic1 = 0;    // 魔数 1（用途不明）
    int32_t magic2 = 0;     // 魔数 2（决定索引类型）
    uint16_t magic3 = 0;    // 魔数 3（与 magic2 组合判断类型）
    int32_t magic4 = 0;     // 魔数 4（仅在已知魔数元组时存在）
    std::string column_name; // 索引列名（UTF-16LE 编码）
    uint16_t magic5 = 0;    // 魔数 5（条目末尾）
};

// ────────────────────────────────────────────────
// 11. B+ 树索引 Trailer — .spx/.atx 共享的 22 字节尾部
// ────────────────────────────────────────────────
// 文件末尾 22 字节，描述整个 B+ 树的结构。
// .spx 和 .atx 使用相同的 B+ 树页面格式，区别仅在于索引值的含义。
struct BPlusTreeTrailer {
    uint8_t  value_size = 0;        // 每个索引值的字节数（.spx 总是 8）
    bool     is_string = false;     // flags & 0x20，字符串索引
    bool     is_numeric = false;    // flags & 0x40，数值索引
    uint32_t magic1 = 0;            // 必须为 1
    uint32_t tree_depth = 0;        // B+ 树深度（1=单页，最大 4）
    uint32_t total_value_count = 0; // 索引中的总条目数
};

// ────────────────────────────────────────────────
// 12. 空间索引条目 — .spx 文件中解析出的单个条目
// ────────────────────────────────────────────────
// 64 位值编码：
//   Bit 63-62: grid_level (0=最细格网, 1=中, 2=最粗)
//   Bit 61-31: cell_x (格网单元 X 坐标，31 位)
//   Bit 30-0:  cell_y (格网单元 Y 坐标，31 位)
struct SpatialIndexEntry {
    uint32_t fid = 0;            // 要素 ID (1-based)
    uint8_t  grid_level = 0;     // 格网层级
    uint32_t cell_x = 0;         // 格网单元 X
    uint32_t cell_y = 0;         // 格网单元 Y
    uint64_t raw_value = 0;      // 原始 64 位索引值
};

// ────────────────────────────────────────────────
// 13. 属性索引条目 — .atx 文件中解析出的单个条目
// ────────────────────────────────────────────────
// 值类型由 Trailer 的 value_size + is_string 决定：
//   value_size=2 → INT16, 4 → INT32/FLOAT32, 8 → INT64/FLOAT64/DATE
//   value_size>8 → STRING (UTF16-LE, 空格填充)
//   value_size=38 → GUID (ASCII UUID)
struct AttributeIndexEntry {
    uint32_t fid = 0;            // 要素 ID (1-based)
    std::string string_value;    // 字符串值（非字符串类型为空）
    double numeric_value = 0;    // 数值（NaN 表示字符串类型）
};

// ────────────────────────────────────────────────
// 14. 零拷贝字段引用 — 顺序扫描模式使用
// ────────────────────────────────────────────────
// FieldRef 直接指向 mmap 内存中的原始字节，不做拷贝。
// 定长类型（Int16/Int32/Int64/Float32/Float64）：data 指向值起始位置
// 变长类型（String/Binary）：data 指向数据内容起始位置（varuint 长度之后）
// ObjectId：无原始字节，is_null=false，value 由 fid+1 隐式给出
//
// 生命周期：FieldRef 仅在 sequential_scan 回调内有效，
// 回调返回后 mmap 内存可能被 unmap，指针失效。
struct FieldRef {
    FieldType type = FieldType::ObjectId;
    const uint8_t* data = nullptr;  // 指向 mmap 内存中的原始字节
    size_t byte_len = 0;            // 原始字节长度（定长类型=类型宽度，变长=内容长度）
    bool is_null = false;           // 是否为 NULL
    int32_t implicit_value = 0;     // ObjectId 的隐式值（fid+1）

    // ── 零拷贝解码方法 ──
    // 直接 reinterpret_cast mmap 中的原始字节（小端，与 x86/ARM 一致）

    int32_t as_i16() const {
        if (is_null || byte_len < 2) return 0;
        int16_t v;
        std::memcpy(&v, data, 2);
        return static_cast<int32_t>(v);
    }

    int32_t as_i32() const {
        if (is_null || byte_len < 4) return implicit_value;
        int32_t v;
        std::memcpy(&v, data, 4);
        return v;
    }

    int64_t as_i64() const {
        if (is_null || byte_len < 8) return static_cast<int64_t>(implicit_value);
        int64_t v;
        std::memcpy(&v, data, 8);
        return v;
    }

    float as_f32() const {
        if (is_null || byte_len < 4) return 0.0f;
        float v;
        std::memcpy(&v, data, 4);
        return v;
    }

    double as_f64() const {
        if (is_null || byte_len < 8) return 0.0;
        double v;
        std::memcpy(&v, data, 8);
        return v;
    }

    int16_t as_datetime_offset_minutes() const {
        if (is_null || byte_len < 10) return 0;
        int16_t value;
        std::memcpy(&value, data + 8, sizeof(value));
        return value;
    }

    // 返回 string_view，零拷贝（指向 mmap 内存）
    std::string_view as_string_view() const {
        if (is_null || !data || byte_len == 0) return {};
        return std::string_view(reinterpret_cast<const char*>(data), byte_len);
    }
};

} // namespace explorgdb

#endif // EXPLORGDB_TYPES_H
