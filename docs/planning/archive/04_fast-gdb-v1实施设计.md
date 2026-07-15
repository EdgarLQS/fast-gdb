# 04 — fast-gdb v1 实施说明与合并状态（历史）

**文档状态**：📚 已合并实施记录  
**实施分支**：`feature/fast-gdb-plan`（已合并 `main`）  
**当前状态入口**：[00_规划文档状态索引.md](../00_规划文档状态索引.md)

## 1. 本文用途

本文记录 v1 只读主路径的实施结果。原分支已经合并，因此原“合并前剩余门禁”不再是当前待办。当前发布结论统一见 [13_fast-gdb最终等价与发布验收报告.md](../../evidence/13_fast-gdb最终等价与发布验收报告.md)。

## 2. v1 已完成内容

### 2.1 字段物理布局

- `field_layout.h` 统一字段物理宽度和跳过语义。
- `DateTimeWithOffset` 按 `double + int16` 消费 10 字节。
- `peek_geometry_blob()` 使用统一跳过逻辑。
- `read_record_by_fid()`、全量记录读取和 `sequential_scan()` 共享固定字段宽度规则。
- 历史 peek 重复实现和 CMake 符号重命名兼容层已删除。
- 已有 `DateTimeWithOffsetBeforeGeometry_*` 自动化测试。

### 2.2 CatalogResolver

- 通过 `GDB_SystemCatalog` 建立大小写不敏感映射。
- 通过表名定位 `GDB_SpatialRefs`、`GDB_Items` 和用户表。
- 不依赖固定业务表编号。

### 2.3 CapabilityReport

- 已成为图层能力判断入口。
- 报告 SRS、曲线、MultiPatch、Raster、空间索引和属性索引状态。
- `.spx` 缺失或解析失败时可明确降级。

当前补充说明：MultiPatch 在代码中仍被标记为 supported，但实际只提供部分语义，当前发布收口应调整定级或实现。

### 2.4 SRS 和元数据

v1 当时完成：

- WKT、WKID、LatestWKID、SRSName。
- 图层 Definition XML 基础读取。

后续阶段已经补齐：

- coded/range domain。
- Metadata item 风格接口。
- Feature Dataset 摘要。
- relationship summary 和 definition。

坐标转换和重投影仍不在 fast-gdb reader 内实现。

### 2.5 QueryEngine

- 已封装 `open/read_by_fid/scan/query_bbox`。
- 已扩展属性索引和 WHERE 子集入口。
- 空间查询优先使用 `.spx`，缺失或失败时明确使用顺序过滤。
- 生产路径没有 GDAL fallback。

## 3. v1 当时的非目标及后续状态

| 原非目标 | 当前状态 |
|----------|----------|
| GDAL 运行时 fallback | 继续不实施 |
| 写入生产化 | writer 主数据直写已实现；系统表更新仍未完成 |
| MultiPatch 标准 WKT | 已有标准 WKT 语法；完整 part type 语义仍未实现 |
| 曲线标准化 | 当前版本仍不实施 |
| 完整 GDB_Items XML | 常用 domain/relationship/Feature Dataset 已补齐 |
| 坐标重投影 | 继续由上层 GDAL / PROJ 处理 |

## 4. 原合并门禁状态

原分支已经合并，因此以下项目仅作为历史记录：

- 配置和构建。
- v1 专项、smoke、系统表测试。
- 同步当时的 `main`。
- 检查分支改动范围。

这些门禁不应继续以未勾选项目出现在当前发布清单中。

## 5. 当前遗留问题

v1 合并并不代表当前 reader 已完成发布验收。当前仍需：

1. 修正 General Curve flag/header 规则。
2. 用普通真实 FileGDB 执行回归。
3. 用真实曲线 FileGDB 验证明确 unsupported。
4. 修正 MultiPatch capability 定级或补齐 part type 语义。
5. 增加 GeneralMultiPoint 独立测试。

详见 [07_fast-gdb-v2后续统一计划.md](07_fast-gdb-v2后续统一计划.md)。
