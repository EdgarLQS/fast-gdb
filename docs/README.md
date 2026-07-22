# fast-gdb 文档索引

本文档是 `docs/` 的分类入口。当前 Writer 开发分支为 `agent/versioned-gdb-store`，公共 Writer API 已收敛为 VersionedGdbStore；旧 Writer、legacy target 和直接发布文档已删除。

## 当前 Writer 入口

| 文件 | 内容 |
|---|---|
| [VersionedGdbStore 使用指南](usage/11_VersionedGdbStore并发读写与版本发布.md) | 初始化、Reader snapshot、working generation、validator、发布状态、recover 和能力边界 |
| [ADR-007](adr/ADR-007-versioned-gdb-store.md) | 唯一 Writer 公共 API、不可变 generation 和持久化协议 |
| [Writer 生命周期](architecture/writer-lifecycle.md) | Store、Reader、Writer、validator 和 recovery 状态机 |
| [Writer Known Limitations](architecture/writer-known-limitations.md) | 并发、文件系统、容量、事务和验收边界 |
| [Writer Roadmap](roadmap/writer-roadmap.md) | API 收敛、并发、持久化和三平台验收门禁 |
| [核心实现三轮自检](evidence/versioned-gdb-store-three-round-self-review-2026-07-21.md) | 并发/生命周期、崩溃一致性、API/跨平台/测试发现与修复 |
| [latest-only API 三轮自检](evidence/versioned-gdb-store-latest-only-api-review-2026-07-22.md) | 旧公共接口删除、versioned-only archive、安装负向检查和文档/CI 收敛 |

旧 ADR-001～ADR-005、WriterSession/Append/Update/Delete/Transaction 使用指南及对应 workflow 已被 ADR-007 取代并从当前分支删除。

## 总览

| 文件 | 内容 |
|---|---|
| [01_fast-gdb项目介绍与当前状态.md](overview/01_fast-gdb项目介绍与当前状态.md) | 项目定位、产品形态、Reader 与 Writer Store 当前状态 |
| [00_项目全景与架构概览.md](overview/00_项目全景与架构概览.md) | 项目全景、构建目标、架构总览和学习路线 |

## 使用

| 文件 | 内容 |
|---|---|
| [11_VersionedGdbStore并发读写与版本发布.md](usage/11_VersionedGdbStore并发读写与版本发布.md) | 唯一 Writer API |
| [03_测试数据准备与跨平台验证.md](usage/03_测试数据准备与跨平台验证.md) | 测试数据、GDAL/ArcGIS Pro 生成和三平台验证 |
| [04_功能与基准测试覆盖矩阵.md](usage/04_功能与基准测试覆盖矩阵.md) | 自动化、性能和正式验证缺口 |
| [05_fast-gdb真实数据验收资料清单.md](usage/05_fast-gdb真实数据验收资料清单.md) | 新能力真实数据验收规范 |
| [01_组件库设计与使用.md](usage/01_组件库设计与使用.md) | usegdal 组件和 Reader 使用示例 |
| [02_几何WKB曲线支持与迁移.md](usage/02_几何WKB曲线支持与迁移.md) | GeometryValue/Model、ISO WKB、Polygon、曲线和 Hybrid FID |

## Reader 当前分支审核入口

| 文件 | 内容 |
|---|---|
| [21_空间属性联合查询实现计划.md](planning/21_空间属性联合查询实现计划.md) | 联合查询实现范围、测试矩阵和验收清单 |
| [10_空间属性联合查询代码审核指南.md](usage/10_空间属性联合查询代码审核指南.md) | Reader 审核顺序和检查点 |
| [spatial-attribute-query-self-review-2026-07-17.md](evidence/spatial-attribute-query-self-review-2026-07-17.md) | Reader 三轮静态审核 |
| [Reader QUERY_FLOW](../src/edgar/explorgdb/reader/QUERY_FLOW.md) | 查询源码执行链和回退语义 |

## 技术专题

| 文件 | 内容 |
|---|---|
| [01_性能基准与优化.md](technical/01_性能基准与优化.md) | 基准测试和性能差异根因 |
| [02_索引构建方案.md](technical/02_索引构建方案.md) | 空间/属性索引构建与验证 |
| [03_技术探索与教训.md](technical/03_技术探索与教训.md) | B+ 树、LRU、mmap 和失败实验 |
| [04_GDB二进制格式图解教程.md](technical/04_GDB二进制格式图解教程.md) | FileGDB 二进制格式、查询链路和源码链接 |

## 状态原则

- Writer 当前唯一公共入口为 VersionedGdbStore；
- `include/fast_gdb/writer/` 是 Writer 公共头权威目录；
- `src/edgar/explorgdb/writer/` 中其他代码均为私有实现，不构成 API；
- Reader 正式输出保持 ISO WKB-first；
- `.spx` 和 `.atx` 只提供候选，最终结果必须复核；
- MultiPatch 仍为 degraded；
- 本地自检、真实数据和正式 CI artifact 必须分开记录；
- VersionedGdbStore 当前为 Implemented / Formal acceptance blocked。
