# 15 — fast-gdb 全规模空间查询性能优化计划

**更新日期**：2026-07-14
**适用分支**：`agent/spatial-query-optimization`
**依据文档**：`docs/evidence/spatial-query-acceptance-test-plan.md`
**目标**：在保持完整 FID 集合与 GDAL 一致的前提下，优先解决大数据空间查询的绝对耗时差距，并让 30%、80% 和全范围查询稳定超过 GDAL。

**状态**：已归档；Phase A、E、G 已完成，B/C/D/F 未触发。

本文档曾取代 `14_fast-gdb空间查询优化计划.md` 中的性能优先级与验收门槛，现作为 Phase A–G 历史实施记录归档。Phase H 历史计划见 [16_spatial-query-scale-optimization-plan.md](16_spatial-query-scale-optimization-plan.md)，当前工作见 [项目状态与规划](../01_项目状态与规划.md)。

---

## 1. 当前结论

当前实现已经具备：

- mmap Geometry Blob 零拷贝视图；
- `.spx` 候选查询与缺失、损坏回退；
- 高覆盖率 Geometry-only 顺序扫描；
- bbox 粗过滤和 contained 直接接受；
- active feature count 与物理 FID slot 分离；
- 1M、10M 覆盖率矩阵及 GDAL 完整 FID 对照。

### 1.1 方向性复测（旧数据，2026-07-14 早期）

该数据来自未设置 `CMAKE_BUILD_TYPE` 且开启细粒度 profile 的构建，只用于定位热点，**不能作为最终 Release 性能结论**。

| 规模 | 1% | 10% | 30% | 80% | 全范围 |
|---|---:|---:|---:|---:|---:|
| 1M fast/GDAL | 3.85 | 2.65 | 1.56 | 0.74 | 0.62 |
| 10M fast/GDAL | 8.42 | 2.29 | 1.41 | 0.72 | 0.67 |

### 1.2 Release 基线（最新，物理单位 ms，2026-07-14）

经过 Phase A 公平基准构建，以下为 Release (`-O3 -DNDEBUG`) + profile OFF + 稳态 + 中位数 of 5 的真实数据。

> **更新（2026-07-14）**：Point、MultiPoint、Polyline、Polygon 的 1K/10K/100K/1M/10M 稳态矩阵已全部完成并通过完整 FID 对照与分档性能门槛。详细原始中位数见 `docs/evidence/spatial-query-baseline-2026-07-14.md`。

**10M steady-state (profile OFF), 中位数 of 5:**

| 覆盖率 | fast_med | gdal_med | fast/gdal | 验收目标 | 结果 |
|------:|--------:|--------:|---------:|---------|:---:|
| 1% | 54.0 ms | 125.3 ms | **0.431** | ≤ +200ms 或 ≤ 0.90× GDAL | ✅ |
| 10% | 185.1 ms | 607.8 ms | **0.305** | ≤ +200ms 或 ≤ 0.90× GDAL | ✅ |
| 30% | 320.2 ms | 1492.0 ms | **0.215** | ≤ 0.90× GDAL | ✅ |
| 80% | 314.3 ms | 3643.1 ms | **0.086** | ≤ 0.90× GDAL | ✅ |
| 100% | 237.5 ms | 2930.0 ms | **0.081** | ≤ 0.90× GDAL | ✅ |

**1M steady-state (profile OFF), 中位数 of 5:**

| 覆盖率 | fast_med | gdal_med | fast/gdal |
|------:|--------:|--------:|---------:|
| 1% | 7.0 ms | 12.5 ms | 0.559 |
| 10% | 24.9 ms | 59.4 ms | 0.420 |
| 30% | 44.5 ms | 139.1 ms | 0.320 |
| 80% | 31.5 ms | 328.0 ms | 0.096 |
| 100% | 24.1 ms | 288.0 ms | 0.084 |

**关键发现**：

1. **全几何、全规模 steady-state 的所有覆盖率均已通过验收标准**；Polygon 的 10M fresh-open 缓存矩阵和 1% 冷打开复测也已通过验收。
2. **旧数据为测量伪影**：profile 开销在 30% 达 +51%，80% 达 +73%，100% 达 +104%。
3. **Phase E 已校准**：将 direct scan 默认估算覆盖率阈值由 35% 下调到 29%，使实际约 29.76% 的 30% 窗口选择顺序扫描。
4. **profile OFF 下的绝对差距极小**：最慢的 30% 也仅 320ms (vs GDAL 1492ms)。
5. steady-state 下 Phase B/C/D/F 均不触发；**Phase G（TablxCache）** 已完成——Polygon 的 10M fresh-open 1% 从 1444.4ms 降至 92.8ms，通过所有当前验收标准。
6. 当前 fresh-open 完整矩阵仅覆盖 Polygon；Point、MultiPoint、Polyline 的 fresh-open 性能仍不作完成声明。
- 方向性复测（旧数据，2026-07-14 早期）
- [当前结论](../../evidence/spatial-query-baseline-2026-07-14.md)

---

## 2. 目标范围与验收原则

### 2.1 首轮覆盖范围

- 几何：Point、MultiPoint、Polyline、Polygon；
- 规模：1K、10K、100K、1M、10M；
- 窗口：1%、10%、30%、80%、全范围；
- 输出：完整、唯一、递增的 0-based FID 集合；
- 曲线和 MultiPatch：保持正确 fallback，首轮不设置性能门槛。

### 2.2 性能门槛

小范围查询采用绝对耗时优先原则：

```text
1% / 10%：
  fast_ms <= gdal_ms + 200ms
  或 fast_ms <= gdal_ms * 0.90

30% / 80% / 全范围：
  fast_ms <= gdal_ms * 0.90
```

补充要求：

- 只有小范围绝对差距超过 200 ms，且主要发生在大数据上，才进入专项优化；
- 所有性能单元必须保持完整 FID 集合一致；
- 10M 已领先场景不得回归；
- steady-state 与 fresh-open 均使用上述分档门槛，小范围查询都允许 200 ms 绝对差距；
- P95 作为长尾观察指标记录，首轮不作为硬门禁。

### 2.3 优化优先级

按“可减少的绝对耗时”排序，不按单一倍率排序：

1. 10M 低密度候选 Blob 访问；
2. 10M 中密度 `.spx` 遍历与候选读取；
3. 30% 边界候选精确判断；
4. 80% 和全范围扫描吞吐；
5. 小数据固定开销。

---

## 3. Phase A — 建立公平性能基线

先修正测量体系，再根据 Release 数据决定具体优化。

### 3.1 构建与计时

- 新建独立 Release 构建，明确使用 `-O3 -DNDEBUG`；
- 墙钟性能门禁关闭 `FAST_GDB_SPATIAL_PROFILE`；
- profile 仅在独立诊断轮次开启，不与 GDAL 墙钟比值混用；
- 记录编译器、GDAL 版本、CPU、内存、操作系统和数据集信息。

### 3.2 比较口径

分别记录两种口径：

1. `steady-state`：fast-gdb 与 GDAL 均在计时前完成打开和预热；
2. `fresh-open`：双方均把打开、查询、结果读取和关闭纳入计时。

双方必须输出并比较完整 FID 集合，不能使用 count 对 FID 或已打开对重复打开等不对称口径。

### 3.3 重复策略

- 每个单元预热一次、测量五次；
- 轮换查询顺序，避免 1% 固定承担首次缺页成本；
- 使用五次中位数判定，记录 P95；
- profile 轮次输出 SPX、Blob、bbox、exact、scan 和结果写入耗时。

---

## 4. Phase B — 候选 Geometry 定位

触发条件：Release/profile-off 基线中，10M 的 1% 或 10% 查询慢于 GDAL 超过 200 ms，且 Blob 定位或 mmap 缺页是主要耗时。

### 4.1 访问模式提示

当前 `.gdbtable` mmap 在打开时使用 `MADV_SEQUENTIAL`，会影响低密度候选的随机页访问。调整为：

- 打开时使用普通访问模式；
- `.spx` 候选路径使用随机访问提示；
- 顺序扫描路径使用顺序访问提示；
- 平台不支持提示时保持现有正确性，不改变执行语义。

### 4.2 编译行布局

在 `GdbTableParser` 内编译一次 Geometry 前缀布局：

```cpp
struct GeometryRowLayout {
    size_t nullable_bitmap_size;
    int geometry_nullable_bit;
    std::vector<FieldSkipOp> prefix_ops;
};
```

要求：

- 标准行直接读取 nullable bitmap 并跳到 Geometry；
- 不读取 Geometry 后面的字段；
- 不为每条记录反复尝试 bitmap/present-bit 组合；
- `peek_geometry_blob()` 与 `scan_geometry_blobs()` 共享同一布局规则；
- 非标准、截断或无法确认的记录回退现有容错解析器；
- mmap 路径继续返回稳定的零拷贝 Blob 视图。

### 4.3 批量候选接口

为递增候选 FID 增加内部批量遍历能力，集中完成：

- offset 与行长度边界检查；
- Geometry 定位；
- bbox 和精确谓词调用；
- 指标累计与提前终止。

不新增调用方必须理解的公共查询接口，`query_bbox_unified()` 保持兼容。

---

## 5. Phase C — `.spx` 与中密度候选

触发条件：Phase B 后 10M 的 10% 或 30% 仍未达标，且 `.spx` 页面遍历或中密度候选读取是主要耗时。

### 5.1 页面访问

- 不在每次查询开始时无条件清空根页和分支页缓存；
- 增加访问页数、缓存命中、原始候选数和去重候选数指标；
- 合并同一 grid level 的有序查询区间；
- 避免每个 X cell 都从根页重复递归；
- 一次查询中每个相关 B+Tree 页面最多解析一次。

### 5.2 BlockCandidates

30% 等中密度场景增加分块候选路径：

```cpp
enum class SpatialPlan {
    SpxCandidates,
    BlockCandidates,
    SequentialScan,
    ParallelScan
};
```

`BlockCandidates` 必须：

- 保留 `.spx` 产生的候选集合；
- 按相邻 FID、文件偏移和 mmap 页范围组织访问；
- 只判断候选，不退化为全表扫描；
- 保持最终 FID 唯一、递增；
- 单独记录 block 数量和预取字节数。

提供以下测试开关用于对照，不作为业务接口：

```text
FAST_GDB_SPATIAL_FORCE_PLAN=spx|block|scan|parallel
```

---

## 6. Phase D — Streaming Predicate

触发条件：Phase B/C 后 30% 查询仍未达标，且 `exact_filter_ms` 或 `model_fallback_tested` 证明完整模型构建是主要剩余耗时。

对 bbox 边界候选实现无分配流式相交判断：

- Point：直接解码 XY；
- MultiPoint：bbox 三态后逐点判断；
- Polyline：逐顶点、逐线段检查矩形相交；
- Polygon：检查 ring 边相交并使用 parity 判断包含关系；
- Z/M 数据按格式跳过，不创建坐标数组；
- 命中或拒绝后立即停止读取剩余坐标。

以下情况统一返回 `Fallback`，继续使用现有 `GeometryModel`：

```text
General curve geometry
MultiPatch
未知类型
异常 varint
超限 point/part count
数值溢出
无法确认的 polygon 拓扑
```

新增指标：

```text
streaming_tested
streaming_accepted
streaming_rejected
model_fallback_tested
streaming_invalid
```

---

## 7. Phase E — 确定性规划器

触发条件：至少两种执行路径已经通过正确性验证，且 Release 矩阵证明不同规模或覆盖率存在稳定交叉点。

只使用当前查询和表级静态信息，不在首轮引入在线学习或历史 EMA：

```text
小表       -> 固定开销高于扫描成本时直接扫描
低密度     -> SPX + 批量候选定位
中密度     -> SPX + BlockCandidates
高覆盖率   -> Geometry-only 顺序扫描
索引异常   -> 安全顺序扫描或候选 FID fallback
```

决策输入限定为：

- active feature count；
- estimated coverage；
- `.spx` 候选比例；
- Geometry 类型；
- mmap 可用性；
- 平均记录大小。

规划错误只能影响性能，不能影响结果或 fallback 语义。

---

## 8. Phase F — 条件并行扫描

并行扫描不是默认必做项。仅当 Release 单线程优化完成后，10M 的 80% 或全范围仍不能达到 `fast <= GDAL * 0.90` 时实施。

实施要求：

- 按连续 FID 范围分区；
- 共享只读 mmap，每线程独立 decoder、metrics 和结果 vector；
- 不使用逐要素锁；
- 按分区顺序拼接结果，无需最终排序；
- 线程创建失败时回退单线程；
- 小数据和低密度查询不启用并行。

---

## 9. Phase G — `.gdbtablx` 跨 open 元数据缓存

触发条件：10M fresh-open 的 1% 或 10% 因 `.gdbtablx` 重复解析未达小范围 200ms 门槛，且稳态查询本身已达标。

### 9.1 实施内容

已在 `GdbTableParser::load_tablx()` 中集成 `TablxCache`，进程内单例：

- **缓存键** = {device, inode, 文件大小, mtime 纳秒}，文件变更自动失效；
- **LRU 淘汰**：上限 16 个条目、总缓存 256 MiB，超出时淘汰最久未使用；
- **线程安全**：LRU 更新使用独占锁，避免共享锁下修改链表；
- **绕过**：`FAST_GDB_TABLX_CACHE=0` 可获取真正的冷打开基线；
- **深拷贝**：缓存命中时深拷贝 `feature_offsets_` 向量，避免引用悬挂；
- **回退安全**：`stat()` 失败、文件损坏、内存分配失败时回退原有解析路径。

### 9.2 性能效果

10M fresh-open Polygon 全矩阵（进程内重复 open，中位数 of 5）：

| 模式 | fast_med | gdal_med | ratio | 验收 |
|:----|:--------:|:--------:|:-----:|:----:|
| 有缓存 1% | 92.8 ms | 126.3 ms | 0.735 | ✅（-33.5ms） |
| 有缓存 10% | 228.8 ms | 605.4 ms | 0.378 | ✅ |
| 有缓存 30% | 404.5 ms | 1438.1 ms | 0.281 | ✅ |
| 有缓存 80% | 391.8 ms | 3413.1 ms | 0.115 | ✅ |
| 有缓存 100% | 314.8 ms | 2909.7 ms | 0.108 | ✅ |
| 冷打开 1% | 154.9 ms | 124.5 ms | 1.245 | ✅（+30.4ms < 200ms） |

### 9.3 测试

- 命中/未命中/相同键独立/淘汰/文件变更失效/纳秒级等长重写 —— 8 个单元测试 ✅
- 8 线程并发读写 —— 无死锁、无数据损坏 ✅
- 绕过标志 `FAST_GDB_TABLX_CACHE=0` ✅
- 1M/10M steady-state 无回归 ✅
- 10M Polygon fresh-open 全矩阵（1%/10%/30%/80%/100%）通过 ✅
- 10M Polygon 冷打开 1% 通过 +200ms 容忍 ✅
- 全量 CTest 481/481 通过 ✅

---

## 10. 测试与门禁

### 10.1 正确性

- 编译布局与旧 Geometry 定位器逐记录差分；
- nullable Geometry、Geometry 前变长字段、DateTimeWithOffset、删除 FID、截断行；
- mmap 不可用、`.spx` 缺失或损坏；
- holes、multipart、反向 ring、Z/M/ZM；
- 曲线和 MultiPatch fallback；
- 四类常规几何至少 1000 个随机 bbox 与 GDAL 完整 FID 对照。

### 10.2 性能门禁分层

| 级别 | 数据规模 | 运行时机 |
|---|---|---|
| 快速门禁 | 1K、10K、100K | 每个相关 PR |
| 标准门禁 | 1M | 本地验收和夜间任务 |
| 最终门禁 | 10M | 阶段收口和最终验收 |

每个性能提交都必须：

1. 先运行对应微基准证明目标阶段下降；
2. 再运行 1M/10M 覆盖率矩阵检查整体影响；
3. 比较完整 FID 集合；
4. 单独报告已领先场景是否回归；
5. 未达到门槛时保留真实数据，不宣称“全面超过 GDAL”。

---

## 11. 提交顺序

除基准提交外，后续提交仅在对应阶段触发条件成立时实施：

```text
bench: establish fair spatial performance gates
perf: compile geometry row access layout
perf: adapt mmap advice to spatial access pattern
perf: reduce repeated spx page traversal
perf: batch medium-density candidate access
perf: stream simple geometry bbox predicates
perf: calibrate deterministic spatial planner
perf: parallelize dense geometry scans        # 条件提交
perf: cache gdbtablx metadata across open    # Phase G
docs: record spatial optimization evidence
```

每个提交必须独立通过相关正确性测试和性能对照。出现 FID 不一致、10M 已领先路径回归或 fallback 退化时，立即停止叠加并定位根因。

---

## 12. 当前已知基准问题

正式实施 Phase A 前，以下问题不得被误写成产品性能结论：

- 当前 `build` 未设置 Release 优化；
- benchmark 强制开启逐候选细粒度计时；
- fast-gdb 与 GDAL 的打开和结果物化口径不对称；
- 固定查询顺序让首项承担主要冷页成本；
- steady-state 矩阵已覆盖 Point、MultiPoint、Polyline、Polygon；fresh-open 完整矩阵当前仅覆盖 Polygon；
- `docs/evidence/spatial-query-optimization-2026-07-13.md` 和本文保留为历史实施记录，Phase G 当前结论以最新验收记录为准。

## 12. Phase G 历史触发依据

**触发依据**：优化前对称计时的 10M fresh-open 1% 为 fast-gdb 1444.4ms、GDAL 264.7ms，超过小范围 +200ms 门槛；稳态相同查询为 58.5ms、132.8ms，说明瓶颈在打开期而不是空间谓词。

Phase G 已完成，正式实施内容、缓存边界、验证口径和数据复用规则见 Section 9 及当前证据文档。这里仅保留触发时的历史依据，避免将历史计划重复解释为待实施任务。
