# fast-gdb 空间查询本地验证指南

适用分支：`main`
适用提交：`2be0938`（PR #9 已合入）

## 1. 本轮重点验证

本轮新增查询前规划器。查询窗口与图层 extent 的重叠面积达到阈值时，直接进入顺序扫描，不解析 `.spx`，也不物化数百万候选 FID。

默认阈值：

```text
FAST_GDB_SPATIAL_DIRECT_SCAN_COVERAGE=0.29
```

新增指标：

- `estimated_coverage`：查询窗口与图层 extent 的面积覆盖率估算；
- `spx_bypassed`：是否在 `.spx` 查询前直接选择顺序扫描；
- `bbox_contained`：通过要素 bbox 完全位于查询框内而直接接受的数量；
- `exact_tested`：仍进入完整 GeometryModel 精确判断的数量。

高覆盖率场景预期执行路径：

```text
bbox:model:sequential-planned
```

## 2. Release 构建

```bash
cmake -S . -B build-spatial -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_CURVE_BACKEND=BUILTIN \
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DFAST_GDB_BUILD_FULL_TESTS=ON \
  -DBUILD_TESTING=ON

cmake --build build-spatial --target gdb_tutorial_test_runner --parallel
```

## 3. 正确性测试

```bash
ctest --test-dir build-spatial --output-on-failure \
  -R '^full\.SpatialQueryAdaptiveTest\.'
```

重点检查：

1. `HighDensityQueryBypassesSpatialIndexAndPreservesFids` 通过；
2. 高密度路径为 `bbox:model:sequential-planned`；
3. `spx_bypassed=true`；
4. 全范围 Point 测试中 `bbox_contained == feature_count`；
5. `exact_tested == 0`；
6. 低密度测试仍为 `bbox:model:spx-candidates`。

## 4. 1M 覆盖率矩阵

```bash
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 \
FAST_GDB_SPATIAL_PROFILE=1 \
ctest --test-dir build-spatial --output-on-failure \
  -R '^full\.SpatialDensityBenchmark\.DensityMatrix1M$'
```

## 5. 10M 覆盖率矩阵

```bash
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 \
FAST_GDB_RUN_10M_BENCHMARKS=1 \
FAST_GDB_SPATIAL_PROFILE=1 \
ctest --test-dir build-spatial --output-on-failure \
  -R '^full\.SpatialDensityBenchmark\.DensityMatrix10M$'
```

## 6. 阈值对照测试

分别运行以下阈值：

```bash
FAST_GDB_SPATIAL_DIRECT_SCAN_COVERAGE=0.20
FAST_GDB_SPATIAL_DIRECT_SCAN_COVERAGE=0.35
FAST_GDB_SPATIAL_DIRECT_SCAN_COVERAGE=0.50
FAST_GDB_SPATIAL_DIRECT_SCAN_COVERAGE=0.70
```

建议每个阈值运行 5 次，丢弃首次冷启动，记录剩余 4 次的中位数。

重点比较：

| 覆盖率 | 主要观察项 |
|---|---|
| 1% | 必须保留 `.spx` 路径，不得回归 |
| 10% | `.spx` 通常应继续占优 |
| 30% | 确定 `.spx` 与顺序扫描交叉点 |
| 80% | 必须绕过 `.spx`，观察候选查询时间是否接近 0 |
| 100% | 必须绕过 `.spx`，并尽量让大多数要素通过 `bbox_contained` 直接接受 |

## 7. 建议记录格式

请保存以下字段：

```text
机器型号 / CPU / 核心数 / 内存 / 操作系统
编译器与版本
GDAL版本
数据集大小与要素数
几何类型
查询覆盖率
FAST_GDB_SPATIAL_DIRECT_SCAN_COVERAGE
execution_path
estimated_coverage
spx_bypassed
candidate_count
bbox_rejected
bbox_contained
exact_tested
fast_ms
gdal_ms
fast_ms / gdal_ms
```

## 8. 验收判断

本轮主要判断两件事：

1. 高覆盖率查询是否彻底消除了 `.spx` 候选物化成本；
2. `bbox_contained` 是否显著降低了 `exact_tested`。

Geometry-only Scanner 已随 `2be0938` 合入；条件并行扫描仍未实施。因此后续如果新增规模仍未超过 GDAL，需要根据阶段耗时判断下一瓶颈：

- 若 `candidate_lookup_ms` 已接近 0，而总耗时仍高，下一步优先检查 Geometry-only Scanner 的记录布局和 I/O 批量读取；
- 若 `exact_tested` 仍高，下一步优先实现流式边界谓词；
- 若单线程扫描已接近 GDAL但仍稍慢，下一步实施固定分区并行扫描。
