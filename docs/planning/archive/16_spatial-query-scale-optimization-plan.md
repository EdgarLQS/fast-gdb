# 16 — fast-gdb 超大规模空间查询优化计划

**更新日期**：2026-07-14
**实现基线**：`main` / `2be0938`
**状态**：已归档；Phase A–H 实现已合入，未完成的规模化验证现由计划 18 管理
**适用范围**：原生 `explorgdb` Reader 的 bbox 空间查询

> 归档说明（2026-07-15）：本文保留 Phase H 的原始目标、实现过程和性能门槛，不再作为当前执行入口。
> Point/MultiPoint/Polyline 10M fresh-open、50M 阶梯和生产数据验证已转入
> [项目状态与规划](../01_项目状态与规划.md)。

## 1. 当前基线与目标

Phase A–H 的空间查询实现已合入 `main`。历史性能证据已移除，本文不再作为当前性能基线。

合入后的后续工作不再围绕已达标的 10M 合成路径盲目优化，目标是：

1. 补齐 Point、MultiPoint、Polyline 的 10M fresh-open 几何类型覆盖；
2. 验证超过当前 `.gdbtablx` 缓存容量后的扩展行为；
3. 识别非均匀数据分布下的执行计划误判；
4. 仅在真实触发条件成立时实施按需偏移表、成本规划或并行扫描。

## 2. 验收门槛

| 场景 | 性能门槛 |
|---|---|
| 1% / 10% | `fast-gdb <= GDAL + 200ms`，或 `fast-gdb <= GDAL × 0.90` |
| 30% / 80% / 100% | `fast-gdb <= GDAL × 0.90` |
| 10M 已通过路径 | 相对冻结基线回归不超过 5% |
| 正确性 | 每轮完整 0-based FID 集合与 GDAL 一致 |
| 资源 | 无 OOM、无无界缓存增长，并记录峰值 RSS |

墙钟门禁使用 Release、profile OFF、预热一次、双方执行顺序交替和中位数 of 5。最终关键场景运行 20 次，用于有效 P95；不能继续把 5 次测量中的最大值称为稳定 P95。

## 3. Phase H0 — 补齐证据

1. 复用 `test_data/spatial_matrix/`，完成 Point、MultiPoint、Polyline 的 10M fresh-open 全矩阵；不重新生成已有数据。
2. 用当前公平基准复测 `test_data/large_10m/large_10m_test.gdb`，区分当前 `explorgdb` 路径与历史 `gdb_component` 结果。
3. 每轮记录执行路径、候选数、bbox 拒绝/包含数、精确判断数、缓存状态、RSS 和完整 FID 对照。

若所有新增场景均通过门槛，只补充证据，不修改查询实现。

## 4. Phase H1 — 可复用的规模阶梯

当前没有 35GB/5 亿级真实数据时，只新增一次 50M Point 数据，用于跨越 TablxCache 的 256 MiB 容量边界。50M 数据必须放在 `test_data/spatial_matrix/` 的固定目录，并满足：

- 生成前校验图层名、扁平几何类型、要素数和 `.spx`；
- 校验通过时打印“跳过生成”并复用目录；
- 只有数据损坏或生成定义变化时才能重建；
- 生成过程串行执行，禁止并发写同一输出目录。

复杂几何成本继续使用现有 10M Polygon、Polyline 和 MultiPoint 数据，不重复创建同类大夹具。获得真实生产数据后，最终结论必须由生产数据重新验收。

## 5. Phase H-W — Windows mmap 与批量 I/O

### 5.1 当前根因

当前 Windows 路径不是 Windows 平台本身较慢，而是 `src/edgar/explorgdb/reader/sys/mman.h` 中的 Windows stub 始终返回 `MAP_FAILED`，导致 Reader 退回逐条 positional I/O。`scan_geometry_blobs()` 在该路径上会对每个 FID 执行：

```text
read_at(offset, 4)                 -> 读取记录长度
read_at(offset + 4, row_buffer)    -> 读取一条记录
解析记录
```

千万级数据下，大量小块 `ReadFile/pread` 是 Windows 性能差距的主要来源。现有 Geometry-only Scanner 已支持 `mapped_data_ != nullptr` 的快速路径，因此 P0 优先恢复真正的 Windows 映射。

### 5.2 实施优先级

#### P0：Windows `MapViewOfFile` mmap 兼容层

修改 `src/edgar/explorgdb/reader/sys/mman.h`，用以下 API 替换当前 stub：

- `CreateFileW`；
- `CreateFileMappingW`；
- `MapViewOfFile`；
- `UnmapViewOfFile`。

实现约束：

1. 映射偏移按 Windows allocation granularity 对齐，返回给调用方的指针加上窗口内偏移；
2. 大文件使用滑动窗口映射，不假设单次映射小于 4GB；
3. `GdbTableParser` 保存 mapping handle、file handle、view base、view length 和逻辑数据指针；
4. 关闭顺序固定为 `UnmapViewOfFile`、关闭 mapping handle、关闭 file handle；
5. 映射成功后必须复用现有 `mapped_data_` 路径，不能再进入逐记录 `read_at()`；
6. 映射失败时保留现有 fd fallback，并记录明确原因。

#### P1：fd fallback 批量顺序读取

当 mmap 不可用时，高覆盖率查询按物理记录区间批量读取：

```text
FID 区间 -> 一次读取 1–8 MiB -> 内存中解析多个记录 -> Geometry-only Scanner
```

Windows 文件句柄增加 `FILE_FLAG_SEQUENTIAL_SCAN`。高覆盖率场景优先按 `.gdbtable` 物理顺序扫描，不为每个 FID 单独执行长度读取和记录读取。

#### P2：低密度候选区间合并

`.spx` 低密度路径仍需随机 FID 定位时：

1. 收集候选 FID；
2. 通过 `.gdbtablx` 获取偏移；
3. 按物理偏移排序；
4. 合并相邻记录为读取区间；
5. 批量读取后恢复 FID 顺序；
6. 保持完整 FID 集合和原有空间语义。

#### P3：OVERLAPPED 异步读取

只有 P1/P2 在 Windows Release 下仍未达到门槛时，才引入 `ReadFile` + `OVERLAPPED` 批量异步 I/O。异步路径必须限制并发请求数，避免内存和磁盘队列无界增长；失败时回退同步批量读取。

### 5.3 Windows 验收矩阵

Windows Release 必须对 1M、10M 数据分别运行 1%、10%、30%、80%、100% 覆盖率，并分别记录：

| 维度 | 必测模式 |
|---|---|
| 访问路径 | Windows mmap、mmap 失败 fallback |
| 缓存状态 | cold cache、warm cache |
| 数据规模 | 1M、10M |
| 查询覆盖率 | 1%、10%、30%、80%、100% |
| 结果检查 | 完整 FID 集合、`invalid_geometries == 0` |
| 性能指标 | `geometry_scan_ms`、`total_ms`、峰值 RSS、系统调用/批量读取统计 |

验收规则：

- 1%/10%：不慢于 GDAL +200ms，或达到 `0.90× GDAL`；
- 30%/80%/100%：达到 `0.90× GDAL`；
- fallback 路径不得比当前 Windows 版本更慢；
- 已通过的 macOS/Linux 10M 基线不得回归超过 5%；
- mmap 成功时执行路径必须确认使用 `mapped_data_`；
- mmap 失败时不得崩溃、丢 FID 或产生异常几何；
- cold/warm 两种状态均必须报告，不能只报告最快一次。

P0 完成后若 Windows 已达到门槛，停止 P1–P3；只有实际剩余差距触发时才继续批量或异步读取。

## 6. Phase H2 — 按需 `.gdbtablx`

触发条件：50M fresh-open 的打开时间、完整偏移解析或峰值 RSS 导致性能门槛失败，或偏移表超过缓存上限后出现明显退化。

实施边界：

1. 将 `.gdbtablx` 从打开时完整解析改为按需加载；
2. 高覆盖率顺序扫描延迟或跳过完整偏移表解析；
3. `.spx` 低密度候选仅按页解码所需偏移；
4. 使用有字节上限、按文件身份失效的不可变页缓存；
5. 缓存命中共享不可变存储，不再深拷贝完整偏移向量；
6. 保留 `FAST_GDB_TABLX_CACHE=0` 冷打开基线和原解析路径回退。

该阶段只调整内部存储接口，不改变现有公开查询 API。

## 7. Phase H3 — 数据分布感知规划

触发条件：聚簇、稀疏、删除洞、宽记录或复杂几何场景中，固定 29% 面积阈值选择错误路径，并造成超过 10% 的性能回归或门槛失败。

规划器使用候选密度、有效 FID 密度、平均 Geometry Blob 大小和几何类型估算 `.spx` 与顺序扫描成本。指标必须是查询级聚合值，禁止在 profile OFF 的逐要素热路径加入高精度计时。现有环境变量继续用于强制路径、阈值对照和回归定位。

若固定阈值在新增矩阵中仍稳定通过，不实施成本规划器。

## 8. Phase H4 — 条件优化

- 只有边界要素完整模型构建占总耗时超过 30% 时，实施只读取 XY 的流式精确谓词；
- 只有单线程 50M 的 30%/80%/100% 仍未达到 `0.90× GDAL` 时，实施只读 mmap 分区并行扫描；
- 小范围查询差距不超过 200ms 时，不引入 SIMD、并行或更复杂缓存；
- 所有性能提交必须独立通过 10M 冻结矩阵，不能叠加未证明收益的优化。

## 9. 提交与验证顺序

```text
bench: complete fresh-open geometry coverage
bench: add reusable 50m scale fixture
perf: enable Windows MapViewOfFile mmap
perf: batch Windows sequential fallback I/O   # 条件提交
perf: merge Windows sparse candidate ranges    # 条件提交
perf: add Windows OVERLAPPED reads             # 条件提交
perf: load gdbtablx offsets on demand       # 条件提交
perf: plan spatial paths by measured cost   # 条件提交
perf: stream boundary predicates            # 条件提交
perf: parallelize dense scans               # 条件提交
docs: record Phase H evidence
```

每阶段完成后更新当前证据、项目状态和性能技术文档。未触发的阶段记录测试结果后跳过，不为了完成计划而修改代码。

## 10. 范围边界

- 本计划不把受控合成结果扩大为 35GB/5 亿级生产承诺；
- `gdb_component` 是单独的 GDAL 依赖路径，其历史数字不得与原生 Reader 公平基准混用；
- 本计划不处理 Writer、MultiPatch、SQL、Raster 或原生 curve WKB；
- Phase A–G 的过程和历史门槛见 [归档计划 14](14_fast-gdb空间查询优化计划.md) 与 [归档计划 15](15_spatial-query-followup-optimization-design.md)。
