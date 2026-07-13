# 15 — 基于验收方案的后续空间查询优化设计

**更新日期**：2026-07-13  
**适用分支**：`agent/spatial-query-optimization`  
**依据文档**：`docs/evidence/spatial-query-acceptance-test-plan.md`  
**目标**：以验收方案中的正确性、执行路径、阶段耗时、内存和稳定性指标为唯一优化依据，逐步把 10M 中高覆盖率 bbox 查询推进到超越 GDAL。

---

## 1. 设计原则

后续优化不再采用“先实现大量功能，再整体测试”的方式，而采用闭环：

```text
验收指标
  -> 判断主瓶颈
  -> 只实施一个主要优化
  -> 运行正确性与性能矩阵
  -> 记录结果
  -> 决定下一阶段
```

每一阶段必须满足：

1. 与 GDAL 完整 0-based FID 集合一致；
2. 1% 和 10% 查询不明显回归；
3. 异常、曲线、MultiPatch、Z/M/ZM fallback 不退化；
4. 阶段指标能够证明该优化实际命中了预期瓶颈；
5. 未产生真实基准前，不宣称性能目标达成。

---

## 2. 验收指标到优化动作的映射

| 验收现象 | 根因判断 | 后续动作 |
|---|---|---|
| `candidate_lookup_ms` 高 | 仍在大规模物化 `.spx` 候选 | 修正查询前规划器 |
| `candidate_lookup_ms` 接近 0，但总耗时高 | 通用行扫描成本高 | 实现 Geometry-only Scanner |
| `exact_tested / feature_count` 高 | 边界候选仍大量构造 `GeometryModel` | 实现 Streaming Predicate |
| `bbox_contained` 高但总耗时仍高 | 行解析和调度占主导 | 优化 scanner、批量迭代、并行扫描 |
| 30% 慢、80%/100% 快 | 中密度切换点不合理 | 校准 planner 或增加中密度分块策略 |
| 1%/10% 回归 | planner 固定开销或 `.spx` 路径受影响 | 缩短 fast path、延迟初始化、恢复低密度专用路径 |
| 峰值 RSS 高 | 候选或结果大向量占用过高 | visitor/count/bitmap API、分块输出 |
| 单线程接近 GDAL但略慢 | CPU 扫描吞吐成为瓶颈 | 固定分区并行扫描 |
| FID 不一致 | 快速判定语义错误 | 立即回滚该 fast path，优先修正正确性 |

---

## 3. 总体架构

后续空间查询路径拆为四层：

```text
SpatialQueryPlanner
  -> CandidateSource
      -> SPX candidate stream
      -> Geometry sequential scan
      -> Block-parallel geometry scan
  -> GeometryFastPredicate
      -> bbox reject
      -> bbox contained accept
      -> streaming exact predicate
      -> GeometryModel fallback
  -> ResultSink
      -> vector FIDs
      -> visitor
      -> count
      -> bitmap
```

关键要求：

- Planner 在 `.spx` 候选物化前作出决定；
- 高密度路径不得解析无关属性列；
- 普通 Point/Polyline/Polygon 不应默认构造 `GeometryModel`；
- 并行只用于只读扫描，不能改变 FID 顺序和结果语义；
- 输出模式与执行引擎解耦，避免 count-only 场景仍创建巨大 FID vector。

---

## 4. Phase A — 验收基线与诊断输出

### 4.1 目标

确保验收文档要求的所有指标都能够稳定输出，以便后续优化有数据依据。

### 4.2 需要补齐的指标

```text
planner_ms
row_scan_ms
geometry_locate_ms
bbox_peek_ms
streaming_predicate_ms
model_fallback_ms
merge_ms
thread_count
result_output_ms
peak_candidate_count
```

保留现有：

```text
estimated_coverage
spx_bypassed
candidate_lookup_ms
bbox_rejected
bbox_contained
exact_tested
invalid_geometries
```

### 4.3 实施要求

- 仅在 `FAST_GDB_SPATIAL_PROFILE=1` 下启用细粒度计时；
- 生产路径不做逐要素高精度时钟调用；
- 使用阶段起止时间，而不是每条记录计时；
- benchmark 输出必须包含 fast/GDAL 比率和完整 FID 一致性结果。

### 4.4 完成标准

验收文档第 6、7、8、11、12 节需要的指标均可直接从测试输出获得。

---

## 5. Phase B — Geometry-only Scanner

### 5.1 触发条件

满足以下任一条件即优先实施：

- 80%/100% 已成功 `spx_bypassed=true`；
- `candidate_lookup_ms` 接近 0；
- 总耗时仍明显高于 GDAL；
- profile 显示 `sequential_scan()` 或字段解析占主导。

### 5.2 当前问题

通用 `sequential_scan()` 会：

- 解释整行 schema；
- 构造或填充全部 `FieldRef`；
- 跳过或解析无关属性字段；
- 通过通用回调层传递 geometry；
- 在高覆盖率下重复数百万次。

### 5.3 设计

新增编译后的行访问计划：

```cpp
struct GeometryScanPlan {
    size_t geometry_field_index;
    size_t nullable_bitmap_size;
    std::vector<FieldSkipOp> prefix_ops;
    bool geometry_nullable;
};
```

`FieldSkipOp` 只描述如何跳过 geometry 之前的字段：

```cpp
enum class FieldSkipKind {
    FixedWidth,
    VaruintLengthPrefixed,
    ObjectIdNoStorage,
    NullableFixedWidth,
    NullableVaruintLengthPrefixed
};
```

新增接口：

```cpp
uint64_t scan_geometry_blobs(
    const GeometryScanPlan& plan,
    GeometryBlobCallback callback) const;
```

回调参数：

```cpp
bool(uint32_t fid, const uint8_t* blob, size_t blob_size)
```

### 5.4 快速路径要求

- 直接读取 record length；
- 读取 nullable bitmap；
- 按预编译 skip plan 前进指针；
- 读取 geometry 长度和 blob view；
- 不创建 `FieldRef[]`；
- 不读取 geometry 后面的字段；
- mmap 模式零拷贝；
- 非 mmap 模式保持正确性 fallback。

### 5.5 测试

新增：

```text
GeometryOnlyScannerMatchesSequentialScan
GeometryOnlyScannerHandlesNullableGeometry
GeometryOnlyScannerHandlesVariablePrefixFields
GeometryOnlyScannerPreservesDeletedOrMissingRows
GeometryOnlyScannerFallsBackWithoutMmap
```

### 5.6 验收指标

- `row_scan_ms` 显著下降；
- FID 集合与通用扫描一致；
- 80%/100% 总耗时下降；
- 1%/10% 不受影响。

---

## 6. Phase C — Streaming Predicate

### 6.1 触发条件

- `exact_tested` 占 feature_count 比例高；
- `model_fallback_ms` 或 `exact_filter_ms` 成为主要耗时；
- Geometry-only Scanner 后仍未达到目标。

### 6.2 API 设计

```cpp
enum class FastPredicateResult {
    Disjoint,
    Intersects,
    Fallback,
    Invalid
};

FastPredicateResult intersects_bbox_streaming(
    const uint8_t* blob,
    size_t size,
    const QueryBbox& query,
    StreamingPredicateStats* stats = nullptr) const;
```

### 6.3 Point

- 读取 geom type；
- 解码 X/Y；
- 直接判断是否位于 query bbox；
- 不读取 Z/M；
- 不构造模型。

### 6.4 MultiPoint

- 读取 point count 和 bbox；
- bbox 三态判断；
- 边界候选流式读取 XY；
- 任一点命中立即返回；
- 正确跳过 Z/M 数据段。

### 6.5 Polyline

边界候选采用：

1. 任一顶点在 query bbox 内；
2. 任一线段与矩形四边相交；
3. 命中即提前返回。

只保留前一点和当前点，不创建 point vector。

### 6.6 Polygon

边界候选采用：

1. 任一顶点在 query bbox 内；
2. 任一 ring 边与矩形相交；
3. query bbox 代表点或角点位于 polygon 内。

Point-in-polygon 必须：

- 支持多 ring；
- 使用 parity 或现有模型一致的 ring 语义；
- 正确处理 holes 和 islands；
- 对曲线 ring 返回 `Fallback`；
- 不因 ring 方向不同改变结果。

### 6.7 fallback

以下情况保留统一 `GeometryModel`：

```text
General curve geometry
MultiPatch
未知类型
异常 varint
超限 point/part count
数值溢出
streaming 语义无法确认
```

### 6.8 指标

新增：

```text
streaming_tested
streaming_accepted
streaming_rejected
model_fallback_tested
streaming_invalid
```

### 6.9 完成标准

- 普通 Point/Polyline/Polygon 大多数边界候选不进入模型；
- 与 GDAL 完整 FID 一致；
- holes、multipart、反向 ring、Z/M/ZM 测试通过；
- `model_fallback_tested / exact_tested` 显著下降。

---

## 7. Phase D — 中密度执行策略

### 7.1 触发条件

- 80%/100% 达标；
- 30% 未达到 `fast-gdb <= GDAL × 0.90`；
- 阈值测试显示 `.spx` 与全扫描交叉点不稳定。

### 7.2 设计选项

新增三种计划：

```cpp
enum class SpatialPlan {
    SpxCandidates,
    BlockCandidates,
    SequentialScan,
    ParallelScan
};
```

中密度 `BlockCandidates`：

- `.spx` 返回候选；
- 候选按 tablx block 或 FID 连续区间分组；
- 连续区间内按顺序读取；
- 避免逐 FID 随机定位；
- 不构建全表扫描成本。

### 7.3 规划成本模型

```text
spx_cost = index_fixed + candidate_count * random_blob_cost
scan_cost = feature_count * sequential_blob_cost
block_cost = index_fixed + block_count * block_open_cost + candidate_count * sequential_in_block_cost
```

初期使用实测常数，后续可按 QueryEngine 热查询历史更新指数移动平均值。

### 7.4 决策输入

```text
feature_count
estimated_coverage
geometry_type
last_spx_candidate_ratio
average_blob_size
mmap_available
hot/cold state
thread_count
```

### 7.5 安全要求

- 估算失败时选择保守路径；
- 规划错误只能影响性能，不能影响结果；
- 可通过环境变量强制指定路径用于对照测试。

建议变量：

```text
FAST_GDB_SPATIAL_FORCE_PLAN=spx|block|scan|parallel
```

---

## 8. Phase E — 并行扫描

### 8.1 触发条件

- 单线程顺序扫描已接近 GDAL；
- `row_scan_ms + predicate_ms` 为主要耗时；
- I/O 位于 SSD/NVMe 或数据已热缓存；
- 正确性与单线程性能均稳定。

### 8.2 分区方式

按 FID 连续区间分区：

```text
[0, N/T)
[N/T, 2N/T)
...
```

每个线程：

- 使用共享只读 mmap；
- 拥有独立 decoder；
- 拥有独立 result buffer；
- 拥有独立 metrics；
- 不写共享 vector；
- 不使用逐要素锁。

### 8.3 合并

由于各分区 FID 不重叠且递增：

- 按分区编号直接拼接；
- 无需最终 sort；
- 只校验边界单调性；
- `merge_ms` 单独记录。

### 8.4 线程数

默认：

```text
min(物理核心数, 8)
```

支持：

```text
FAST_GDB_SPATIAL_THREADS=1..N
```

### 8.5 终止与异常

- 任一线程检测到致命扫描错误时设置原子终止标志；
- 非法单条 geometry 计入 invalid，不终止全查询；
- fallback 仅在线程本地执行；
- 线程创建失败回退单线程。

### 8.6 验收

测试 1、2、4、8 线程：

- FID 完全一致；
- 输出顺序一致；
- 峰值 RSS 可接受；
- 4～8 线程在 10M 80%/100% 上产生稳定收益；
- 不在 HDD 冷缓存场景强制并行。

---

## 9. Phase F — ResultSink 与内存优化

### 9.1 触发条件

- 峰值 RSS 过高；
- 全范围结果 vector 占用明显；
- count-only 或 visitor 场景仍分配完整结果。

### 9.2 接口

```cpp
std::vector<uint32_t> query_bbox_fids(...);
uint64_t query_bbox_count(...);
bool query_bbox_visit(..., FIDVisitor visitor);
SpatialBitmap query_bbox_bitmap(...);
```

内部统一：

```cpp
class SpatialResultSink {
public:
    virtual bool emit(uint32_t fid) = 0;
};
```

### 9.3 要求

- benchmark 与 GDAL 比较完整 FID 时必须使用 FID sink；
- count-only 只能用于 count 对 count 的公平比较；
- visitor 可提前终止；
- bitmap 仅在调用方明确需要集合运算时使用；
- vector sink 预估 reserve，但不能因错误估算分配 feature_count 级候选。

---

## 10. Phase G — 稳定性、异常与跨平台

必须覆盖验收方案中的异常路径：

```text
.spx 缺失
.spx 损坏
mmap 不可用
extent 无效
空表
无 geometry 字段
非法 bbox
非法 Geometry Blob
曲线
MultiPatch
Z/M/ZM
删除记录
Windows 多配置 Release
Linux/macOS Release
```

新增规则：

- fast path 发生任何不确定状态时返回 `Fallback`，不能猜测结果；
- fallback reason 必须可观察；
- 所有长度、指针推进和 varint 读取必须使用 checked cursor；
- point/part count 必须有上限和剩余字节验证；
- 并行路径必须通过 TSAN 或等效的数据竞争审查（条件允许时）。

---

## 11. 代码组织建议

建议拆分：

```text
reader/
  spatial_query_planner.h/.cpp
  geometry_scan_plan.h/.cpp
  geometry_column_scanner.h/.cpp
  geometry_stream_predicate.h/.cpp
  spatial_result_sink.h/.cpp
  query_engine_geometry.cpp
```

`query_engine_geometry.cpp` 只负责 orchestration，不继续承载全部二进制解析与规划逻辑。

依赖方向：

```text
QueryEngine
  -> SpatialQueryPlanner
  -> GeometryColumnScanner
  -> GeometryStreamPredicate
  -> SpatialResultSink
```

避免反向依赖 `QueryEngine`。

---

## 12. 提交拆分

建议每一步独立提交：

1. `perf: add spatial phase metrics`
2. `perf: add geometry-only row scanner`
3. `test: validate geometry-only scanner equivalence`
4. `perf: stream point and multipoint bbox predicates`
5. `perf: stream polyline bbox predicates`
6. `perf: stream polygon bbox predicates`
7. `test: cover topology and dimensional fallbacks`
8. `perf: add block candidate execution plan`
9. `perf: add parallel geometry scan`
10. `perf: add spatial result sinks`
11. `docs: record spatial acceptance results`

每个提交必须能够独立编译、测试和回退。

---

## 13. 阶段门禁

### 门禁 1 — Geometry-only Scanner

- scanner 与通用扫描 FID 一致；
- 80%/100% `row_scan_ms` 下降；
- 无小范围回归。

### 门禁 2 — Streaming Predicate

- 普通几何 FID 与 GDAL 一致；
- topology/fallback 测试通过；
- `model_fallback_tested` 显著下降。

### 门禁 3 — Planner

- 1%/10% 保持 `.spx`；
- 80%/100% 规划扫描；
- 30% 选择实测更快路径。

### 门禁 4 — Parallel Scan

- 1/2/4/8 线程结果完全一致；
- 4～8 线程产生稳定加速；
- RSS 和稳定性通过。

### 门禁 5 — 最终验收

按 `spatial-query-acceptance-test-plan.md` 全部执行：

```text
30%  <= GDAL × 0.90
80%  <= GDAL × 0.80
100% <= GDAL × 0.80
```

未达到门槛时，根据阶段指标继续优化，不通过修改验收标准规避问题。

---

## 14. 下一步实施顺序

当前已有：

- 查询前覆盖率规划；
- 高覆盖率 `.spx` bypass；
- bbox reject；
- bbox contained accept；
- 基础性能矩阵。

因此推荐下一步顺序：

1. **补齐阶段指标**；
2. **实现 Geometry-only Scanner**；
3. 本地执行 1M/10M 验收矩阵；
4. 根据 `exact_tested` 决定是否立即实现 Streaming Predicate；
5. 根据 30% 结果决定是否实现 BlockCandidates；
6. 单线程接近 GDAL后实施并行扫描；
7. 最后处理 ResultSink 和峰值内存。

这一路线优先解决当前最可能的主瓶颈：高覆盖率下通用 `sequential_scan()` 对无关字段的逐行解析成本。
