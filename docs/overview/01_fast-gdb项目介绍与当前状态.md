# fast-gdb 项目介绍与当前状态

## 当前定位

fast-gdb 是高性能 FileGDB Reader，不提供受支持的 FileGDB Writer 产品。项目集中维护：

- 二进制表和索引解析；
- GeometryModel / GeometryValue；
- ISO WKB-first；
- QueryEngine / FeatureCursor；
- 属性、空间和联合查询；
- GDAL parity 与 Hybrid fallback；
- 规划中的 Adaptive Reader 写入活动观测、源变化检测和 fresh GDAL 只读恢复。

## 当前产品

| 目标 | 状态 |
|---|---|
| `fast_gdb::linear` | 正式 Reader 产品 |
| `fast_gdb::hybrid` | 可选 GDAL-backed Reader 产品 |
| `fast_gdb::adaptive` | Proposed / 未实现；ADR-008 验收后才能成为可选 Reader 产品 |
| Writer | 不属于产品，已从构建、安装和兼容性范围删除 |
| `src/edgar/usegdal` | 非产品、非构建的 GDAL/OGR 历史参考代码 |

## 当前编辑合同

调用方使用 GDAL/OpenFileGDB 或 ArcGIS 修改 GDB。当前唯一受支持时序：

```text
close fast-gdb Readers
→ GDAL edit
→ GDALClose
→ reopen fast-gdb Readers
```

同一 `.gdb` 目录边写边读明确不支持。ADR-008 当前只是规划如何将重叠访问转换为 `SourceBusy`、结果丢弃和写后读取恢复，不代表并发读写已经受支持。

## Proposed Adaptive Reader

目标行为：

```text
稳定源
→ fast-gdb 快路径

writer_active=true
→ 不调用 fast-gdb
→ 不调用 GDAL fallback
→ SourceBusy

读取前后 generation 或文件快照变化
→ 丢弃结果
→ ReaderExpired / SourceChangedDuringRead

写入结束且源稳定
→ fresh GDALOpenEx(READONLY)
→ 完整物化
→ GDALClose
→ 再验证源未变化
→ 返回 GDAL 结果
```

协调模式通过调用方提供的 `writer_active/generation` 建立确定性合同。ArcGIS、QGIS 或第三方 GDAL 等无协调 Writer 只能做 best-effort 文件变化检测，不能承诺绝对发现。

## `usegdal` 参考层

`src/edgar/usegdal` 保留 datasource、dataset、recordset、query、transaction 和 batch-write 等早期包装探索，便于后续设计比较和独立研究。

该目录：

- 不由根 CMake 构建；
- 不安装、不导出；
- 不进入 package consumer 或发布门禁；
- 不提供 API/ABI、事务、并发、性能或正确性保证；
- 其中的写示例不构成 fast-gdb Writer 支持声明；
- 不作为 Adaptive Reader 的 GDAL recovery 实现依赖。

## 当前重点

1. Reader 正确性；
2. Reader 性能；
3. `.spx/.atx` 安全解析和回退；
4. FeatureCursor 和 WKB-first；
5. 真实 FileGDB 与 GDAL parity；
6. GDAL 写后 Reader 重开兼容矩阵；
7. ADR-008 Phase 0～Phase 1：合同、测试骨架、跨平台文件快照；
8. Windows/Linux/macOS Reader 验收。

## 已清理范围

- `include/fast_gdb/writer` 和 `src/edgar/explorgdb/writer`；
- VersionedGdbStore；
- Append/Update/Delete/Transaction/Recovery 产品 API；
- Writer CMake target 和公共头；
- Writer 测试、基准、工具和工作流；
- Writer ADR、设计、roadmap 和证据文档；
- `usegdal` 从正式构建、安装、导出和 release gate 中移除，但源代码作为 reference only 保留。

## 风险边界

- 外部 GDAL 写入期间，已有 Reader 可能读到 old/new/mixed/error；
- GDALClose 后旧 Reader 仍可能持有过期 mmap、Schema 和索引；
- 写后必须销毁并从 catalog scan 开始完整重开；
- 未实现 ADR-008 前，不能依赖自动检测或 GDAL fallback 恢复；
- 无协调模式即使实现，也不能保证捕获所有 Writer；
- 在线副本切换由业务系统实现；
- MultiPatch、关系、域、层级、栅格和稀疏 64-bit ObjectID 仍需专项验证；
- reference-only 代码可能老化，不等同于可构建或可生产使用。

## 验收状态

Reader-only 架构、构建目标、文档和边界测试已进入开发分支。ADR-008 与计划 22 已完成设计文档，但没有运行时代码、target 或测试通过声明。正式验收以可用的 CI step、日志、artifact 和真实数据结果为准。

## 相关文档

- [ADR-007：Reader-only 与 GDAL 编辑边界](../adr/ADR-007-reader-only-gdal-edit-boundary.md)
- [ADR-008：Adaptive Reader 写入检测与 fresh GDAL 只读回退](../adr/ADR-008-adaptive-reader-write-detection-gdal-fallback.md)
- [Adaptive Reader 实施计划](../planning/22_AdaptiveReader写入检测与GDAL回退计划.md)
