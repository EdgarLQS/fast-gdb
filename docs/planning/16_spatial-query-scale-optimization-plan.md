# 16 — fast-gdb 超大规模空间查询优化计划

**更新日期**：2026-07-14
**目标分支**：`agent/spatial-query-scale-optimization`
**状态**：当前计划，尚未实施
**适用范围**：原生 `explorgdb` Reader 的 bbox 空间查询

## 1. 当前基线与目标

Phase A–G 已完成 1K–10M 的 Point、MultiPoint、Polyline、Polygon steady-state 验收，以及 10M Polygon fresh-open 验收。最新证据见 [空间查询公平基准验收记录](../evidence/spatial-query-baseline-2026-07-14.md)。这些结果作为冻结回归基线，不代表 35GB/5 亿级真实生产数据已经验收。

Phase H 不再围绕已达标的 10M 合成路径盲目优化，目标是：

1. 补齐 10M fresh-open 的几何类型覆盖；
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

## 5. Phase H2 — 按需 `.gdbtablx`

触发条件：50M fresh-open 的打开时间、完整偏移解析或峰值 RSS 导致性能门槛失败，或偏移表超过缓存上限后出现明显退化。

实施边界：

1. 将 `.gdbtablx` 从打开时完整解析改为按需加载；
2. 高覆盖率顺序扫描延迟或跳过完整偏移表解析；
3. `.spx` 低密度候选仅按页解码所需偏移；
4. 使用有字节上限、按文件身份失效的不可变页缓存；
5. 缓存命中共享不可变存储，不再深拷贝完整偏移向量；
6. 保留 `FAST_GDB_TABLX_CACHE=0` 冷打开基线和原解析路径回退。

该阶段只调整内部存储接口，不改变现有公开查询 API。

## 6. Phase H3 — 数据分布感知规划

触发条件：聚簇、稀疏、删除洞、宽记录或复杂几何场景中，固定 29% 面积阈值选择错误路径，并造成超过 10% 的性能回归或门槛失败。

规划器使用候选密度、有效 FID 密度、平均 Geometry Blob 大小和几何类型估算 `.spx` 与顺序扫描成本。指标必须是查询级聚合值，禁止在 profile OFF 的逐要素热路径加入高精度计时。现有环境变量继续用于强制路径、阈值对照和回归定位。

若固定阈值在新增矩阵中仍稳定通过，不实施成本规划器。

## 7. Phase H4 — 条件优化

- 只有边界要素完整模型构建占总耗时超过 30% 时，实施只读取 XY 的流式精确谓词；
- 只有单线程 50M 的 30%/80%/100% 仍未达到 `0.90× GDAL` 时，实施只读 mmap 分区并行扫描；
- 小范围查询差距不超过 200ms 时，不引入 SIMD、并行或更复杂缓存；
- 所有性能提交必须独立通过 10M 冻结矩阵，不能叠加未证明收益的优化。

## 8. 提交与验证顺序

```text
bench: complete fresh-open geometry coverage
bench: add reusable 50m scale fixture
perf: load gdbtablx offsets on demand       # 条件提交
perf: plan spatial paths by measured cost   # 条件提交
perf: stream boundary predicates            # 条件提交
perf: parallelize dense scans               # 条件提交
docs: record Phase H evidence
```

每阶段完成后更新当前证据、项目状态和性能技术文档。未触发的阶段记录测试结果后跳过，不为了完成计划而修改代码。

## 9. 范围边界

- 本计划不把受控合成结果扩大为 35GB/5 亿级生产承诺；
- `gdb_component` 是单独的 GDAL 依赖路径，其历史数字不得与原生 Reader 公平基准混用；
- 本计划不处理 Writer、MultiPatch、SQL、Raster 或原生 curve WKB；
- Phase A–G 的过程和历史门槛见 [归档计划 14](archive/14_fast-gdb空间查询优化计划.md) 与 [归档计划 15](archive/15_spatial-query-followup-optimization-design.md)。
