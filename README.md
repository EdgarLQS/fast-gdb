# fast_gdb — GDB 教程 + C++ 组件库

ESRI FileGDB 格式研究项目。**测试即教程** — 每个测试文件头部的注释即完整教程内容。

## 快速开始

```bash
cd fast_gdb && mkdir -p build && cd build
cmake ..
make -j$(sysctl -n hw.ncpu)

# 运行全部 369 个测试
./bin/gdb_tutorial_test_runner

# GDB 二进制探索 CLI
./bin/explorgdb_cli explore <gdb_path>
```

## 项目组成

| 组件 | 目录 | 说明 |
|------|------|------|
| **usegdal** | `src/edgar/usegdal/` | 基于 GDAL 高层 API 的 Datasource→Dataset→Recordset 组件库 |
| **explorgdb** | `src/edgar/explorgdb/` | 纯 C++ GDB 二进制解析器（reader + writer），不依赖 GDAL |

## 文档

- **架构、API、构建、测试** → [CLAUDE.md](CLAUDE.md)
- **详细技术文档** → `docs/` 目录（5 篇整合文档）

| 文件 | 内容 |
|------|------|
| `docs/01_组件库设计与使用.md` | 架构设计、API 教程、空间关系查询 |
| `docs/02_性能基准与优化.md` | 完整基准测试、优化历程、性能对比根因分析 |
| `docs/03_索引构建方案.md` | 混合工作流（ArcGIS Pro）、GDAL SQL 索引、API 参考 |
| `docs/04_项目状态与规划.md` | 当前状态、已完成工作、待办事项、已知限制 |
| `docs/05_技术探索与教训.md` | B+树分隔符发现、LRU Bug、mmap 优化、失败实验教训 |

## 依赖

- GDAL 3.9.3（路径：`/Users/edgarlqs/local/gdal-3.9.3`）
- Google Test（`brew install googletest`）
- C++17，CMake 3.15+
