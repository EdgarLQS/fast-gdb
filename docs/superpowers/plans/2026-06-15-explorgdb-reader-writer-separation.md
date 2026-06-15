# explorgdb 读写分离重构 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 `src/edgar/explorgdb/` 拆分为 `common/`、`reader/`、`writer/` 三个纯 C++ 目录，writer 去掉 GDAL 依赖，全部 112 个测试通过。

**Architecture:** 三个静态库 `explorgdb_common_lib`（基础设施）、`explorgdb_reader_lib`（读取器）、`explorgdb_writer_lib`（写入器）。reader 和 writer 都只依赖 common，互不依赖。全部纯 C++17，无 GDAL 依赖。

**Tech Stack:** C++17, CMake 3.15+, Google Test, GDAL 3.9.3（仅测试用）

**设计文档:** `docs/superpowers/specs/2026-06-15-explorgdb-reader-writer-separation-design.md`

---

## 当前结构

```
src/edgar/explorgdb/
├── [根目录 24 文件]      ← 混合了读+公共
│   ├── binary_reader.h/cpp, explorgdb_types.h/cpp, varint.h/cpp
│   ├── utf16.h/cpp, ole_date.h/cpp
│   ├── gdb_table.h/cpp, gdb_tablx.h/cpp, gdb_catalog.h/cpp
│   ├── gdb_geometry.h/cpp, gdb_indexes.h/cpp
│   ├── gdb_attribute_index.h/cpp, gdb_spatial_index.h/cpp
│   ├── explorgdb_cli.cpp, QUERY_FLOW.md, README.md
│   └── writer/ [6 文件]  ← 已分离的写
└── tests/edgar/explorgdb/ [17 文件平铺]
```

## 目标结构

```
src/edgar/explorgdb/
├── common/   [5 对 .h/.cpp]  ← binary_reader, explorgdb_types, varint, utf16, ole_date
├── reader/   [7 对 + cli + docs]  ← gdb_table/tablx/catalog/geometry/indexes/attribute_index/spatial_index
└── writer/   [3 对 + 1 纯 .h]  ← gdb_table_writer, geometry_serializer, row_buffer, tablx_writer

tests/edgar/explorgdb/
├── reader/   [14 测试文件]
├── writer/   [1 测试文件]
├── generate_large_gdb.cpp
└── test_fixture_explorgdb.h
```

---

### Task 1: 创建目录结构

**Files:**
- Create: `src/edgar/explorgdb/common/` (目录)
- Create: `src/edgar/explorgdb/reader/` (目录)
- Create: `tests/edgar/explorgdb/reader/` (目录)
- Create: `tests/edgar/explorgdb/writer/` (目录)

- [ ] **Step 1: 创建目录**

```bash
cd /Users/edgarlqs/Downloads/daydaydaywork/dailyWork/convert/gdal/fast_gdb
mkdir -p src/edgar/explorgdb/common
mkdir -p src/edgar/explorgdb/reader
mkdir -p tests/edgar/explorgdb/reader
mkdir -p tests/edgar/explorgdb/writer
```

- [ ] **Step 2: 验证目录已创建**

Run: `find src/edgar/explorgdb -type d | sort`
Expected: 出现 `common/`, `reader/`, `writer/` 三个子目录

---

### Task 2: 迁移公共文件到 common/

**Files:**
- Move: `src/edgar/explorgdb/binary_reader.h` → `src/edgar/explorgdb/common/binary_reader.h`
- Move: `src/edgar/explorgdb/binary_reader.cpp` → `src/edgar/explorgdb/common/binary_reader.cpp`
- Move: `src/edgar/explorgdb/explorgdb_types.h` → `src/edgar/explorgdb/common/explorgdb_types.h`
- Move: `src/edgar/explorgdb/explorgdb_types.cpp` → `src/edgar/explorgdb/common/explorgdb_types.cpp`
- Move: `src/edgar/explorgdb/varint.h` → `src/edgar/explorgdb/common/varint.h`
- Move: `src/edgar/explorgdb/varint.cpp` → `src/edgar/explorgdb/common/varint.cpp`
- Move: `src/edgar/explorgdb/utf16.h` → `src/edgar/explorgdb/common/utf16.h`
- Move: `src/edgar/explorgdb/utf16.cpp` → `src/edgar/explorgdb/common/utf16.cpp`
- Move: `src/edgar/explorgdb/ole_date.h` → `src/edgar/explorgdb/common/ole_date.h`
- Move: `src/edgar/explorgdb/ole_date.cpp` → `src/edgar/explorgdb/common/ole_date.cpp`

- [ ] **Step 1: git mv 公共文件**

```bash
cd /Users/edgarlqs/Downloads/daydaydaywork/dailyWork/convert/gdal/fast_gdb
git mv src/edgar/explorgdb/binary_reader.h src/edgar/explorgdb/common/
git mv src/edgar/explorgdb/binary_reader.cpp src/edgar/explorgdb/common/
git mv src/edgar/explorgdb/explorgdb_types.h src/edgar/explorgdb/common/
git mv src/edgar/explorgdb/explorgdb_types.cpp src/edgar/explorgdb/common/
git mv src/edgar/explorgdb/varint.h src/edgar/explorgdb/common/
git mv src/edgar/explorgdb/varint.cpp src/edgar/explorgdb/common/
git mv src/edgar/explorgdb/utf16.h src/edgar/explorgdb/common/
git mv src/edgar/explorgdb/utf16.cpp src/edgar/explorgdb/common/
git mv src/edgar/explorgdb/ole_date.h src/edgar/explorgdb/common/
git mv src/edgar/explorgdb/ole_date.cpp src/edgar/explorgdb/common/
```

- [ ] **Step 2: 验证文件已移动**

Run: `ls src/edgar/explorgdb/common/`
Expected: 10 个文件（5 对 .h/.cpp）

---

### Task 3: 迁移读取器文件到 reader/

**Files:**
- Move: 7 对 gdb_*.h/cpp + explorgdb_cli.cpp + 2 个文档

- [ ] **Step 1: git mv 读取器文件**

```bash
cd /Users/edgarlqs/Downloads/daydaydaywork/dailyWork/convert/gdal/fast_gdb
git mv src/edgar/explorgdb/gdb_table.h src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/gdb_table.cpp src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/gdb_tablx.h src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/gdb_tablx.cpp src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/gdb_catalog.h src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/gdb_catalog.cpp src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/gdb_geometry.h src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/gdb_geometry.cpp src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/gdb_indexes.h src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/gdb_indexes.cpp src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/gdb_attribute_index.h src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/gdb_attribute_index.cpp src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/gdb_spatial_index.h src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/gdb_spatial_index.cpp src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/explorgdb_cli.cpp src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/QUERY_FLOW.md src/edgar/explorgdb/reader/
git mv src/edgar/explorgdb/README.md src/edgar/explorgdb/reader/
```

- [ ] **Step 2: 验证根目录已清空**

Run: `ls src/edgar/explorgdb/*.h src/edgar/explorgdb/*.cpp src/edgar/explorgdb/*.md 2>/dev/null`
Expected: 无输出（根目录只剩下 common/、reader/、writer/ 三个子目录）

---

### Task 4: 更新 common/ 内部 #include

**Files:**
- Modify: `src/edgar/explorgdb/common/binary_reader.cpp`
- Modify: `src/edgar/explorgdb/common/explorgdb_types.cpp`
- Modify: `src/edgar/explorgdb/common/varint.cpp`
- Modify: `src/edgar/explorgdb/common/utf16.cpp`
- Modify: `src/edgar/explorgdb/common/ole_date.cpp`

common/ 文件互相引用时使用短名称（同目录），不需要修改。验证当前 include 已经是短名称即可。

- [ ] **Step 1: 验证 common/ 文件的 include 无需修改**

Run: `grep '#include "' src/edgar/explorgdb/common/*.cpp src/edgar/explorgdb/common/*.h | grep -v '<'`
Expected: 所有项目内 include 都是短名称（如 `"utf16.h"`, `"binary_reader.h"`, `"explorgdb_types.h"`, `"ole_date.h"`），无需修改。

输出应该是：
```
binary_reader.cpp:#include "binary_reader.h"
binary_reader.cpp:#include "utf16.h"
explorgdb_types.cpp:#include "explorgdb_types.h"
varint.cpp:#include "varint.h"
utf16.cpp:#include "utf16.h"
ole_date.cpp:#include "ole_date.h"
```

这些都是同目录内的短名称引用，CMake 的 `target_include_directories` 会自动解析。无需修改。

---

### Task 5: 更新 reader/ 内部 #include

**Files:**
- Modify: `src/edgar/explorgdb/reader/gdb_table.h` — `"explorgdb_types.h"`, `"binary_reader.h"`, `"gdb_geometry.h"`
- Modify: `src/edgar/explorgdb/reader/gdb_table.cpp` — `"gdb_table.h"`, `"binary_reader.h"`, `"gdb_tablx.h"`
- Modify: `src/edgar/explorgdb/reader/gdb_tablx.h` — `"explorgdb_types.h"`
- Modify: `src/edgar/explorgdb/reader/gdb_tablx.cpp` — `"gdb_tablx.h"`, `"binary_reader.h"`
- Modify: `src/edgar/explorgdb/reader/gdb_catalog.h` — `"explorgdb_types.h"`
- Modify: `src/edgar/explorgdb/reader/gdb_catalog.cpp` — `"gdb_catalog.h"`, `"binary_reader.h"`
- Modify: `src/edgar/explorgdb/reader/gdb_geometry.cpp` — `"gdb_geometry.h"`
- Modify: `src/edgar/explorgdb/reader/gdb_indexes.h` — `"explorgdb_types.h"`
- Modify: `src/edgar/explorgdb/reader/gdb_indexes.cpp` — `"gdb_indexes.h"`, `"binary_reader.h"`
- Modify: `src/edgar/explorgdb/reader/gdb_attribute_index.h` — `"explorgdb_types.h"`
- Modify: `src/edgar/explorgdb/reader/gdb_attribute_index.cpp` — `"gdb_attribute_index.h"`, `"binary_reader.h"`
- Modify: `src/edgar/explorgdb/reader/gdb_spatial_index.h` — `"explorgdb_types.h"`
- Modify: `src/edgar/explorgdb/reader/gdb_spatial_index.cpp` — `"gdb_spatial_index.h"`, `"binary_reader.h"`
- Modify: `src/edgar/explorgdb/reader/explorgdb_cli.cpp` — 6 个 gdb_*.h include

这些文件当前已经全部使用短名称（如 `#include "binary_reader.h"`），移动后通过 CMake PUBLIC include path 自动解析。

- [ ] **Step 1: 验证 reader/ 文件的 include 无需修改**

Run: `grep '#include "' src/edgar/explorgdb/reader/*.cpp src/edgar/explorgdb/reader/*.h | grep -v '<'`
Expected: 所有项目内 include 都是短名称。`"binary_reader.h"`, `"explorgdb_types.h"` 等通过 CMake 从 common/ 解析；`"gdb_table.h"` 等通过同目录或 CMake 从 reader/ 解析。无需修改。

---

### Task 6: 合并 varint + 更新 writer/ #include

**Files:**
- Modify: `src/edgar/explorgdb/common/varint.h` — 添加零分配编码函数（来自 varint_encoder.h）
- Modify: `src/edgar/explorgdb/writer/geometry_serializer.h` — 替换内联编码为 common/varint.h 函数
- Modify: `src/edgar/explorgdb/writer/row_buffer.h` — 替换内联编码为 common/varint.h 函数
- Modify: `src/edgar/explorgdb/writer/gdb_table_writer.h` — 更新 include 路径
- Modify: `src/edgar/explorgdb/writer/gdb_table_writer.cpp` — 更新 include 路径，移除 varint_encoder.h
- Delete: `src/edgar/explorgdb/writer/varint_encoder.h`

- [ ] **Step 1: 扩展 common/varint.h — 添加零分配编码函数**

在 `src/edgar/explorgdb/common/varint.h` 末尾（`} // namespace explorgdb` 之前）添加：

```cpp
// ── 零分配编码函数（直接写入 uint8_t* 缓冲区）──
// 用于性能热路径，避免 std::vector 堆分配。
// 所有函数返回写入的字节数。调用者需确保 dst 有足够空间（最大 10 字节）。

// 最大 varint 编码长度（64-bit 值最多 10 字节）
static constexpr size_t kMaxVarintLen = 10;

// 将无符号整数编码写入 dst，返回写入字节数
inline size_t encode_varuint_to(uint8_t* dst, uint64_t value) {
    size_t n = 0;
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value != 0) byte |= 0x80;
        dst[n++] = byte;
    } while (value != 0);
    return n;
}

// 将无符号整数编码写入 dst+offset，返回写入字节数
inline size_t encode_varuint_at(uint8_t* dst, size_t offset, uint64_t value) {
    return encode_varuint_to(dst + offset, value);
}

// 计算 varuint 编码后的字节长度（不实际编码）
inline size_t varuint_encoded_len(uint64_t value) {
    size_t n = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++n;
    }
    return n;
}

// 将有符号整数编码写入 dst，返回写入字节数
// 编码规则：首字节 bit6=符号，bit7=延续，低6bit=数据
inline size_t encode_varint_to(uint8_t* dst, int64_t value) {
    uint64_t sign_bit = 0;
    uint64_t abs_val;
    if (value < 0) {
        sign_bit = 0x40;
        abs_val = static_cast<uint64_t>(-value);
    } else {
        abs_val = static_cast<uint64_t>(value);
    }

    uint64_t first_data = abs_val & 0x3F;
    abs_val >>= 6;

    size_t n = 0;
    if (abs_val == 0) {
        dst[n++] = static_cast<uint8_t>(first_data | sign_bit);
        return n;
    }

    dst[n++] = static_cast<uint8_t>(first_data | sign_bit | 0x80);

    while (abs_val != 0) {
        uint8_t byte = static_cast<uint8_t>(abs_val & 0x7F);
        abs_val >>= 7;
        if (abs_val != 0) byte |= 0x80;
        dst[n++] = byte;
    }
    return n;
}

// 将小端 16-bit 整数写入 dst
inline void write_u16_le(uint8_t* dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFF);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

// 将小端 32-bit 整数写入 dst
inline void write_u32_le(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFF);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

// 将小端 64-bit 浮点（double）写入 dst
inline void write_f64_le(uint8_t* dst, double value) {
    uint64_t bits;
    static_assert(sizeof(double) == sizeof(uint64_t), "double must be 64 bits");
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<uint8_t>(bits & 0xFF);
        bits >>= 8;
    }
}
```

同时在 varint.h 的 `#include` 区添加 `<cstring>`（write_f64_le 需要 std::memcpy）。

- [ ] **Step 2: 更新 writer/geometry_serializer.h**

将所有内联 `encode_varuint` 和 `encode_signed_varint` 替换为调用 common/varint.h 的函数。

在文件头部添加：
```cpp
#include "../common/varint.h"
```

删除文件末尾的两个 private 静态方法：
- `encode_varuint` — 替换为 `explorgdb::encode_varuint_to`
- `encode_signed_varint` — 替换为 `explorgdb::encode_varint_to`

在文件体中，将所有调用替换：
- `encode_varuint(tmp_ + pos, ...)` → `encode_varuint_to(tmp_ + pos, ...)`
- `encode_signed_varint(tmp_ + pos, ...)` → `encode_varint_to(tmp_ + pos, ...)`

注意：这些方法原来在 `explorgdb::writer` 命名空间内。新函数在 `explorgdb` 命名空间。由于 writer 代码在 `namespace writer {}` 内，可以直接调用外层命名空间的函数（C++ 名称查找规则），无需加 `explorgdb::` 前缀。

- [ ] **Step 3: 更新 writer/row_buffer.h**

在文件头部添加：
```cpp
#include "../common/varint.h"
```

删除 private 静态方法 `encode_varuint`。

将所有 `encode_varuint(varbuf, ...)` 调用替换为 `encode_varuint_to(varbuf, ...)`。

- [ ] **Step 4: 更新 writer/gdb_table_writer.h**

将：
```cpp
#include "../explorgdb_types.h"
```
改为：
```cpp
#include "../common/explorgdb_types.h"
```

- [ ] **Step 5: 更新 writer/gdb_table_writer.cpp**

将：
```cpp
#include "varint_encoder.h"
#include "../binary_reader.h"
#include "../gdb_catalog.h"
```
改为：
```cpp
#include "../common/binary_reader.h"
```

（删除 `varint_encoder.h` 和未使用的 `gdb_catalog.h` include）

- [ ] **Step 6: 删除 writer/varint_encoder.h**

```bash
git rm src/edgar/explorgdb/writer/varint_encoder.h
```

- [ ] **Step 7: 验证 writer/ 文件列表**

Run: `ls src/edgar/explorgdb/writer/`
Expected: `gdb_table_writer.cpp`, `gdb_table_writer.h`, `geometry_serializer.h`, `row_buffer.h`, `tablx_writer.h`（5 个文件，varint_encoder.h 已删除）

---

### Task 7: 移除 writer 的 GDAL 依赖

**Files:**
- Modify: `src/edgar/explorgdb/writer/gdb_table_writer.h`
- Modify: `src/edgar/explorgdb/writer/gdb_table_writer.cpp`

- [ ] **Step 1: 更新 gdb_table_writer.h — 移除 GDAL 相关声明**

删除以下行：
```cpp
// GDAL 前置声明
class GDALDataset;
class OGRLayer;
```

删除 `create_new()` 方法声明：
```cpp
    // 新建 .gdb（内部调用 GDAL 创建 schema）
    bool create_new(const std::string& gdb_path,
                    const std::string& layer_name,
                    const std::vector<WriterField>& fields,
                    const std::string& wkt_srs,
                    int geom_type);
```

- [ ] **Step 2: 更新 gdb_table_writer.cpp — 移除 GDAL 实现**

删除 GDAL 头文件 include：
```cpp
#include "gdal.h"
#include "gdal_priv.h"
#include "ogr_api.h"
#include "ogr_srs_api.h"
#include "ogrsf_frmts.h"
#include "cpl_conv.h"
```

删除整个 `create_new()` 方法实现（第 43-125 行，从 `bool GdbTableWriter::create_new(...)` 到其结尾的 `return open_existing(gdb_path, layer_name); }`）。

- [ ] **Step 3: 验证无 GDAL 残留**

Run: `grep -rn "gdal\|GDAL\|OGR\|ogr" src/edgar/explorgdb/writer/`
Expected: 无输出

---

### Task 8: 迁移测试文件

**Files:**
- Move: 14 个读测试 → `tests/edgar/explorgdb/reader/`
- Move: 1 个写测试 → `tests/edgar/explorgdb/writer/`

- [ ] **Step 1: git mv 读测试**

```bash
cd /Users/edgarlqs/Downloads/daydaydaywork/dailyWork/convert/gdal/fast_gdb
git mv tests/edgar/explorgdb/test_binary_reader.cpp tests/edgar/explorgdb/reader/
git mv tests/edgar/explorgdb/test_catalog.cpp tests/edgar/explorgdb/reader/
git mv tests/edgar/explorgdb/test_full_audit.cpp tests/edgar/explorgdb/reader/
git mv tests/edgar/explorgdb/test_gdb_attribute_index.cpp tests/edgar/explorgdb/reader/
git mv tests/edgar/explorgdb/test_gdb_spatial_index.cpp tests/edgar/explorgdb/reader/
git mv tests/edgar/explorgdb/test_gdbindexes.cpp tests/edgar/explorgdb/reader/
git mv tests/edgar/explorgdb/test_gdbtable.cpp tests/edgar/explorgdb/reader/
git mv tests/edgar/explorgdb/test_gdbtablx.cpp tests/edgar/explorgdb/reader/
git mv tests/edgar/explorgdb/test_geometry.cpp tests/edgar/explorgdb/reader/
git mv tests/edgar/explorgdb/test_ole_date.cpp tests/edgar/explorgdb/reader/
git mv tests/edgar/explorgdb/test_spatial_benchmark.cpp tests/edgar/explorgdb/reader/
git mv tests/edgar/explorgdb/test_synthetic.cpp tests/edgar/explorgdb/reader/
git mv tests/edgar/explorgdb/test_utf16.cpp tests/edgar/explorgdb/reader/
git mv tests/edgar/explorgdb/test_varint.cpp tests/edgar/explorgdb/reader/
```

- [ ] **Step 2: git mv 写测试**

```bash
git mv tests/edgar/explorgdb/test_writer.cpp tests/edgar/explorgdb/writer/
```

- [ ] **Step 3: 验证测试目录结构**

Run: `find tests/edgar/explorgdb -type f | sort`
Expected:
```
tests/edgar/explorgdb/generate_large_gdb.cpp
tests/edgar/explorgdb/reader/test_binary_reader.cpp
tests/edgar/explorgdb/reader/test_catalog.cpp
tests/edgar/explorgdb/reader/test_full_audit.cpp
tests/edgar/explorgdb/reader/test_gdb_attribute_index.cpp
tests/edgar/explorgdb/reader/test_gdb_spatial_index.cpp
tests/edgar/explorgdb/reader/test_gdbindexes.cpp
tests/edgar/explorgdb/reader/test_gdbtable.cpp
tests/edgar/explorgdb/reader/test_gdbtablx.cpp
tests/edgar/explorgdb/reader/test_geometry.cpp
tests/edgar/explorgdb/reader/test_ole_date.cpp
tests/edgar/explorgdb/reader/test_spatial_benchmark.cpp
tests/edgar/explorgdb/reader/test_synthetic.cpp
tests/edgar/explorgdb/reader/test_utf16.cpp
tests/edgar/explorgdb/reader/test_varint.cpp
tests/edgar/explorgdb/test_fixture_explorgdb.h
tests/edgar/explorgdb/writer/test_writer.cpp
```

---

### Task 9: 更新测试文件 #include

**Files:**
- Modify: `tests/edgar/explorgdb/writer/test_writer.cpp`

读测试文件的 include 全部是短名称（如 `#include "gdb_table.h"`），通过 CMake include path 自动解析，无需修改。

写测试文件当前使用 `#include "writer/gdb_table_writer.h"`，需要改为 `#include "gdb_table_writer.h"`（因为 writer include dir 现在是 `src/edgar/explorgdb/writer`）。同理 `#include "writer/geometry_serializer.h"` → `#include "geometry_serializer.h"`。

- [ ] **Step 1: 更新 test_writer.cpp 的 include**

在 `tests/edgar/explorgdb/writer/test_writer.cpp` 中：

将：
```cpp
#include "writer/gdb_table_writer.h"
#include "writer/geometry_serializer.h"
```
改为：
```cpp
#include "gdb_table_writer.h"
#include "geometry_serializer.h"
```

- [ ] **Step 2: 更新 test_fixture_explorgdb.h（如有需要）**

检查 `test_fixture_explorgdb.h` 是否有项目内 include：

Run: `grep '#include "' tests/edgar/explorgdb/test_fixture_explorgdb.h`
Expected: 只有标准库 include，无需修改。

---

### Task 10: 更新 CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 替换 explorgdb 库定义部分**

将 CMakeLists.txt 第 41-61 行（`# ===== explorgdb: ...` 到 `target_compile_options(gdb_component PRIVATE ...)` 之后的 explorgdb 相关部分）替换为：

```cmake
# ===== explorgdb: 纯 C++ GDB 二进制解析器（不依赖 GDAL） =====

# common — 读写共享基础设施
file(GLOB COMMON_SOURCES src/edgar/explorgdb/common/*.cpp)
add_library(explorgdb_common_lib STATIC ${COMMON_SOURCES})
target_include_directories(explorgdb_common_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/common
)
target_compile_features(explorgdb_common_lib PUBLIC cxx_std_17)
target_compile_options(explorgdb_common_lib PRIVATE -Wall -Wno-unused-result)

# reader — GDB 读取器
file(GLOB READER_SOURCES src/edgar/explorgdb/reader/*.cpp)
list(FILTER READER_SOURCES EXCLUDE REGEX ".*explorgdb_cli\\.cpp$")
add_library(explorgdb_reader_lib STATIC ${READER_SOURCES})
target_include_directories(explorgdb_reader_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader
)
target_link_libraries(explorgdb_reader_lib PUBLIC explorgdb_common_lib)
target_compile_features(explorgdb_reader_lib PUBLIC cxx_std_17)
target_compile_options(explorgdb_reader_lib PRIVATE -Wall -Wno-unused-result)

# writer — GDB 写入器（纯 C++，不依赖 GDAL）
file(GLOB WRITER_SOURCES src/edgar/explorgdb/writer/*.cpp)
add_library(explorgdb_writer_lib STATIC ${WRITER_SOURCES})
target_include_directories(explorgdb_writer_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/writer
)
target_link_libraries(explorgdb_writer_lib PUBLIC explorgdb_common_lib)
target_compile_features(explorgdb_writer_lib PUBLIC cxx_std_17)
target_compile_options(explorgdb_writer_lib PRIVATE -Wall -Wno-unused-result)

# CLI 工具（不依赖 GDAL）
add_executable(explorgdb_cli src/edgar/explorgdb/reader/explorgdb_cli.cpp)
target_link_libraries(explorgdb_cli explorgdb_reader_lib)
target_compile_options(explorgdb_cli PRIVATE -Wall -Wno-unused-result -Wno-format)
```

- [ ] **Step 2: 更新测试源文件 glob**

将：
```cmake
file(GLOB EXPLORGDB_TEST_SOURCES tests/edgar/explorgdb/*.cpp)
```
改为：
```cmake
file(GLOB EXPLORGDB_READER_TEST_SOURCES tests/edgar/explorgdb/reader/*.cpp)
file(GLOB EXPLORGDB_WRITER_TEST_SOURCES tests/edgar/explorgdb/writer/*.cpp)
```

- [ ] **Step 3: 更新测试源文件列表**

将 `${EXPLORGDB_TEST_SOURCES}` 替换为 `${EXPLORGDB_READER_TEST_SOURCES} ${EXPLORGDB_WRITER_TEST_SOURCES}`：

```cmake
set(TEST_SOURCES
    tests/test_runner.cpp
    tests/test_fixture.h
    ${BASEUSE_SOURCES}
    ${USEGDAL_TEST_SOURCES}
    ${EXPLORGDB_READER_TEST_SOURCES}
    ${EXPLORGDB_WRITER_TEST_SOURCES}
)
```

- [ ] **Step 4: 更新测试 include 目录**

将：
```cmake
target_include_directories(gdb_tutorial_test_runner PRIVATE ${GDAL_INCLUDE_DIR} tests tests/edgar/component/usegdal tests/edgar/explorgdal tests/edgar/baseuse tests/edgar/explorgdb)
```
改为：
```cmake
target_include_directories(gdb_tutorial_test_runner PRIVATE
    ${GDAL_INCLUDE_DIR}
    tests
    tests/edgar/component/usegdal
    tests/edgar/explorgdal
    tests/edgar/baseuse
    tests/edgar/explorgdb
    tests/edgar/explorgdb/reader
    tests/edgar/explorgdb/writer
)
```

- [ ] **Step 5: 更新测试链接库**

将：
```cmake
target_link_libraries(gdb_tutorial_test_runner PRIVATE ${GDAL_LIBRARY} GTest::gtest GTest::gtest_main gdb_component explorgdb_lib explorgdb_writer_lib)
```
改为：
```cmake
target_link_libraries(gdb_tutorial_test_runner PRIVATE
    ${GDAL_LIBRARY} GTest::gtest GTest::gtest_main
    gdb_component
    explorgdb_reader_lib
    explorgdb_writer_lib
)
```

（`explorgdb_reader_lib` 和 `explorgdb_writer_lib` 都 PUBLIC 链接 `explorgdb_common_lib`，所以不需要单独列出）

- [ ] **Step 6: 更新 generate_large_gdb 的 include 路径**

`generate_large_gdb.cpp` 只依赖 GDAL，无 explorgdb include，无需修改。但确认一下：

Run: `grep '#include' tests/edgar/explorgdb/generate_large_gdb.cpp | head -5`
Expected: 只有 GDAL 头文件（`gdal_priv.h`, `ogrsf_frmts.h`, `cpl_string.h`）

- [ ] **Step 7: 更新 Build Info 消息**

将 CMakeLists.txt 末尾的 Build Info 部分更新：
```cmake
message(STATUS "=== GDB Tutorial ===")
message(STATUS "Component: gdb_component (usegdal — GDAL 高层 API)")
message(STATUS "Component: explorgdb_common_lib (纯 C++ 共享基础设施)")
message(STATUS "Component: explorgdb_reader_lib (纯 C++ GDB 二进制解析)")
message(STATUS "Component: explorgdb_writer_lib (纯 C++ GDB 写入器)")
message(STATUS "GDAL: ${GDAL_ROOT}")
message(STATUS "Target: explorgdb_cli - GDB 二进制探索工具")
message(STATUS "Target: gdb_tutorial_test_runner - 教程配套测试")
```

---

### Task 11: 构建验证

- [ ] **Step 1: cmake 配置**

```bash
cd /Users/edgarlqs/Downloads/daydaydaywork/dailyWork/convert/gdal/fast_gdb
rm -rf build && mkdir build && cd build
cmake ..
```

Expected: 配置成功，输出包含 `explorgdb_common_lib`, `explorgdb_reader_lib`, `explorgdb_writer_lib`

- [ ] **Step 2: 编译**

```bash
make -j$(sysctl -n hw.ncpu) 2>&1
```

Expected: 编译成功，无错误。如有编译错误，根据错误信息修复 include 路径。

常见问题排查：
- `"xxx.h" not found` → 检查 CMake `target_include_directories` 是否包含对应目录
- `undefined reference` → 检查 `target_link_libraries` 依赖链
- `duplicate symbol` → 检查是否有文件被两个库同时编译

- [ ] **Step 3: 运行全部测试**

```bash
./bin/gdb_tutorial_test_runner 2>&1
```

Expected: 全部 112 个测试通过（或接近 112，以实际数字为准）

- [ ] **Step 4: 运行 CTest**

```bash
ctest --output-on-failure 2>&1
```

Expected: 100% tests passed

- [ ] **Step 5: 验证 CLI 工具**

```bash
./bin/explorgdb_cli 2>&1 | head -5
```

Expected: 输出帮助信息

---

### Task 12: 提交

- [ ] **Step 1: 检查 git status**

```bash
cd /Users/edgarlqs/Downloads/daydaydaywork
git status
```

Expected: 大量 rename 操作

- [ ] **Step 2: 提交**

```bash
git add -A
git commit -m "refactor(explorgdb): 读写分离 — common/reader/writer 三目录重构

- 创建 common/ 目录: binary_reader, explorgdb_types, varint, utf16, ole_date
- 创建 reader/ 目录: gdb_table/tablx/catalog/geometry/indexes/attribute_index/spatial_index
- writer/ 保留: gdb_table_writer, geometry_serializer, row_buffer, tablx_writer
- 合并 varint_encoder.h 到 common/varint.h（消除重复）
- 移除 writer 的 GDAL 依赖（删除 create_new 方法）
- 测试目录同步分离为 reader/ 和 writer/
- 三个库全部纯 C++17，无 GDAL 依赖
- 全部 112 个测试通过"
```

---

## 风险与注意事项

1. **include 路径冲突**: common/ 和 reader/ 的 include 目录都被加到搜索路径，如果有同名文件会冲突。当前不存在同名文件。
2. **命名空间**: writer 代码在 `explorgdb::writer` 命名空间，common 函数在 `explorgdb` 命名空间。C++ 名称查找会从内层向外层查找，所以 writer 代码可以直接调用 `encode_varuint_to()` 无需加前缀。
3. **GDAL 测试依赖**: 虽然 writer 库本身不依赖 GDAL，但 `test_writer.cpp` 仍然使用 GDAL 创建测试用的 .gdb schema。这是测试代码的依赖，不影响库本身。
