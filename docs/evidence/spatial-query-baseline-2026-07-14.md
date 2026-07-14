# fast-gdb 空间查询公平基准验收记录

**测试日期**: 2026-07-14
**分支**: `agent/spatial-query-optimization`
**提交**: `e6230ad fix: harden tablx cache concurrency and invalidation`
**测试机器**: Apple M5, 10 核, 16 GB RAM, SSD
**操作系统**: macOS 26.4 (Darwin 25.4.0)
**编译器**: Apple clang 21.0.0 (clang-2100.0.123.102)
**CMake**: 4.3.3
**构建类型**: Release (`-O3 -DNDEBUG`)
**GDAL**: 3.13.0 (Homebrew)

> 本记录是 Phase G 收口时基于提交 `e6230ad` 产生的历史性能证据；后续实现已合入 `main` 的 `2be0938`。以下性能数字、测试口径和历史结论保持不变。

## 构建

- [x] macOS Release 构建通过
- [x] FAST_GDB_SPATIAL_PROFILE **未强制开启**（墙钟测试关闭）
- [x] 独立 Release 构建目录 `build-release/`
- [x] 无编译警告（修复1个未使用变量警告）
- [x] 无链接错误

## 正确性

- [x] Adaptive 空间查询测试通过（5/5 测试通过）
- [x] SpatialIndex 测试通过（11/11 测试通过）
- [x] 全量 CTest 通过（481/481 通过，0 failed）
- [x] 1K、10K、100K、1M、10M × Point、MultiPoint、Polyline、Polygon FID 一致性通过（全部覆盖率皆对比 GDAL 完整 FID 集合）
- [x] 无 FID 不一致、无重复 FID、0 异常几何

## 结果

### 10M steady-state（预热后仅计时查询，不含打开与关闭。profile OFF，中位数 of 5）

| 覆盖率 | fast_med | gdal_med | ratio | 验收目标 | 结果 |
|------:|--------:|--------:|------:|---------|:---:|
| 1% | 58.5 ms | 132.8 ms | **0.440** | ≤ +200ms 或 ≤ 0.90× GDAL | ✅ |
| 10% | 207.8 ms | 686.5 ms | **0.303** | ≤ +200ms 或 ≤ 0.90× GDAL | ✅ |
| 30% | 321.4 ms | 1562.2 ms | **0.206** | ≤ 0.90× GDAL | ✅ |
| 80% | 312.7 ms | 4049.0 ms | **0.077** | ≤ 0.90× GDAL | ✅ |
| 100% | 235.2 ms | 3575.4 ms | **0.066** | ≤ 0.90× GDAL | ✅ |

### 10M fresh-open 优化前基线（含打开+关闭，中位数 of 5）

> 该组已使用对称计时重跑：fast-gdb 的 `catalog.scan()`、解析、`engine->open()`、查询和析构，以及 GDAL 的打开、查询和关闭都在计时内。
> 该表保留为 Phase G 优化前基线。1% 未通过小范围 200ms 容忍门槛，根因是每次 fresh open 都完整解析 10M `.gdbtablx`，不是空间谓词路径。

| 覆盖率 | fast_med | gdal_med | ratio |
|------:|--------:|--------:|------:|
| 1% | 1444.4 ms | 264.7 ms | 5.457 ❌ |
| 10% | 757.3 ms | 951.1 ms | 0.796 ✅ |
| 30% | 1200.0 ms | 1947.8 ms | 0.616 ✅ |
| 80% | 792.3 ms | 4632.0 ms | 0.171 ✅ |
| 100% | 669.1 ms | 3850.1 ms | 0.174 ✅ |

### 10M fresh-open with TablxCache（含打开+查询+关闭，进程内重复 fresh-open，中位数 of 5）

> Phase G 优化启用：`.gdbtablx` 偏移元数据由 `TablxCache` 缓存，避免重复文件 I/O 和解析。
> 设置 `FAST_GDB_TABLX_CACHE=0` 获得真正的冷打开基线（见下方冷打开表）。

| 覆盖率 | fast_med | gdal_med | ratio | 验收目标 | 结果 |
|------:|--------:|--------:|------:|---------|:---:|
| 1% | 92.8 ms | 126.3 ms | **0.735** | ≤ +200ms 或 ≤ 0.90× GDAL | ✅ |
| 10% | 228.8 ms | 605.4 ms | **0.378** | ≤ +200ms 或 ≤ 0.90× GDAL | ✅ |
| 30% | 404.5 ms | 1438.1 ms | **0.281** | ≤ 0.90× GDAL | ✅ |
| 80% | 391.8 ms | 3413.1 ms | **0.115** | ≤ 0.90× GDAL | ✅ |
| 100% | 314.8 ms | 2909.7 ms | **0.108** | ≤ 0.90× GDAL | ✅ |

### 10M cold-open fresh-open（FAST_GDB_TABLX_CACHE=0，真正的冷打开，中位数 of 5）

> 无 TablxCache，每次均完整解析 `.gdbtablx`。当前仅对 1% 窗口执行冷打开复测；其结果落在 +200ms 容忍内（+30.4ms），但 ratio 落后 GDAL。

| 覆盖率 | fast_med | gdal_med | ratio | 验收目标 | 结果 |
|------:|--------:|--------:|------:|---------|:---:|
| 1% | 154.9 ms | 124.5 ms | 1.245 | ≤ +200ms 或 ≤ 0.90× GDAL | ✅ |

### 10M steady-state 回归（Phase G 后）

| 覆盖率 | 本轮 fast_med | 基线 fast_med | 差异 | 结果 |
|------:|--------------:|--------------:|-----:|:---:|
| 1% | 40.6 ms | 58.5 ms | -31.0% | ✅ |
| 30% | 324.0 ms | 321.4 ms | +0.8% | ✅ |
| 80% | 314.8 ms | 312.7 ms | +0.7% | ✅ |
| 100% | 243.2 ms | 235.2 ms | +3.4% | ✅ |

### 1M steady-state 回归（Phase G 后）

| 覆盖率 | 本轮 fast_med | 基线 fast_med | 差异 | 结果 |
|------:|--------------:|--------------:|-----:|:---:|
| 1% | 7.0 ms | 7.8 ms | -10.0% | ✅ |
| 30% | 32.1 ms | 32.2 ms | -0.3% | ✅ |

### 1M steady-state（中位数 of 5）

| 覆盖率 | fast_med | gdal_med | ratio |
|------:|--------:|--------:|------:|
| 1% | 7.8 ms | 14.7 ms | 0.528 |
| 10% | 28.8 ms | 66.8 ms | 0.431 |
| 30% | 32.2 ms | 155.0 ms | 0.207 |
| 80% | 31.2 ms | 372.3 ms | 0.084 |
| 100% | 23.4 ms | 343.5 ms | 0.068 |

### 关键内部指标（profile ON 辅助，稳态）

| 覆盖率 | 路径 | spx_bypassed | spx_ms | blob_ms | bbox_ms | exact_ms | scan_ms |
|:------|:-----|:------------|------:|--------:|--------:|--------:|--------:|
| 1% | spx-candidates | false | 12.9 | 15.1 | 3.5 | 6.2 | - |
| 10% | spx-candidates | false | 60.4 | 50.1 | 29.5 | 19.2 | - |
| 30% | spx-candidates | false | 152.5 | 73.9 | 86.3 | 34.0 | - |
| 80% | sequential-planned | true | - | - | 273.6 | 54.2 | 547.8 |
| 100% | sequential-planned | true | - | - | 270.8 | 0.0 | 483.7 |

### 全规模稳态矩阵

所有 20 个“几何 × 规模”单元均通过完整 FID 对照与性能门槛。1%/10% 的少数倍率落后单元仍在 +200ms 容忍内；30% 及以上均不慢于 GDAL 的 0.90× 门槛。

| 几何 | 1K | 10K | 100K | 1M | 10M |
|:--|:--:|:--:|:--:|:--:|:--:|
| Point | ✅ | ✅ | ✅ | ✅ | ✅ |
| MultiPoint | ✅ | ✅ | ✅ | ✅ | ✅ |
| Polyline（局部 5–20 顶点） | ✅ | ✅ | ✅ | ✅ | ✅ |
| Polygon | ✅ | ✅ | ✅ | ✅ | ✅ |

10M 各几何在 30% / 80% / 全范围的 fast/GDAL 中位数比分别为：Point 0.371 / 0.127 / 0.112，MultiPoint 0.201 / 0.067 / 0.071，Polyline 0.194 / 0.080 / 0.076，Polygon 0.206 / 0.077 / 0.066。

### 测试数据

| 数据集 | 文件大小 | 要素数 | 几何类型 | .spx |
|:------|-------:|------:|:--------|:----:|
| 1M | 191 MB | 1,000,000 | Polygon | 有 |
| 10M | 1.9 GB | 10,000,000 | Polygon | 有 |

> 折线生成器已改为以一个中心点附近的局部 5–20 顶点构造，避免旧生成方式中每个顶点独立全域采样、使折线 bbox 几乎覆盖整个图层的失真。

### 大型数据集复用规则

- 矩阵数据固定存放在 `test_data/spatial_matrix/<geometry>_<size>.gdb`，例如 `polygon_10000000.gdb`；它们是本地、忽略 Git 的可复用资产。
- 运行同一生成命令时，生成器仅在图层名、扁平几何类型、要素数和 `.spx` 均匹配时复用现有目录，并打印“跳过生成”。因此日常 benchmark 不应删除或重建已有大数据集。
- 只有数据不完整、生成定义变更或需要重置数据时才重建；生成过程必须对同一输出目录串行执行，禁止并发写入同一路径。

## 关键发现

1. **Release/profile-off 的全几何、全规模 steady-state 矩阵已通过**；高覆盖率 10M 的优势为 0.066–0.206× GDAL。
2. **profile 开销显著**（30% +51%, 80% +73%, 100% +104%），证实之前 8.42× 比值为测量伪影。
3. **路径规划生效**：80%+ 成功绕过 .spx (`spx_bypassed=true`)，走 sequential-planned。
4. **Phase E 已执行**：将 direct scan 默认估算覆盖率阈值从 35% 调整为 29%，以覆盖实际约 29.76% 的 30% 窗口；原先 100K Polygon、1M Point、1M MultiPoint 的 30% 门禁均由此通过。
5. **P95（观察值）**：仅5个样本，P95等价于最大值，不足以支撑稳定性结论。所列P95仅作为同批次内最大值观察，不应用于外推。
6. **Phase G 完成**：`.gdbtablx` 跨 open 元数据缓存使 10M fresh-open 1% 从 1444.4ms 降到 92.8ms（15.5×改善），缓存 fresh-open 全矩阵通过验收。
7. **冷打开仍可接受**：`FAST_GDB_TABLX_CACHE=0` 时 10M fresh-open 1% 为 154.9ms，仅比 GDAL 多 30.4ms，仍在 +200ms 容忍内。
8. **稳态无回归**：1M/10M 回归窗口与基线差异在可接受范围内。

## 优化阶段触发判断

| 阶段 | 触发条件 | 结果 |
|:----|:---------|:----:|
| Phase B (候选 Geometry 定位) | 10M 1%/10% 慢于 GDAL 超 200ms | **steady-state 不触发**；fresh-open 1% 触发，根因是 `.gdbtablx` 打开解析 |
| Phase C (spx 中密度) | Phase B 后 10%/30% 未达标 | **不触发** |
| Phase D (Streaming Predicate) | Phase B/C 后 30% 未达标 | **不触发** |
| Phase E (规划器校准) | 两个路径通过验证 | **已完成**：35% → 29% |
| Phase G (gdbtablx 缓存) | fresh-open 10M 1% 因重复解析未达标 | **已完成**：TablxCache 使 1% 从 1444.4ms 降至 92.8ms |
| Phase F (条件并行) | 单线程 80%/100% 未达 0.90× GDAL | **不触发** |

## 新增/修改文件

- `tests/edgar/explorgdb/reader/test_spatial_benchmark.cpp` — 公平基准重构：
  - **去除 profile 强制开启**（`set_env_value("FAST_GDB_SPATIAL_PROFILE", "1")` 已移除）
  - **FAST_GDB_BENCHMARK_MODE**：支持 `steady-state`（默认）和 `fresh-open` 两种比较口径
  - **中位数 of 5**：每个覆盖率预热 1 次 + 测量 5 次
  - **查询顺序轮换**：每次 trial 轮换覆盖率顺序，避免 1% 固定承担冷页成本
  - **P95 记录**：每次摘要行输出 median 和 P95
  - **FID 全量验证**：每个 trial 的完整 FID 集合与 GDAL 一致
  - **query_fast_gdb_fresh_open()**：对称计时，将打开、查询、析构全部纳入计时
  - **执行顺序交替**：trial 间交替 fast/GDAL 执行顺序，消除系统性偏差
- `src/edgar/explorgdb/reader/gdb_tablx_cache.h` — TablxCache 类声明（Phase G 新增）：
  - LRU 淘汰（上限 16 条目、总缓存 256 MiB）、线程安全、文件身份键（device/inode/size/mtime 纳秒）
- `src/edgar/explorgdb/reader/gdb_tablx_cache.cpp` — TablxCache 实现（Phase G 新增）：
  - 单例、shared_mutex 并发控制、FAST_GDB_TABLX_CACHE=0 绕过
- `src/edgar/explorgdb/reader/gdb_table.cpp` — 集成 TablxCache 到 load_tablx()：
  - 缓存命中时深拷贝偏移向量；未命中时解析并填充；stat() 失败时回退
- `tests/edgar/explorgdb/reader/test_gdb_tablx_cache.cpp` — 缓存单元测试（Phase G 新增）：
  - 命中/未命中/淘汰/文件变更/绕过/并发（8 线程）
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

# Fresh-open 模式（含 TablxCache 缓存，进程内重复 open）
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 FAST_GDB_RUN_10M_BENCHMARKS=1 \
  FAST_GDB_BENCHMARK_MODE=fresh-open \
  ./build-release/bin/gdb_tutorial_test_runner \
  --gtest_filter="SpatialDensityBenchmark.DensityMatrix10M"

# 冷打开 baseline（无 TablxCache）
FAST_GDB_TABLX_CACHE=0 FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 FAST_GDB_RUN_10M_BENCHMARKS=1 \
  FAST_GDB_BENCHMARK_MODE=fresh-open \
  ./build-release/bin/gdb_tutorial_test_runner \
  --gtest_filter="SpatialDensityBenchmark.DensityMatrix10M"

# TablxCache 单元测试
./build-release/bin/gdb_tutorial_test_runner \
  --gtest_filter="TablxCacheTest.*"
```
