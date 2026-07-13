# 14 — fast-gdb 大规模空间查询超越 GDAL 计划

**更新日期**：2026-07-13  
**状态**：实施中  
**适用范围**：`explorgdb` Reader 的 bbox/空间查询路径

## 1. 目标

本计划不再以“小数据或小窗口局部领先”为完成标准。最终目标是在真实大规模 FileGDB 上，对 10M 及以上要素的中高覆盖率 bbox 查询稳定快于原生 GDAL，同时保持结果集合和几何语义一致。

硬性验收目标：

| 场景 | 目标 |
|---|---:|
| 1% / 10% 覆盖率 | 不慢于 GDAL，且不得相对现有 fast-gdb 回归超过 5% |
| 30% 覆盖率 | `fast-gdb <= GDAL × 0.90` |
| 80% 覆盖率 | `fast-gdb <= GDAL × 0.80` |
| 接近全范围 | `fast-gdb <= GDAL × 0.80` |
| 结果正确性 | 与 GDAL 的完整 0-based FID 集合完全一致 |
| 稳定性 | 连续多轮中位数和 P95 均满足目标，不挑选最快单次 |

性能结论必须来自 Release 构建和真实数据，不以估算代替验收。

## 2. 当前基线与根因

2026-07-13 使用 `test_data/large_10m/large_10m_test.gdb`（约 1.9 GB）复测：

| 项目 | 结果 |
|---|---:|
| Large 窗口候选数 | 8,172,990 |
| fast-gdb 总耗时 | 40,881.4 ms |
| GDAL component 总耗时 | 3,842.5 ms |
| 原始差距 | fast-gdb 约慢 10.6 倍 |

阶段耗时约为：`.spx` 候选查询 1.58 秒、FID 到 Geometry Blob 定位 6.99 秒、Geometry Blob bbox/相交解析 37.68 秒。核心问题不是文件打开，而是高覆盖率下对数百万要素逐条定位、完整解码并构造通用几何模型。

当前基础分支已经完成 mmap Blob 零拷贝、`.spx` 复用、候选密度切换、顺序扫描、分阶段指标和可重复基准。这些属于基础设施，不代表最终性能目标已经完成。

## 3. 正确的执行模型

### 3.1 三态 bbox 判定

每条 Geometry Blob 首先只读取包围盒，并返回：

```text
DISJOINT            -> 直接拒绝
CONTAINED_BY_QUERY  -> 直接接受，不解码 GeometryModel
BOUNDARY_CANDIDATE  -> 进入流式精确谓词
```

当要素 bbox 完全位于查询框内部时，该要素必然与查询框相交。高覆盖率窗口应让绝大多数命中要素在这一阶段直接返回，避免完整几何解码。

### 3.2 流式、无对象空间谓词

仅对跨越查询边界的少量要素解码坐标。不得默认构造完整 `GeometryModel`、ring/part/point vector 或临时拓扑对象。

实现顺序：

1. Point：解码单点后直接判断；
2. MultiPoint：流式读取，命中即返回；
3. Polyline：保留相邻两点，执行点在框内和线段-矩形相交；
4. Polygon：顶点在框内、边界相交、查询框角点 point-in-polygon；
5. Z/M/ZM：空间过滤只读取 XY，正确跳过其余维度；
6. 曲线、MultiPatch、异常编码：回退统一 `GeometryModel`，保证语义。

目标：普通 Point/Polyline/Polygon 至少 95% 不进入通用模型回退路径。

### 3.3 查询前执行计划

不能先生成 800 万候选 FID，再决定改用顺序扫描。执行计划必须在 `.spx` 大规模物化前确定：

```text
低覆盖率       -> .spx 候选 + 快速谓词
中等覆盖率     -> 分块候选或顺序扫描，按真实基准选择
高覆盖率       -> 直接 Geometry Column 顺序扫描
```

规划依据包括查询窗口与图层 extent 的面积比例、要素数、几何类型、缓存状态和历史采样统计。阈值由 1%、10%、30%、80%、全范围矩阵校准，不允许把 50% 固定启发值当最终结论。

### 3.4 专用 Geometry Column Scanner

高密度路径不得使用通用 `sequential_scan()` 解析所有字段并组装 `FieldRef` 数组。新增只读取 geometry 列的编译行布局：

```text
record length
  -> null bitmap
  -> 按预编译 skip plan 跳过非几何字段
  -> geometry blob view
  -> fast bbox predicate
```

目标是消除逐行 schema 解释、无关字段解析和回调层额外成本。

### 3.5 并行分块扫描

完成单线程快速路径后，按 FID/文件块范围并行：

- 共享只读 mmap；
- 每线程独立 decoder、统计和结果缓冲；
- 无共享 `push_back` 锁；
- 分区结果按 FID 顺序拼接；
- 默认线程数不超过物理核心和 8；
- 保留单线程模式用于回归与基准。

### 3.6 输出模式

支持：

```cpp
query_bbox_fids(...)
query_bbox_visit(..., callback)
query_bbox_count(...)
query_bbox_bitmap(...)
```

与 GDAL 对比时双方必须完成相同工作；完整 FID 基准不能拿 count-only 路径进行不公平比较。

## 4. 实施阶段

### Phase 0 — 基础设施（已完成代码）

- 可重复 1M/10M 覆盖率矩阵；
- mmap Geometry Blob 零拷贝；
- `.spx` 生命周期复用；
- 候选密度指标；
- 候选路径和顺序扫描路径；
- 与 GDAL 完整 FID 集合对照。

### Phase 1 — 三态 bbox 快速接受

- 增加 `bbox_contained` 指标；
- bbox 完全位于查询框内时直接输出 FID；
- 只有边界候选调用精确谓词；
- 新增高密度 Point/Polygon 正确性测试。

### Phase 2 — Streaming Predicate

- Point/MultiPoint；
- Polyline；
- Polygon；
- 特殊几何 fallback；
- 增加 `streaming_tested`、`model_fallback_tested` 指标。

### Phase 3 — Geometry-only Scanner

- 编译字段 skip plan；
- 顺序访问 geometry blob；
- 对比通用 `sequential_scan()` 结果；
- 记录 row scan 和 predicate 独立耗时。

### Phase 4 — 查询前规划器

- 图层 extent/采样统计缓存；
- 在 `.spx` 候选物化前选择路径；
- 高覆盖率不分配数百万候选 vector；
- 基准确定切换阈值。

### Phase 5 — 并行扫描

- 固定分区；
- 线程本地结果；
- 有序合并；
- 单线程/多线程结果与 GDAL 一致。

### Phase 6 — 性能验收

至少覆盖：

- 1M、10M 和可获得的 35GB 真实数据；
- Point、Polyline、Polygon；
- 1%、10%、30%、80%、100%；
- 冷缓存和热缓存；
- 单线程和默认并行；
- 中位数、P95、峰值 RSS、CPU cycles、instructions、branch/cache misses。

## 5. 正确性边界

- `.spx` 只负责候选，不能代替最终判断；
- bbox 完全被查询框包含时直接接受属于严格成立的空间相交结论；
- bbox 仅部分相交时不得把 bbox 相交冒充真实几何相交；
- 曲线、MultiPatch、非法几何继续走明确 fallback；
- 保持 0-based FID、Z/M/ZM、holes、islands、multipart、反向 rings 和 Hybrid 行为；
- 不修改 FileGDB 原生格式。

## 6. 完成定义

只有同时满足以下条件才可把本计划标记为完成：

1. Linux、Windows Release 构建和相关 CTest 通过；
2. 与 GDAL 的完整 FID 集合一致；
3. 30%、80%、全范围达到第 1 节目标；
4. 小范围查询无显著回归；
5. 异常/复杂几何 fallback 测试通过；
6. 实际性能数据写入证据文档；
7. PR 不再是 Draft。

在真实 10M 基准和跨平台 CI 完成前，不宣称已经超过 GDAL。

## 7. 关联证据

- [空间查询优化实施记录](../evidence/spatial-query-optimization-2026-07-13.md)
- [最终等价与发布验收报告](13_fast-gdb最终等价与发布验收报告.md)
- [性能基准与优化](../technical/01_性能基准与优化.md)
- [真实数据验收资料清单](13_fast-gdb真实数据验收资料清单.md)
