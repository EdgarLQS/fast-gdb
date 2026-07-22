# fast-gdb 项目介绍与当前状态

## 当前定位

fast-gdb 是高性能 FileGDB Reader，不提供 FileGDB Writer。项目集中维护：

- 二进制表和索引解析；
- GeometryModel / GeometryValue；
- ISO WKB-first；
- QueryEngine / FeatureCursor；
- 属性、空间和联合查询；
- GDAL parity 与 Hybrid fallback。

## 当前产品

| 目标 | 状态 |
|---|---|
| `fast_gdb::linear` | 正式 Reader 产品 |
| `fast_gdb::hybrid` | 可选 GDAL-backed Reader 产品 |
| Writer | 不属于产品，已删除 |

## 编辑策略

调用方使用 GDAL/OpenFileGDB 或 ArcGIS 修改 GDB。受支持时序：

```text
close fast-gdb Readers
→ GDAL edit
→ GDALClose
→ reopen fast-gdb Readers
```

同一 `.gdb` 目录边写边读明确不支持。

## 当前重点

1. Reader 正确性；
2. Reader 性能；
3. `.spx/.atx` 安全解析和回退；
4. FeatureCursor 和 WKB-first；
5. 真实 FileGDB 与 GDAL parity；
6. GDAL 写后 Reader 重开兼容矩阵；
7. Windows/Linux/macOS Reader 验收。

## 已清理范围

- 自研 FileGDB Writer；
- VersionedGdbStore；
- Append/Update/Delete/Transaction/Recovery；
- Writer CMake target 和公共头；
- Writer 测试、基准、工具和工作流；
- Writer ADR、设计、roadmap 和证据文档；
- GDAL BatchWriter/Transaction 包装。

## 风险边界

- 外部 GDAL 写入期间，已有 Reader 可能读到 old/new/mixed/error；
- GDALClose 后旧 Reader 仍可能持有过期 mmap、Schema 和索引；
- 写后必须销毁并从 catalog scan 开始完整重开；
- 在线副本切换由业务系统实现；
- MultiPatch、关系、域、层级、栅格和稀疏 64-bit ObjectID 仍需专项验证。

## 验收状态

Reader-only 架构、构建目标、文档和边界测试已进入开发分支。正式验收以可用的 CI step、日志、artifact 和真实数据结果为准。
