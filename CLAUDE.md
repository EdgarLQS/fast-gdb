# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

**测试即教程** — ESRI FileGDB 格式的教程项目，每个测试文件头部的注释即完整教程内容。包含两个独立 C++ 组件和配套测试。

## 构建与运行

```bash
# 构建
cd fast_gdb && mkdir -p build && cd build
cmake ..
make -j$(sysctl -n hw.ncpu)

# 运行全部 112 个测试（27 基础 + 85 组件）
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

# explorgdb CLI 工具
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
├── edgar/
│   ├── baseuse/               # 基础教程测试（T001~T010，27 用例）
│   ├── component/usegdal/     # usegdal 组件测试（T011~T015，85 用例）
│   └── explorgdb/             # explorgdb 解析器测试
├── test_runner.cpp            # Google Test 入口
├── test_fixture.h             # 共享 Fixture（GDAL 初始化 + 自动清理）
└── INDEX.md                   # 测试索引
```

测试命名规则：`T001_*` 对应教程 001，`T011_*` 对应 Phase 1A 组件，以此类推。

## 构建产物

| 目标 | 说明 |
|------|------|
| `gdb_tutorial_test_runner` | 教程测试运行器（基础 + 组件全部测试） |
| `explorgdb_cli` | GDB 二进制探索 CLI 工具 |
| `generate_large_gdb` | 大规模测试数据生成器 |

二进制输出到 `build/bin/`，运行时通过 `CMAKE_BUILD_RPATH` 自动找到 `libgdal.dylib`。
