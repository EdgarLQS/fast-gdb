# VersionedGdbStore 架构自检与 GDAL 对比文档索引

- **整理日期**：2026-07-22
- **适用分支**：`agent/versioned-gdb-store`
- **审查标签**：@深度研究
- **审查对象**：VersionedGdbStore、Reader snapshot、Writer publication、validator、recovery、generation GC
- **对照基线**：GDAL/OGR OpenFileGDB、GDAL Dataset Transaction RFC、ArcGIS File Geodatabase locking 与文件系统约束
- **当前结论**：Implemented / Formal acceptance blocked

本目录将一次深度架构自检拆成六份可独立维护的文档。它们不是 API 使用教程的替代品，而是当前实现的风险、支持边界和验收依据。

## 文档分档

| 文档 | 主要回答的问题 |
|---|---|
| [00_架构自检总览与结论.md](00_架构自检总览与结论.md) | 当前架构到底解决了什么，核心假设是什么，最重要的风险结论是什么 |
| [01_与GDAL-OpenFileGDB能力和语义对比.md](01_与GDAL-OpenFileGDB能力和语义对比.md) | VersionedGdbStore 与 GDAL/OpenFileGDB 在读写、事务、索引、Schema、FID 和部署语义上有什么根本差异 |
| [02_当前实现风险陷阱与误用清单.md](02_当前实现风险陷阱与误用清单.md) | 哪些地方最容易踩坑，错误使用会破坏哪条保证，应该如何检测或规避 |
| [03_明确不支持场景与Fail-Fast策略.md](03_明确不支持场景与Fail-Fast策略.md) | 哪些场景明确不支持、哪些暂未指定、哪些应在运行时直接拒绝 |
| [04_生产运行恢复与故障处理手册.md](04_生产运行恢复与故障处理手册.md) | ENOSPC、崩溃、CURRENT 损坏、持久性不确定、外部误改等生产事故如何处理 |
| [05_跨平台测试与正式验收矩阵.md](05_跨平台测试与正式验收矩阵.md) | 正式 Accepted 前必须补齐哪些平台、文件系统、故障注入和真实 GDB 证据 |

## 阅读顺序

1. 项目负责人和架构评审先读总览；
2. 接口设计、格式兼容和 GDAL 集成人员读能力对比；
3. 开发和代码审查人员读风险陷阱；
4. SDK 使用方和运维人员先读不支持清单；
5. 生产值守人员读故障处理手册；
6. 测试和发布负责人用验收矩阵关闭 Formal acceptance blocked。

## 三条不可弱化的结论

1. **VersionedGdbStore 是 FileGDB 的版本化发布协议，不是 ArcGIS/GDAL 全部 FileGDB 行为的透明替代。**
2. **一致性保证依赖所有访问经托管入口、可靠本地文件系统和同进程单 Writer；绕过这些前提后行为不受支持。**
3. **validator 通过只证明仓库已定义的不变量成立，不代表关系、域、层级、曲线、栅格、稀疏 64-bit ObjectID 等全部 ArcGIS/GDAL 高级语义已完整兼容。**

## 与现有权威文档的关系

- API 与调用流程：`docs/usage/11_VersionedGdbStore并发读写与版本发布.md`
- Reader 详细流程：`docs/technical/06_Reader读取流程专题.md`
- Writer 详细流程：`docs/technical/07_Writer写入与版本发布流程专题.md`
- 架构决策：`docs/adr/ADR-007-versioned-gdb-store.md`
- 限制清单：`docs/architecture/writer-known-limitations.md`

本目录中的结论应与上述文档保持一致。若实现、支持矩阵或平台验收发生变化，必须同步更新，不允许只修改其中一处。
