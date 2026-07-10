# fast_gdb 文档索引

本文档是 `docs/` 的分类入口。新读者先读总览；判断当前完成度时先读规划状态索引；排查实现细节时读技术专题。

## 总览

| 文件 | 内容 |
|------|------|
| [00_项目全景与架构概览.md](overview/00_项目全景与架构概览.md) | 项目全景、构建目标、架构总览、学习路线 |

## 使用

| 文件 | 内容 |
|------|------|
| [01_组件库设计与使用.md](usage/01_组件库设计与使用.md) | usegdal 组件库设计、API 教程、查询与写入示例 |

## 技术专题

| 文件 | 内容 |
|------|------|
| [01_性能基准与优化.md](technical/01_性能基准与优化.md) | 基准测试、优化历程、性能差异根因 |
| [02_索引构建方案.md](technical/02_索引构建方案.md) | 空间/属性索引构建策略和验证工具 |
| [03_技术探索与教训.md](technical/03_技术探索与教训.md) | B+ 树、LRU、mmap、失败实验和经验沉淀 |
| [04_GDB二进制格式图解教程.md](technical/04_GDB二进制格式图解教程.md) | FileGDB 二进制格式图解、查询链路和源码链接 |

## 规划与状态

状态冲突时，先看 [00_规划文档状态索引.md](planning/00_规划文档状态索引.md)。

| 文件 | 状态 | 内容 |
|------|:---:|------|
| [00_规划文档状态索引.md](planning/00_规划文档状态索引.md) | 当前入口 | 权威顺序、v2 结果和 v3 待办 |
| [01_项目状态与规划.md](planning/01_项目状态与规划.md) | 当前 | 项目总体状态和范围边界 |
| [02_GDAL功能对比矩阵.md](planning/02_GDAL功能对比矩阵.md) | 当前 | reader 与 GDAL OpenFileGDB 的实际能力差异 |
| [03_fast-gdb优先生产化计划.md](planning/03_fast-gdb优先生产化计划.md) | 历史 | v1 生产化原始计划及结果摘要 |
| [04_fast-gdb-v1实施设计.md](planning/04_fast-gdb-v1实施设计.md) | 历史 | v1 已合并实现记录 |
| [05_merge_readiness.md](planning/05_merge_readiness.md) | 历史 | v1 原分支合并检查单归档 |
| [06_fast-gdb-v2开发计划.md](planning/06_fast-gdb-v2开发计划.md) | 历史 | v2 原始范围与最终结果 |
| [07_fast-gdb-v2后续统一计划.md](planning/07_fast-gdb-v2后续统一计划.md) | v2 关闭 | v2 自动化和文档收口结果 |
| [08_fast-gdb只读发布收口.md](planning/08_fast-gdb只读发布收口.md) | v2 验收 | 本地结果、能力边界和真实数据移交 |
| [09_fast-gdb曲线几何分析.md](planning/09_fast-gdb曲线几何分析.md) | 参考 | 曲线格式、显式保护和未来实现要求 |
| [10_fast-gdb-v3几何正确性与真实数据计划.md](planning/10_fast-gdb-v3几何正确性与真实数据计划.md) | 当前计划 | General 点类型、bbox/坐标一致性和真实数据回归 |
| [11_fast-gdb替换GDAL矢量能力分析.md](planning/11_fast-gdb替换GDAL矢量能力分析.md) | 架构参考 | 替换 GDAL 矢量只读路径的优劣势、适用边界和推荐双路径架构 |
