# fast_gdb — GDB 教程 + C++ 组件库

ESRI FileGDB 格式研究项目。**测试即教程** — 369 个测试，每个测试文件头部的注释即完整教程内容。

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

## 亮点速览

- **三层操作路径**：GDAL CLI → usegdal 组件库 → explorgdb 二进制直写
- **测试即教程**：369 个测试，阅读即学习
- **不依赖 GDAL 的解析器**：explorgdb 可以直接读取 GDB 文件
- **极高性能**：Phase C 直写器 ~0.18 μs/feature；`GdbTableWriter` 在 100K+ 规模约 4.3~4.5 μs/feature；零拷贝读取比 GDAL 快 9-12x

## 学习路线

1. **入门** → 运行 `T001_*:T002_*:T003_*` 测试，读教程注释
2. **API 使用** → 读 `01_组件库设计与使用.md`
3. **性能** → 读 `02_性能基准与优化.md`
4. **二进制深入** → 用 `explorgdb_cli` 探索真实 GDB
5. **索引** → 读 `03_索引构建方案.md`
6. **全景** → 读 `04_项目状态与规划.md`

详见 [docs/00_项目全景与架构概览.md](docs/00_项目全景与架构概览.md)。

## 文档

| 文件 | 内容 |
|------|------|
| `docs/00_项目全景与架构概览.md` | 项目全景、架构总览、学习路线（入口） |
| `docs/01_组件库设计与使用.md` | 架构设计、API 教程、空间关系查询 |
| `docs/02_性能基准与优化.md` | 完整基准测试、优化历程、性能对比根因分析 |
| `docs/03_索引构建方案.md` | 混合工作流（ArcGIS Pro）、GDAL SQL 索引、API 参考 |
| `docs/04_项目状态与规划.md` | 当前状态、已完成工作、待办事项、已知限制 |
| `docs/05_技术探索与教训.md` | B+树分隔符发现、LRU Bug、mmap 优化、失败实验教训 |
| `docs/07_GDB二进制格式图解教程.md` | 带图二进制格式教程（Mermaid + 源码链接） |

## 依赖

- GDAL 3.9.3（路径：`/Users/edgarlqs/local/gdal-3.9.3`）
- Google Test（`brew install googletest`）
- C++17，CMake 3.15+
