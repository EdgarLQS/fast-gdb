# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

**测试即教程** — ESRI FileGDB 格式的教程项目，每个测试文件头部的注释即完整教程内容。包含两个独立 C++ 组件和配套测试。

## 构建与运行

### macOS / Linux

```bash
# 构建
cd fast_gdb && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)

# 运行全部 369 个测试（27 教程 + 113 组件 + 229 explorgdb）
./bin/gdb_tutorial_test_runner

# 运行单个章节
./bin/gdb_tutorial_test_runner --gtest_filter='T001_*'   # 001 GDB 格式
./bin/gdb_tutorial_test_runner --gtest_filter='T011_*'   # Phase 1A 组件测试

# 运行某类测试
./bin/gdb_tutorial_test_runner --gtest_filter='T001_*:T002_*:T003_*:T004_*'  # 基础篇
./bin/gdb_tutorial_test_runner --gtest_filter='T011_*:T012_*:T013_*:T014_*:T015_*'  # 组件篇

# CTest
ctest --output-on-failure

# 代码覆盖率（需要 lcov）
bash scripts/coverage.sh
```

### Windows（MSVC）

```bash
# 构建（需先安装 GDAL，设置 GDAL_HOME 或 CMAKE_PREFIX_PATH）
cd fast_gdb
mkdir build_win && cd build_win
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build . --config Release -- /maxcpucount:8

# 运行测试
.\bin\Release\gdb_tutorial_test_runner.exe
.\bin\Release\gdb_tutorial_test_runner.exe --gtest_filter='TablxCache*'

# Windows 特有构建说明
# - 自动添加 /utf-8 编译标志处理中文源文件编码
# - 使用 CMake 默认的 Visual Studio 生成器（VS 2022/2026）
# - GDAL 路径在 CMakeLists.txt 第 17-19 行配置
# - PROJ 数据库冲突时设置环境变量：
#   set PROJ_LIB=C:\path\to\gdal\share\proj
```

### 通用（各平台）

```bash
# explorgdb CLI 工具（输出在 build/bin/ 或 build_win/bin/Release/）
./bin/explorgdb_cli explore <gdb_path>          # 探索 GDB 目录
./bin/explorgdb_cli dump-table <file>           # 解析 .gdbtable
./bin/explorgdb_cli dump-tablx <file>           # 解析 .gdbtablx
./bin/explorgdb_cli dump-indexes <file>         # 解析 .gdbindexes
./bin/explorgdb_cli dump-records <file>         # 解析要素记录
```

## 依赖

- **GDAL 3.9.3**：硬编码路径 `/Users/edgarlqs/local/gdal-3.9.3`（CMakeLists.txt 第 17-19 行）
- **Google Test**：`brew install googletest`
- **lcov**（可选）：`brew install lcov`，用于覆盖率报告
- C++17，CMake 3.15+

## 架构

### 两个独立组件

| 组件 | 目录 | 依赖 GDAL | 说明 |
|------|------|-----------|------|
| **usegdal** | `src/edgar/usegdal/` | 是 | 基于 GDAL 高层 API 的 Datasource→Dataset→Recordset 组件库 |
| **explorgdb** | `src/edgar/explorgdb/` | 否 | 纯 C++ GDB 二进制解析器，直接解析 .gdbtable/.gdbtablx/.gdbindexes 等文件 |

### usegdal 对象层次

```
GdbConnectionPool（唯一线程安全对象，mutex 保护 acquire/release）
    └── GdbDatasource（拥有 GDALDataset*，继承 GdbErrorContext）
        └── GdbDatasets（视图，持有错误指针）
            └── GdbDataset（视图，持有错误指针）
                └── GdbRecordset（视图，持有错误指针 + RAII）

GdbField + GdbFeature + GdbQuery（值类型与查询）
GdbBatchWriter（批量写入，每线程独立连接）
```

**线程安全模型**：仅 `GdbConnectionPool` 线程安全。所有视图对象不线程安全。多线程并发时每个线程通过 `Pool.acquire()` 获取独立连接。

### 测试目录结构

```
tests/
├── tutorials/                    # 基础教程测试（T001~T010，27 用例）
├── usegdal/                      # usegdal 组件测试（T011~T015，113 用例）
├── edgar/explorgdb/              # explorgdb 解析器测试
│   ├── common/                   #   common/ 模块测试
│   ├── reader/                   #   reader/ 模块测试
│   └── writer/                   #   writer/ 模块测试
├── tools/                        # 独立工具（benchmark/verify/generate）
├── test_runner.cpp               # Google Test 入口
├── test_fixture.h                # 共享 Fixture
└── INDEX.md                      # 测试索引
```

测试目录镜像 `src/` 结构：`tests/usegdal/` ↔ `src/edgar/usegdal/`，`tests/edgar/explorgdb/{common,reader,writer}/` ↔ `src/edgar/explorgdb/{common,reader,writer}/`。

测试命名规则：`T001_*` 对应教程 001，`T011_*` 对应 Phase 1A 组件，以此类推。

## 构建产物

| 目标 | 说明 |
|------|------|
| `gdb_tutorial_test_runner` | 教程测试运行器（所有 369 个测试） |
| `explorgdb_cli` | GDB 二进制探索 CLI 工具 |
| `generate_large_gdb` | 大规模测试数据生成器 |
| `generate_100k_polygons` | 10 万面数据生成器（供 ArcGIS Pro 索引测试） |
| `verify_arcgis_indexes` | 验证 ArcGIS Pro 索引可被 explorgdb 正确读取 |
| `verify_binary_write_index` | 验证混合工作流（GDAL schema + 二进制写入 → 索引创建） |
| `verify_gdal_indexes` | 验证 GDAL 索引可被 explorgdb 正确读取 |
| `benchmark_index_creation` | 索引创建性能基准（空间/属性/复合） |

二进制输出到 `build/bin/`，运行时通过 `CMAKE_BUILD_RPATH` 自动找到 `libgdal.dylib`。

## 独立工具

`tests/tools/` 目录下的 5 个独立可执行文件（上表后 5 个）不参与 `test_runner` 编译，需单独构建运行。用于验证索引兼容性、生成测试数据、基准测试。

## 文档

`docs/` 目录包含 7 篇文档：

| 文件 | 内容 |
|------|------|
| `00_项目全景与架构概览.md` | 项目全景、架构总览、学习路线（**入口**） |
| `01_组件库设计与使用.md` | 架构设计、API 教程、空间关系查询（Phase 1A-3） |
| `02_性能基准与优化.md` | 完整基准测试（25 用例）、优化历程、性能对比根因分析 |
| `03_索引构建方案.md` | 混合工作流（ArcGIS Pro）、GDAL SQL 索引、API 参考 |
| `04_项目状态与规划.md` | 当前状态、已完成工作、待办事项、已知限制 |
| `05_技术探索与教训.md` | B+树分隔符发现、LRU Bug、mmap 优化、失败实验教训 |
| `07_GDB二进制格式图解教程.md` | 带图二进制格式教程（Mermaid + 源码链接） |

建议从 `docs/00_项目全景与架构概览.md` 开始阅读。
