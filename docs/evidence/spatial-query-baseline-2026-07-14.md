# fast-gdb 空间查询公平基准验收记录

**测试日期**: 2026-07-14
**分支**: `agent/spatial-query-optimization`
**提交**: (当前分支 `agent/spatial-query-optimization`，HEAD `15f37e3`，变更未提交)
**测试机器**: Apple M5, 10 核, 16 GB RAM, SSD
**操作系统**: macOS 26.4 (Darwin 25.4.0)
**编译器**: Apple clang 21.0.0 (clang-2100.0.123.102)
**CMake**: 4.3.3
**构建类型**: Release (`-O3 -DNDEBUG`)
**GDAL**: 3.13.0 (Homebrew)

## 构建

- [x] macOS Release 构建通过
- [x] FAST_GDB_SPATIAL_PROFILE **未强制开启**（墙钟测试关闭）
- [x] 独立 Release 构建目录 `build-release/`
- [x] 无编译警告（修复1个未使用变量警告）
- [x] 无链接错误

## 正确性

- [x] Adaptive 空间查询测试通过（5/5 测试通过）
- [x] SpatialIndex 测试通过（11/11 测试通过）
- [x] 全量 CTest 通过（354/369 通过，15 预期跳过）
- [x] 1M FID 一致性通过（全部覆盖率皆对比 GDAL 完整 FID 集合）
- [x] 10M FID 一致性通过（全部覆盖率皆对比 GDAL 完整 FID 集合）
- [x] 无 FID 不一致、无重复 FID、0 异常几何

## 结果

### 10M steady-state（预热后仅计时查询，不含打开与关闭。profile OFF，中位数 of 5）

| 覆盖率 | fast_med | gdal_med | ratio | 验收目标 | 结果 |
|------:|--------:|--------:|------:|---------|:---:|
| 1% | 54.0 ms | 125.3 ms | **0.431** | ≤ +200ms 或 ≤ 0.90× GDAL | ✅ |
| 10% | 185.1 ms | 607.8 ms | **0.305** | ≤ +200ms 或 ≤ 0.90× GDAL | ✅ |
| 30% | 320.2 ms | 1492.0 ms | **0.215** | ≤ 0.90× GDAL | ✅ |
| 80% | 314.3 ms | 3643.1 ms | **0.086** | ≤ 0.90× GDAL | ✅ |
| 100% | 237.5 ms | 2930.0 ms | **0.081** | ≤ 0.90× GDAL | ✅ |

### 10M fresh-open（含打开+关闭，中位数 of 5）

> **⚠️ 该组数据无效**：fast-gdb 的打开（`catalog.scan()`, `resolver.load()`, `engine->open()`）未被纳入计时，
> 而 GDAL 将 `GDALOpenEx` 和 `GDALClose` 包含在计时内，导致比较不对称。
> 见 `test_spatial_benchmark.cpp` 的 `query_fast_gdb_fresh_open()` 修正详情。
> 修复后待重跑。

| 覆盖率 | fast_med | gdal_med | ratio |
|------:|--------:|--------:|------:|
| 1% | 96.5 ms | 131.6 ms | 0.733 |
| 10% | 254.6 ms | 622.0 ms | 0.409 |
| 30% | 426.1 ms | 1472.9 ms | 0.289 |
| 80% | 398.3 ms | 3663.8 ms | 0.109 |
| 100% | 306.4 ms | 2931.2 ms | 0.105 |

### 1M steady-state（中位数 of 5）

| 覆盖率 | fast_med | gdal_med | ratio |
|------:|--------:|--------:|------:|
| 1% | 7.0 ms | 12.5 ms | 0.559 |
| 10% | 24.9 ms | 59.4 ms | 0.420 |
| 30% | 44.5 ms | 139.1 ms | 0.320 |
| 80% | 31.5 ms | 328.0 ms | 0.096 |
| 100% | 24.1 ms | 288.0 ms | 0.084 |

### 关键内部指标（profile ON 辅助，稳态）

| 覆盖率 | 路径 | spx_bypassed | spx_ms | blob_ms | bbox_ms | exact_ms | scan_ms |
|:------|:-----|:------------|------:|--------:|--------:|--------:|--------:|
| 1% | spx-candidates | false | 12.9 | 15.1 | 3.5 | 6.2 | - |
| 10% | spx-candidates | false | 60.4 | 50.1 | 29.5 | 19.2 | - |
| 30% | spx-candidates | false | 152.5 | 73.9 | 86.3 | 34.0 | - |
| 80% | sequential-planned | true | - | - | 273.6 | 54.2 | 547.8 |
| 100% | sequential-planned | true | - | - | 270.8 | 0.0 | 483.7 |

### 测试数据

| 数据集 | 文件大小 | 要素数 | 几何类型 | .spx |
|:------|-------:|------:|:--------|:----:|
| 1M | 191 MB | 1,000,000 | Polygon | 有 |
| 10M | 1.9 GB | 10,000,000 | Polygon | 有 |

> **测试范围限制**：当前仅测试了大型 Polygon（1M/10M）。计划要求 Point、MultiPoint、Polyline、Polygon 及 1K~10M 全规模矩阵，
> 因此当前验收结论仅适用于大型 Polygon 数据集。其他几何类型和小数据的性能验证待后续补全。

## 关键发现

1. **Release/profile-off 下所有覆盖率皆已大幅超越 GDAL**（ratio 0.08~0.43）。
2. **profile 开销显著**（30% +51%, 80% +73%, 100% +104%），证实之前 8.42× 比值为测量伪影。
3. **路径规划生效**：80%+ 成功绕过 .spx (`spx_bypassed=true`)，走 sequential-planned。
4. **30% 在当前阈值下处于交叉点附近**：sequential 和 spx 路径耗时相近（339 vs 358 ms）。
5. **P95（观察值）**：仅5个样本，P95等价于最大值，不足以支撑稳定性结论。所列P95仅作为同批次内最大值观察，不应用于外推。

## 优化阶段触发判断

| 阶段 | 触发条件 | 结果 |
|:----|:---------|:----:|
| Phase B (候选 Geometry 定位) | 10M 1%/10% 慢于 GDAL 超 200ms | **steady-state 不触发**；fresh-open 待重跑 |
| Phase C (spx 中密度) | Phase B 后 10%/30% 未达标 | **steady-state 不触发**；fresh-open 待重跑 |
| Phase D (Streaming Predicate) | Phase B/C 后 30% 未达标 | **steady-state 不触发**；fresh-open 待重跑 |
| Phase E (规划器校准) | 两个路径通过验证 | **可选** (当前不执行；补齐矩阵后再评估) |
| Phase F (条件并行) | 单线程 80%/100% 未达 0.90× GDAL | **steady-state 不触发**；fresh-open 待重跑 |

## 新增/修改文件

- `tests/edgar/explorgdb/reader/test_spatial_benchmark.cpp` — 公平基准重构：
  - **去除 profile 强制开启**（`set_env_value("FAST_GDB_SPATIAL_PROFILE", "1")` 已移除）
  - **FAST_GDB_BENCHMARK_MODE**：支持 `steady-state`（默认）和 `fresh-open` 两种比较口径
  - **中位数 of 5**：每个覆盖率预热 1 次 + 测量 5 次
  - **查询顺序轮换**：每次 trial 轮换覆盖率顺序，避免 1% 固定承担冷页成本
  - **P95 记录**：每次摘要行输出 median 和 P95
  - **FID 全量验证**：每个 trial 的完整 FID 集合与 GDAL 一致
- `docs/evidence/spatial-query-baseline-2026-07-14.md` — 本验收记录

## 命令参考

```bash
# Release 构建
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DFAST_GDB_WITH_GDAL=ON -DFAST_GDB_BUILD_FULL_TESTS=ON -DBUILD_TESTING=ON
cmake --build build-release --target gdb_tutorial_test_runner --parallel

# 稳态基准（profile OFF, 墙钟 + FID 验证）
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 \
  ./build-release/bin/gdb_tutorial_test_runner \
  --gtest_filter="SpatialDensityBenchmark.DensityMatrix1M"

FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 FAST_GDB_RUN_10M_BENCHMARKS=1 \
  ./build-release/bin/gdb_tutorial_test_runner \
  --gtest_filter="SpatialDensityBenchmark.DensityMatrix10M"

# Fresh-open 模式
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 FAST_GDB_RUN_10M_BENCHMARKS=1 \
  FAST_GDB_BENCHMARK_MODE=fresh-open \
  ./build-release/bin/gdb_tutorial_test_runner \
  --gtest_filter="SpatialDensityBenchmark.DensityMatrix10M"

# Profile 诊断（独立运行，不混入墙钟对比）
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 FAST_GDB_RUN_10M_BENCHMARKS=1 \
  FAST_GDB_SPATIAL_PROFILE=1 \
  ./build-release/bin/gdb_tutorial_test_runner \
  --gtest_filter="SpatialDensityBenchmark.DensityMatrix10M"
```
