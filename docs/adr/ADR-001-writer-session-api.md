# ADR-001 — WriterSession 稳定会话 API

- **状态**：Accepted
- **日期**：2026-07-16
- **关联计划**：`docs/planning/18_writer跨平台测试统一与后续编辑计划.md` M18.2
- **适用范围**：空 schema 顺序批量写入与新 GDB 无覆盖发布

## 1. 背景

现有 `GdbTableWriter`、`AtomicGdbWriteSession`、`RowBuffer`、`TablxWriter` 等类型直接暴露了 FileGDB 物理布局和内部资源关系。调用方需要自行组合打开、行写入、关闭和目录发布，容易出现 Writer 已失败但仍发布、目标目录被覆盖、未提交 staging 遗留及错误上下文不足等问题。

M18.2 不增加 schema 创建、非空追加、Update、Delete 或嵌套事务，只冻结一个可安装、可迁移、可诊断的空 schema Writer 入口。

## 2. 决策

### 2.1 公共入口

稳定入口为 `explorgdb::writer::WriterSession`，通过安装目标 `fast_gdb::writer` 暴露。公共头文件仅包括：

- `writer_session.h`：会话、错误、几何类型和坐标值类型；
- `writer_index.h`：仅在 `FAST_GDB_WITH_GDAL=ON` 时安装的索引助手。

`RowBuffer`、`TablxWriter`、字段物理描述、文件头、tablx 和索引维护实现不属于稳定目标的 include surface。

### 2.2 会话生命周期

`WriterSession` 是不可复制、可移动、一次性使用的对象：

1. 调用方先创建完整但无写入/删除历史的 staging FileGDB schema；
2. `open(staging, layer)` 成功前，staging 仍由调用方所有；
3. `open()` 成功后，会话取得 staging 目录所有权；
4. 调用方按 `begin_row()` → 显式字段赋值 → 几何准备/赋值 → `end_row()` 顺序写入；
5. `commit(final)` 关闭并验证 Writer，再执行同父目录、目标不存在的原子发布；
6. `abort()` 或未提交会话析构会删除会话拥有的 staging；
7. `commit()` 成功后会话终结，不能再次写入或 abort；
8. 一个会话不能重新 `open()`，重试必须创建新会话。

### 2.3 资源所有权

- `WriterSession` 独占底层 Writer、文件句柄、行缓冲和 staging 生命周期；
- 调用方不得在会话存活期间直接操作同一 `.gdbtable`、`.gdbtablx` 或 staging 目录；
- `commit()` 只发布完整 staging 目录，不原地改写既有目标；
- 最终目标已存在时必须失败，既有目标保持不变；
- 发布失败后 staging 保留至显式 `abort()` 或析构清理。

### 2.4 错误模型

失败通过 `WriterError` 暴露，字段固定为：

- `stage`：`Open`、`Row`、`Geometry`、`Flush`、`Close`、`Publish` 或 `Abort`；
- `layer`：目标图层名；
- `path`：当前失败关联的 staging 或 final 路径；
- `system_reason`：底层 Writer、文件系统或操作系统原因；
- `message`：适合日志展示的完整文本；
- `retryable`：是否允许在同一会话直接重试。

调用方不得依赖 `message` 文本解析阶段或重试策略，应使用结构化字段。

### 2.5 可重试性

- 行、几何、flush 和 close 失败会使当前会话不可继续安全发布，`retryable=false`；
- `commit()` 在关闭后执行发布，任何发布失败均不允许在同一会话重试，`retryable=false`；
- abort 删除失败可能由临时文件锁或系统状态导致，`retryable=true`；
- open 失败发生在会话取得 staging 所有权前，调用方可修复外部条件后创建新会话，但不能复用当前对象。

### 2.6 默认值

本阶段不自动应用 schema 默认值。每个非 nullable 字段必须由调用方显式赋值；缺失字段时 `end_row()` 明确失败。该规则避免 Writer 对不同 GDAL/ArcGIS schema 默认值表示作隐式解释。

### 2.7 几何入口

稳定 API 使用 `WriterCoordinate` 和 `WriterGeometryType`，提供 Point、MultiPoint、Polyline、Polygon 及 Z/M/ZM 变体。调用方不接触内部 `GeometrySerializer` 缓冲或 FileGDB blob。

## 3. 兼容策略

旧实验 API 通过安装目标 `fast_gdb::writer_legacy` 暂时保留，并由 CMake 标记为 deprecated。其头文件安装到独立的 `fast_gdb/writer_legacy` include 目录，不污染稳定 `fast_gdb::writer` 的安装面。

本阶段不删除旧类型。迁移完成并经过独立弃用周期后，才能另行决定移除版本。

## 4. 后果

### 正面

- 调用方只需管理一个会话对象；
- 原子发布和未提交清理成为默认行为；
- 安装面不再泄露行缓冲、tablx 和物理布局实现；
- 错误可直接进入 M18 schema v2 证据；
- 无 GDAL 构建可消费稳定 Writer，会话本身不依赖 GDAL。

### 约束

- schema 仍由调用方或 GDAL/ArcGIS 预先创建；
- 会话失败后需 abort 并重建 staging，不能原地恢复；
- 非空追加、Update、Delete、事务和崩溃恢复仍被拒绝；
- 索引创建仅在 GDAL 构建中提供。

## 5. 验收

- 无 GDAL 安装包可编译运行 `fast_gdb::writer` consumer；
- GDAL 安装包可编译链接 `writer_index.h`；
- `WriterSession` 完成空 schema 写入、GDAL 重开和无覆盖发布；
- open/publish 失败返回阶段、图层、路径和底层原因；
- abort 和析构删除未提交 staging；
- `fast_gdb::writer_legacy` 消费项目继续编译。
