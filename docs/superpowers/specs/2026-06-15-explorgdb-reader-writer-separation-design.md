# explorgdb 读写分离重构设计

**日期**: 2026-06-15
**目标**: 将 `src/edgar/explorgdb/` 拆分为 `common/`、`reader/`、`writer/` 三个独立目录，消除读写耦合，writer 去掉 GDAL 依赖变为纯 C++。

## 背景

当前 `explorgdb/` 根目录混杂了读取器代码和公共类型（24 个文件），`writer/` 子目录已分离写入器代码（6 个文件）。存在以下问题：

1. 读写代码混在根目录，结构不清晰
2. writer 依赖 GDAL（`create_new()` 用 GDAL 创建 schema），不是纯 C++
3. `varint_encoder.h`（writer）和 `varint.h`（根目录）存在编码逻辑重复
4. `binary_reader` 被读写两端共用，但放在根目录归属不明确

## 目标结构

```
src/edgar/explorgdb/
├── common/                      ← 读写共享的基础设施（纯 C++17，无外部依赖）
│   ├── binary_reader.h/cpp
│   ├── explorgdb_types.h/cpp
│   ├── varint.h/cpp             ← 统一编解码（合并原 varint_encoder.h）
│   ├── utf16.h/cpp
│   └── ole_date.h/cpp
│
├── reader/                      ← GDB 读取器（纯 C++，不依赖 GDAL）
│   ├── gdb_table.h/cpp
│   ├── gdb_tablx.h/cpp
│   ├── gdb_catalog.h/cpp
│   ├── gdb_geometry.h/cpp
│   ├── gdb_indexes.h/cpp
│   ├── gdb_attribute_index.h/cpp
│   ├── gdb_spatial_index.h/cpp
│   ├── explorgdb_cli.cpp
│   ├── QUERY_FLOW.md
│   └── README.md
│
└── writer/                      ← GDB 写入器（纯 C++，不依赖 GDAL）
    ├── gdb_table_writer.h/cpp
    ├── geometry_serializer.h
    ├── row_buffer.h
    └── tablx_writer.h
```

## 测试目录结构

```
tests/edgar/explorgdb/
├── reader/                      ← 读相关测试（14 个文件）
│   ├── test_binary_reader.cpp
│   ├── test_catalog.cpp
│   ├── test_full_audit.cpp
│   ├── test_gdb_attribute_index.cpp
│   ├── test_gdb_spatial_index.cpp
│   ├── test_gdbindexes.cpp
│   ├── test_gdbtable.cpp
│   ├── test_gdbtablx.cpp
│   ├── test_geometry.cpp
│   ├── test_ole_date.cpp
│   ├── test_spatial_benchmark.cpp
│   ├── test_synthetic.cpp
│   ├── test_utf16.cpp
│   └── test_varint.cpp
├── writer/                      ← 写相关测试
│   └── test_writer.cpp
├── generate_large_gdb.cpp       ← 独立可执行文件，保留根目录
└── test_fixture_explorgdb.h     ← 共享 fixture
```

## 依赖关系

```
common/     ← 无依赖（纯 C++17 标准库）
reader/     → common/
writer/     → common/
```

三个库全部纯 C++，不依赖 GDAL。只有测试运行器（`gdb_tutorial_test_runner`）和 CLI 工具需要 GDAL。

## 关键变更

### 1. 文件迁移

| 原位置 | 目标位置 |
|--------|----------|
| `binary_reader.h/cpp` | `common/` |
| `explorgdb_types.h/cpp` | `common/` |
| `varint.h/cpp` | `common/` |
| `utf16.h/cpp` | `common/` |
| `ole_date.h/cpp` | `common/` |
| `gdb_table.h/cpp` | `reader/` |
| `gdb_tablx.h/cpp` | `reader/` |
| `gdb_catalog.h/cpp` | `reader/` |
| `gdb_geometry.h/cpp` | `reader/` |
| `gdb_indexes.h/cpp` | `reader/` |
| `gdb_attribute_index.h/cpp` | `reader/` |
| `gdb_spatial_index.h/cpp` | `reader/` |
| `explorgdb_cli.cpp` | `reader/` |
| `QUERY_FLOW.md`, `README.md` | `reader/` |

### 2. varint 统一

删除 `writer/varint_encoder.h`，将编码函数合并到 `common/varint.h`。

需要修改的文件：
- `writer/geometry_serializer.h` — 改用 `#include "../common/varint.h"`
- `writer/row_buffer.h` — 改用 `#include "../common/varint.h"`

### 3. writer 去掉 GDAL 依赖

从 `gdb_table_writer.h/cpp` 中移除：
- `#include "gdal.h"` 等 GDAL 头文件
- `class GDALDataset; class OGRLayer;` 前置声明
- `create_new()` 方法（GDAL schema 创建逻辑）
- GDALDataset* 成员变量

保留：
- `open_existing()` 方法（纯 C++ 打开已有 .gdb）
- 所有行写入方法（`begin_row()`, `append_*()`, `end_row()`）
- `close()`, `flush()` 等控制方法

schema 创建逻辑已在测试中通过 GDAL 直接完成（`test_writer.cpp` 的 `create_z_gdb()` 等），无需 writer 提供此功能。

### 4. CMake 变更

```cmake
# 替换原来的 explorgdb_lib 和 explorgdb_writer_lib

# common — 纯 C++ 共享库
file(GLOB COMMON_SOURCES src/edgar/explorgdb/common/*.cpp)
add_library(explorgdb_common_lib STATIC ${COMMON_SOURCES})
target_include_directories(explorgdb_common_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/common
)

# reader — 纯 C++ 读取器
file(GLOB READER_SOURCES src/edgar/explorgdb/reader/*.cpp)
list(FILTER READER_SOURCES EXCLUDE REGEX ".*explorgdb_cli\\.cpp$")
add_library(explorgdb_reader_lib STATIC ${READER_SOURCES})
target_include_directories(explorgdb_reader_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/reader
)
target_link_libraries(explorgdb_reader_lib PUBLIC explorgdb_common_lib)

# writer — 纯 C++ 写入器
file(GLOB WRITER_SOURCES src/edgar/explorgdb/writer/*.cpp)
add_library(explorgdb_writer_lib STATIC ${WRITER_SOURCES})
target_include_directories(explorgdb_writer_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/edgar/explorgdb/writer
)
target_link_libraries(explorgdb_writer_lib PUBLIC explorgdb_common_lib)

# CLI 工具 — 依赖 reader
add_executable(explorgdb_cli src/edgar/explorgdb/reader/explorgdb_cli.cpp)
target_link_libraries(explorgdb_cli explorgdb_reader_lib)

# 测试运行器 — 需要 GDAL（测试中用 GDAL 创建 schema）
target_link_libraries(gdb_tutorial_test_runner PRIVATE
    ${GDAL_LIBRARY} GTest::gtest GTest::gtest_main
    gdb_component
    explorgdb_reader_lib
    explorgdb_writer_lib
)
```

### 5. include 路径更新

迁移后所有 `#include` 需要更新路径：

**reader/ 内部文件**：
```cpp
// 原来
#include "binary_reader.h"
#include "explorgdb_types.h"
// 改为
#include "../common/binary_reader.h"
#include "../common/explorgdb_types.h"
```

**writer/ 内部文件**：
```cpp
// 原来
#include "../explorgdb_types.h"
#include "../binary_reader.h"
// 改为
#include "../common/explorgdb_types.h"
#include "../common/binary_reader.h"
```

**测试文件**：
```cpp
// 原来
#include "binary_reader.h"
#include "gdb_table.h"
// 改为
#include "../src/edgar/explorgdb/common/binary_reader.h"  // 或通过 CMake include path
#include "../src/edgar/explorgdb/reader/gdb_table.h"
```

通过 CMake 的 `target_include_directories` 设置 PUBLIC include path 后，测试可以用简短路径：
```cpp
#include "binary_reader.h"      // 来自 common
#include "gdb_table.h"          // 来自 reader
#include "gdb_table_writer.h"   // 来自 writer
```

## 验证标准

```bash
cd build && cmake .. && make -j$(sysctl -n hw.ncpu)

# 全部 112 个测试通过
./bin/gdb_tutorial_test_runner

# 特定模块测试
./bin/gdb_tutorial_test_runner --gtest_filter='T_W*'      # writer 测试
./bin/gdb_tutorial_test_runner --gtest_filter='T016_*'    # explorgdb 读取测试

# CTest
ctest --output-on-failure

# CLI 工具
./bin/explorgdb_cli explore <gdb_path>
```

## 不在范围内

- 不改变任何业务逻辑
- 不修改 API 接口（除删除 `create_new()` 外）
- 不添加新功能
- 不重构代码实现（只移动文件 + 更新 include）
