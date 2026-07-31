# fast-gdb 与 GDAL/OpenFileGDB 功能对比矩阵

## 定位

fast-gdb 负责本地 FileGDB 高性能读取；GDAL/OpenFileGDB 负责 FileGDB 编辑，并作为正确性对照和可选读取 fallback。

v0.2.0 的统一入口把本地路径、S3 路径和已知能力缺口路由到合适后端。统一 SDK 和
本地插件已实现；S3 尚未取得真实环境支持证据。

ADR-008 已实现一个 Reader-only Adaptive 编排层：观察 Writer 活动和 generation，校验数据源变化，并在写入结束且数据源稳定后使用 fresh GDAL 只读连接恢复。跨平台、压力、性能、多 GDAL 版本和安装包证据仍待补齐。

| 能力 | fast-gdb | GDAL/OpenFileGDB | 结论 |
|---|---|---|---|
| 打开 FileGDB | 是 | 是 | fast-gdb 主读路径 |
| 顺序扫描 | 是 | 是 | 性能与校验对照 |
| FID 随机读取 | 是 | 是 | 需明确 FID 映射 |
| 属性 WHERE | 是 | 是 | GDAL parity |
| bbox/空间查询 | 是 | 是 | 候选后精确复核 |
| `.spx` 读取 | 是 | 是 | fast-gdb 自研读取 |
| `.atx` 读取 | 是 | 是 | fast-gdb 自研读取 |
| ISO WKB | 是 | 是 | fast-gdb 正式输出 |
| 曲线 | 内置线性化/Hybrid | 是 | 复杂场景 fallback |
| MultiPatch | degraded | 较完整 | 专项 profile |
| Writer 活动/generation 观察 | 已实现，可选只读消费调用方信号 | 非 fast-gdb 管理职责 | 协调模式确定性 Busy/失效 |
| 无协调外部 Writer 变化检测 | best-effort | 无统一跨 Writer 合同 | 不保证绝对发现 |
| 写后稳定源读取恢复 | 已实现，可选 fresh GDAL 编排 | read-only Dataset | 必须 fresh open/close 和前后校验 |
| CreateFeature | 否 | 是 | 交给 GDAL |
| SetFeature | 否 | 是 | 交给 GDAL |
| DeleteFeature | 否 | 是 | 交给 GDAL |
| Create/Delete Field | 否 | 是 | 交给 GDAL |
| Create/Delete Index | 否 | 是 | 交给 GDAL |
| REPACK | 否 | 是 | 交给 GDAL；写后重开 |
| 事务 | 否 | 模拟/驱动能力 | 不进入 fast-gdb 产品 |
| 在线版本发布 | 否 | 否 | 业务层能力 |
| 统一 `Dataset/Group/Layer/Feature/Cursor` 入口 | 已实现 | GDAL/fast-gdb Adapter | 拥有型模型和冻结 Schema |
| 本地 `.gdb` 自动后端选择 | 已实现 | 原生读取 | `Auto` 默认优先 fast-gdb |
| `s3://` / `/vsis3/` 读取 | 否 | 依赖 GDAL VSI/OpenFileGDB | 需凭据、目录枚举和真实数据验收 |
| `FastFileGDB` GDAL/OGR 兼容入口 | 已实现、只读 | OpenFileGDB 原生驱动 | 独立驱动名，不替换官方驱动 |
| fast-gdb 查询计划和原始记录诊断 | 已实现 | GDAL 常规诊断 | `explain` 与 FastOnly extension |

## 当前读写阶段规则

### 支持

```text
fast-gdb close all Reader state
→ GDAL update
→ GDALClose
→ fast-gdb full reopen
```

### 不支持

```text
fast-gdb Reader open
+ GDAL update same .gdb
```

并发期间 old/new/mixed/error 均可能出现。未协调外部 Writer 仍不能依赖自动检测或
GDAL recovery；只有显式同进程协调和 fresh fallback 合同可被 Adaptive 识别。

## Adaptive 行为

```text
writer_active=true
→ SourceBusy
→ fast/GDAL 两个读取后端均不调用

generation 或依赖文件在读取前后变化
→ 丢弃结果
→ ReaderExpired / SourceChangedDuringRead

writer_active=false 且源稳定
→ fast-gdb 成功则返回 fast 结果
→ fast 不支持或失败时使用 fresh GDAL read-only
→ GDALClose 后再次验证源稳定
→ 稳定才返回 GDAL 结果
```

约束：

- 不包装 GDAL update；
- 不创建或删除 writer marker；
- 不在 Writer 活动期间通过 GDAL 强行返回数据；
- 不复用曲线回退中的 thread-local Dataset；
- 无协调模式必须标记 best-effort。

## 统一访问路由（已实现；S3 未验收）

```text
fast_gdb::unified::Dataset::open(uri, Auto)
  ├─ 本地 .gdb → fast-gdb
  ├─ s3://... / /vsis3/... → GDAL OpenFileGDB
  └─ fast-gdb 不支持的只读能力 → fresh GDAL OpenFileGDB
```

`FastOnly` 不得静默调用 GDAL；`GdalOnly` 不得创建 fast-gdb Reader。一次 Cursor 的后端
必须在开始输出前确定，已发布部分结果后不得切换。

## GDAL 写后 Reader 兼容矩阵

| GDAL 操作 | fast-gdb 重开后验证 | Adaptive 计划验证 |
|---|---|---|
| CreateFeature | 记录数、FID、字段、几何 | active 时 Busy；结束后 fresh GDAL/新 Reader 看到新要素 |
| SetFeature | 新字段值、几何、属性/空间索引 | 读取中变化丢弃；稳定后看到完整新值 |
| DeleteFeature | 删除槽、扫描、FID lookup | 不返回删除前后 mixed 集合 |
| CreateField | Schema、nullable、record layout | generation 变化使旧 Schema/Reader 过期 |
| DeleteField | 字段偏移和记录解析 | 旧 Reader 不得继续解码新布局 |
| CreateIndex | `.gdbindexes/.atx` 解析和查询 | 索引依赖变化使结果作废 |
| DeleteIndex | 安全回退 | 新 generation 重新规划查询 |
| REPACK | tablx、row offset、FID 语义 | file ID/size/mtime 变化；无崩溃；旧 mmap 失效 |
| Recompute extent | bbox 和图层范围 | 空间依赖变化后重新打开 |

## 测试解释

- GDAL parity 用于验证 Reader；
- GDAL 生成数据不表示 fast-gdb 提供写入；
- 同目录重叠测试只记录可见性类别；
- 任何单平台 old/new 结果都不是并发支持证明；
- 当前正式门禁只覆盖完整关闭和重开后的正确性；
- Adaptive 测试验证已实现的同进程协调和本地 fresh fallback；未协调重叠测试仍只是 characterization；
- 协调模式目标是完整旧、完整新或 Busy；无协调模式不能建立普遍检测保证。

## 相关文档

- [ADR-007：Reader-only 与 GDAL 编辑边界](../adr/ADR-007-reader-only-gdal-edit-boundary.md)
- [ADR-008：Adaptive Reader 写入检测与 fresh GDAL 只读回退](../adr/ADR-008-adaptive-reader-write-detection-gdal-fallback.md)
- [Adaptive Reader 实施计划](22_AdaptiveReader写入检测与GDAL回退计划.md)
- [统一访问与 GDAL/S3 路由计划](24_fast-gdb统一访问与GDAL_S3路由计划.md)
- [ADR-009：统一 FileGDB 访问与 GDAL/S3 路由](../adr/ADR-009-unified-filegdb-routing.md)
