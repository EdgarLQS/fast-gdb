# fast_gdb — GDB 教程 + C++ 组件库

ESRI FileGDB 格式研究项目。**测试即教程**——测试文件头部注释同时记录格式、API 和实现边界。

## 快速开始

```bash
cd fast_gdb
cmake -S . -B build
cmake --build build --target gdb_tutorial_test_runner -j

# 枚举当前实际测试数量
./build/bin/gdb_tutorial_test_runner --gtest_list_tests

# 运行功能测试
./build/bin/gdb_tutorial_test_runner

# GDB 二进制探索 CLI
./build/bin/explorgdb_cli explore <gdb_path>
```

大型 benchmark 建议与 correctness 测试分开运行，避免基准进程状态影响功能验收结论。
10M benchmark 默认跳过；仅在性能验收时通过 `FAST_GDB_RUN_10M_BENCHMARKS=1` 显式运行。

仓库内置的普通真实 FileGDB 样本可用于 release contract 验证：

```bash
FAST_GDB_REAL_DATASET="$PWD/test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb" \
./build/bin/gdb_tutorial_test_runner \
  --gtest_filter='RealDataReleaseContractTest.RegularFileGdbMatchesCoreReadContract'
```

注意该样本目录是双层 `.gdb` 包装，环境变量必须指向内层目录。它不包含 GDAL 可识别的原生曲线，不能作为 `FAST_GDB_CURVE_DATASET`；曲线契约仍需单独准备 ArcGIS Pro 原生曲线样本。

## 项目组成

| 组件 | 目录 | 说明 |
|------|------|------|
| **usegdal** | `src/edgar/usegdal/` | 基于 GDAL 高层 API 的 Datasource→Dataset→Recordset 组件库 |
| **explorgdb** | `src/edgar/explorgdb/` | 纯 C++ GDB 二进制 reader + writer；reader 主路径不依赖 GDAL |

## 能力边界

- 常规 reader 主路径、索引、元数据和查询门面已实现。
- 原生曲线当前明确 unsupported，不线性化。
- MultiPatch 当前为 degraded：保留坐标和有限 WKT 表达，不保留完整 part type/表面拓扑。
- writer 主数据直写已实现，但系统表同步尚未完成。

当前状态和后续计划以以下文档为准：

- `docs/planning/00_规划文档状态索引.md`
- `docs/planning/01_项目状态与规划.md`
- `docs/planning/02_GDAL功能对比矩阵.md`
- `docs/planning/10_fast-gdb-v3几何正确性与真实数据计划.md`

## 学习路线

1. 入门：运行 `T001_*:T002_*:T003_*`。
2. API：读 `docs/usage/01_组件库设计与使用.md`。
3. 性能：读 `docs/technical/01_性能基准与优化.md`。
4. 二进制：运行 `explorgdb_cli`，配合格式教程。
5. 当前状态：读 `docs/README.md` 和 planning 状态索引。

## 依赖

- GDAL 3.9.3（当前 CMake 默认路径为 `/Users/edgarlqs/local/gdal-3.9.3`）
- Google Test
- C++17，CMake 3.15+
