# fast_gdb 文档索引

本文档是 `docs/` 的分类入口。当前分支为 `codex/spatial-attribute-query`，空间与属性联合查询、FeatureCursor 和 WKB-first Reader 已通过本地代码审核、Release 构建、并行 CTest、package consumer 与 100K 基准。分支内容尚未进入 `main`，跨平台和正式发布验收仍阻塞。

## 当前分支审核入口

| 文件 | 内容 |
|---|---|
| [21_空间属性联合查询实现计划.md](planning/21_空间属性联合查询实现计划.md) | 实现范围、GDAL 参考、执行路径、测试矩阵、进度和验收清单 |
| [10_空间属性联合查询代码审核指南.md](usage/10_空间属性联合查询代码审核指南.md) | 审核顺序、重点文件、P0/P1 检查点、建议命令和报告格式 |
| [spatial-attribute-query-self-review-2026-07-17.md](evidence/spatial-attribute-query-self-review-2026-07-17.md) | 三轮静态代码审核发现和修复记录 |
| [spatial-attribute-query-document-audit-2026-07-17.md](evidence/spatial-attribute-query-document-audit-2026-07-17.md) | 文档一致性自检、冲突和修复记录 |
| [branch-review-and-validation-2026-07-18.md](evidence/branch-review-and-validation-2026-07-18.md) | 独立审核、修复、本地构建/CTest/consumer/100K 证据 |
| [04_功能与基准测试覆盖矩阵.md](usage/04_功能与基准测试覆盖矩阵.md) | 当前自动化、性能和正式验证缺口 |
| [Reader QUERY_FLOW](../src/edgar/explorgdb/reader/QUERY_FLOW.md) | 联合查询源码执行链和回退语义 |

## 总览

| 文件 | 内容 |
|---|---|
| [01_fast-gdb项目介绍与当前状态.md](overview/01_fast-gdb项目介绍与当前状态.md) | 面向开发人员的项目定位、架构、性能、测试和验收总览 |
| [00_项目全景与架构概览.md](overview/00_项目全景与架构概览.md) | 项目全景、构建目标、架构总览、学习路线 |

## 使用

| 文件 | 内容 |
|---|---|
| [03_测试数据准备与跨平台验证.md](usage/03_测试数据准备与跨平台验证.md) | 测试数据权威入口；GDAL/ArcGIS Pro 生成、三平台回归和性能验收 |
| [04_功能与基准测试覆盖矩阵.md](usage/04_功能与基准测试覆盖矩阵.md) | 能力、测试证据、数据、平台、CI 门禁和基准状态矩阵 |
| [05_fast-gdb真实数据验收资料清单.md](usage/05_fast-gdb真实数据验收资料清单.md) | 新增能力和真实数据的验收规范 |
| [10_空间属性联合查询代码审核指南.md](usage/10_空间属性联合查询代码审核指南.md) | 当前分支代码审核入口 |
| [01_组件库设计与使用.md](usage/01_组件库设计与使用.md) | usegdal 组件库设计、API 教程、查询与写入示例 |
| [02_几何WKB曲线支持与迁移.md](usage/02_几何WKB曲线支持与迁移.md) | GeometryValue/Model、ISO WKB、Polygon、曲线、Hybrid FID 和兼容策略 |

## 技术专题

| 文件 | 内容 |
|---|---|
| [01_性能基准与优化.md](technical/01_性能基准与优化.md) | 基准测试、优化历程、性能差异根因 |
| [02_索引构建方案.md](technical/02_索引构建方案.md) | 空间/属性索引构建策略和验证工具 |
| [03_技术探索与教训.md](technical/03_技术探索与教训.md) | B+ 树、LRU、mmap、失败实验和经验沉淀 |
| [04_GDB二进制格式图解教程.md](technical/04_GDB二进制格式图解教程.md) | FileGDB 二进制格式图解、查询链路和源码链接 |
| [Reader QUERY_FLOW](../src/edgar/explorgdb/reader/QUERY_FLOW.md) | 空间、属性和 `SpatialWhere` 联合执行流程 |

## 规划与状态

状态冲突时，以 [00_规划文档状态索引.md](planning/00_规划文档状态索引.md) 的阅读顺序为准。

| 文件 | 状态 | 内容 |
|---|:---:|---|
| [00_规划文档状态索引.md](planning/00_规划文档状态索引.md) | 当前入口 | 当前分支审核顺序、进度和未完成证据 |
| [01_项目状态与规划.md](planning/01_项目状态与规划.md) | 当前 | 项目总体状态、发布边界和联合查询分支状态 |
| [02_GDAL功能对比矩阵.md](planning/02_GDAL功能对比矩阵.md) | 当前 | linear / hybrid / GDAL 实际能力差异 |
| [21_空间属性联合查询实现计划.md](planning/21_空间属性联合查询实现计划.md) | 当前计划 | 联合查询实现、测试、自审、文档和验收状态 |
| [18_writer跨平台测试统一与后续编辑计划.md](planning/18_writer跨平台测试统一与后续编辑计划.md) | 其他工作流 | Writer 能力、合同和验收边界 |
| [13_fast-gdb最终等价与发布验收报告.md](evidence/13_fast-gdb最终等价与发布验收报告.md) | 发布证据 | v0.1.0 既有支持范围，不含本分支新增能力 |
| [curve-polyline-m-real-acceptance-2026-07-13.md](evidence/curve-polyline-m-real-acceptance-2026-07-13.md) | 历史证据 | 真实 M 曲线逐要素、WKB、空间查询和 sanitizer 证据 |
| [reader-fresh-open-macos-2026-07-15.md](evidence/reader-fresh-open-macos-2026-07-15.md) | 历史证据 | Point/MultiPoint/Polyline 10M fresh-open 正确性与性能失败档 |

历史阶段计划和归档文档不再作为本分支完成度来源。

## 当前结论

- 既有正式输出仍为 ISO WKB-first；
- `.spx` 和 `.atx` 都只提供候选；
- `SpatialWhere` 在当前分支组合精确 bbox 与现有 WHERE 子集；
- 所有 `.atx` 快速路径都执行最终 WHERE 复核；
- 损坏索引必须 fail closed；
- 当前分支本地代码审核和提交门禁已通过；跨平台、真实数据和大规模性能证据未完成；
- MultiPatch 仍为 degraded；
- 合成自动化、真实数据和正式 artifact 必须分开记录。
