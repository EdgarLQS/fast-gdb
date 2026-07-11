# fast_gdb 文档索引

本文档是 `docs/` 的分类入口。新读者先读总览；判断当前完成度先读规划状态索引和实施报告；迁移几何 API 时读 WKB/曲线迁移指南。

## 总览

| 文件 | 内容 |
|---|---|
| [00_项目全景与架构概览.md](overview/00_项目全景与架构概览.md) | 项目全景、构建目标、架构总览、学习路线 |

## 使用

| 文件 | 内容 |
|---|---|
| [01_组件库设计与使用.md](usage/01_组件库设计与使用.md) | usegdal 组件库设计、API 教程、查询与写入示例 |
| [02_几何WKB曲线支持与迁移.md](usage/02_几何WKB曲线支持与迁移.md) | GeometryValue/Model、ISO WKB、Polygon、曲线、Hybrid FID 和兼容策略 |

## 技术专题

| 文件 | 内容 |
|---|---|
| [01_性能基准与优化.md](technical/01_性能基准与优化.md) | 基准测试、优化历程、性能差异根因 |
| [02_索引构建方案.md](technical/02_索引构建方案.md) | 空间/属性索引构建策略和验证工具 |
| [03_技术探索与教训.md](technical/03_技术探索与教训.md) | B+ 树、LRU、mmap、失败实验和经验沉淀 |
| [04_GDB二进制格式图解教程.md](technical/04_GDB二进制格式图解教程.md) | FileGDB 二进制格式图解、查询链路和源码链接 |

## 规划与状态

状态冲突时，以 [00_规划文档状态索引.md](planning/00_规划文档状态索引.md) 的阅读顺序为准。

| 文件 | 状态 | 内容 |
|---|:---:|---|
| [00_规划文档状态索引.md](planning/00_规划文档状态索引.md) | 当前入口 | 权威文档顺序和原则 |
| [01_项目状态与规划.md](planning/01_项目状态与规划.md) | 当前 | 项目总体状态、产品和发布边界 |
| [02_GDAL功能对比矩阵.md](planning/02_GDAL功能对比矩阵.md) | 当前 | linear / hybrid / GDAL 实际能力差异 |
| [08_fast-gdb只读发布收口.md](planning/08_fast-gdb只读发布收口.md) | v2 验收 | v2 只读历史证据 |
| [10_fast-gdb几何正确性与曲线支持执行计划.md](planning/10_fast-gdb几何正确性与曲线支持执行计划.md) | 当前计划 | Polygon、WKB-first、曲线和双后端设计 |
| [11_fast-gdb几何正确性与曲线支持实施报告.md](planning/11_fast-gdb几何正确性与曲线支持实施报告.md) | 当前实施 | 代码、自动化、兼容决策和真实数据门禁 |
| [11_fast-gdb替换GDAL矢量能力分析.md](planning/11_fast-gdb替换GDAL矢量能力分析.md) | 架构参考 | fast-gdb 与 GDAL 双路径取舍 |

`03`–`07`、旧 v3 计划和早期曲线分析属于历史背景；从 [planning/archive/README.md](planning/archive/README.md) 或状态索引进入，不再作为当前完成度来源。

## 当前几何结论

- 正式输出是 ISO WKB-first；
- WKT 是兼容/调试接口；
- Polygon WKB、WKT 和空间过滤共用一个拓扑模型；
- CircularArc、Bezier、Ellipse 可由内置后端折线化；
- 可选 Hybrid 只在曲线/拓扑失败时使用缓存式 GDAL 回退；
- MultiPatch 仍为 degraded；
- 合成自动化与 ArcGIS Pro 真实曲线验收分开记录。
