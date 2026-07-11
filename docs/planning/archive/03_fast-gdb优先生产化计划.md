# 03 — fast-gdb 优先生产化计划（历史）

**文档状态**：📚 历史计划，v1 阶段已结束  
**原实施分支**：`feature/fast-gdb-plan`  
**当前状态入口**：[00_规划文档状态索引.md](00_规划文档状态索引.md)

## 1. 原始目标

v1 的目标是建立 fast-gdb 只读主路径：

- 不把 GDAL 作为默认运行时 fallback。
- 统一字段物理布局和系统表定位。
- 建立 `CapabilityReport`、`MetadataReader` 和 `QueryEngine`。
- 让不支持能力明确进入 degraded / unsupported。

该目标已经完成并合入 `main`。本文件保留原阶段设计和结果摘要，不再作为当前待办来源。

## 2. 实际完成结果

| 原计划任务 | 最终状态 | 结果 |
|------------|:---:|------|
| 字段宽度和跳过规则统一 | ✅ | `field_layout`、`fixed_physical_width()`、`skip_field_value()` 已统一主要路径 |
| DateTimeWithOffset 10 字节物理布局 | ✅ | 不再导致后续字段错位；offset 尚未独立暴露 |
| nullable bitmap 旧记录兼容 | ✅ | schema 扩容时缺失新增字段返回 null |
| CatalogResolver | ✅ | 通过系统表名称定位，不依赖固定表号 |
| CapabilityReport | ✅ | 已暴露 SRS、曲线、MultiPatch、Raster、索引状态 |
| SRS 元数据 | ✅ | WKT/WKID/LatestWKID/SRSName |
| QueryEngine | ✅ | FID、扫描、bbox、属性索引、WHERE 子集 |
| 图层 XML | ✅ | Definition/Documentation 等已可读取 |
| 字段域和关系类 | ✅ | 后续阶段补齐并已合入 |
| 曲线检测 | ⚠️ | 显式 unsupported 已有；Curve flag/header 判断仍需修正 |
| MultiPatch | ⚠️ | 已输出标准 WKT 语法，但 part type 语义未完整保留 |
| 真实样本准入 | 🧪 | 测试入口已新增，尚待本地真实数据执行 |

## 3. 原阶段与当前归属

### Phase 1：安全基础

已完成：

- 字段物理宽度统一。
- nullable bitmap 兼容。
- 系统表名称解析。
- capability 入口。

遗留到当前发布收口：

- General Curve flag `0x20000000` 的正确判断。
- 真实普通/曲线 GDB 回归。

### Phase 2：只读生产缺口

已完成：

- SRS 和 XML 元数据。
- QueryEngine。
- `.spx` / `.atx` 查询入口。
- Raster 字段 capability 标记。

部分完成：

- MultiPatch 只完成 WKT 语法表达，没有完整恢复 part type 语义。

### Phase 3：生产准入

已完成：

- 自动化专项和集成测试入口。
- 文档化 capability/fallback reason。

尚待当前分支完成：

- 普通真实 FileGDB 回归。
- 真实曲线 FileGDB 边界回归。
- General header 修正。
- MultiPatch capability 定级统一。

## 4. 当前不再沿用的内容

以下内容属于当时的实施顺序，不能继续理解为当前待办：

1. “先实现 CatalogResolver”——已完成。
2. “再实现 CapabilityReport”——已完成。
3. “再实现 SRS”——已完成。
4. “再封装 QueryEngine”——已完成。
5. “字段域和关系类放到未来”——后续已经完成。
6. “MultiPatch 标准 WKT 等于完整支持”——当前重新定级为部分支持。

## 5. 当前文档入口

- 当前项目状态：[01_项目状态与规划.md](01_项目状态与规划.md)
- 当前能力矩阵：[02_GDAL功能对比矩阵.md](02_GDAL功能对比矩阵.md)
- 当前关闭计划：[07_fast-gdb-v2后续统一计划.md](07_fast-gdb-v2后续统一计划.md)
- 发布验收：[08_fast-gdb只读发布收口.md](08_fast-gdb只读发布收口.md)
- 曲线分析：[09_fast-gdb曲线几何分析.md](09_fast-gdb曲线几何分析.md)

## 6. 历史设计原则（继续有效）

- GDAL 用作测试 oracle 和人工对照，不作为默认运行时架构。
- 系统表按名称解析，不写死 `aXXXXXXXX`。
- 字段读取和跳过规则必须共享统一物理布局。
- 无法完整表达的数据必须明确降级或拒绝。
- 新能力必须同时更新 capability、测试和文档。
