# fast-gdb 空间查询验收测试方案

适用分支：`main`（原始开发分支为 `agent/spatial-query-optimization`）
适用提交：`2be0938`（PR #9 已合入）
验收目标：在保证结果正确、回退语义完整和小范围查询不回归的前提下，使真实大规模 FileGDB 的中高覆盖率 bbox 查询达到或超过 GDAL。

---

## 1. 验收结论规则

本次验收分为四个等级：

| 等级 | 判定 |
|---|---|
| 通过 | 所有必须项通过，且性能目标全部满足 |
| 条件通过 | 正确性、稳定性、回归全部通过；仅个别性能目标未达标，但已定位瓶颈且无功能风险 |
| 不通过 | 出现 FID 不一致、崩溃、非法内存访问、错误回退、明显小范围性能回归或 Release 构建失败 |
| 无法验收 | 测试数据、GDAL、编译环境或基准输出不完整，无法形成可比结论 |

只有“通过”才允许把 PR 从 Draft 改为 Ready，并在计划文档中标记完成。

---

## 2. 测试环境记录

验收前必须记录以下信息：

```text
测试日期：
分支：main（后续开发须另行创建分支）
提交 SHA：
操作系统：
CPU 型号：
物理核心数：
逻辑线程数：
内存容量：
磁盘类型：NVMe / SATA SSD / HDD
文件系统：
编译器及版本：
CMake 版本：
构建类型：Release
GDAL 版本：
FAST_GDB_CURVE_BACKEND：
FAST_GDB_GEOMETRY_OUTPUT：
```

禁止用 Debug 构建做最终性能验收。

---

## 3. 测试数据要求

### 3.1 必测数据

| 数据集 | 规模 | 几何类型 | 用途 |
|---|---:|---|---|
| 自适应单元测试数据 | 1,200 | Point | 查询规划、bbox 快速路径、FID 正确性 |
| 1M 基准数据 | 1,000,000 | Polygon 或项目现有生成类型 | 中等规模性能与回归 |
| 10M 基准数据 | 10,000,000 | Polygon 或项目现有生成类型 | 核心性能验收 |

### 3.2 推荐扩展数据

条件允许时增加：

| 数据集 | 几何类型 | 目的 |
|---|---|---|
| 真实业务 FileGDB | Point | 验证 Point 高覆盖率快速接受 |
| 真实业务 FileGDB | Polyline | 验证边界候选与精确判断成本 |
| 真实业务 FileGDB | Polygon | 验证 holes、multipart、反向 ring |
| 含 Z/M/ZM 数据 | Point/Polyline/Polygon | 验证维度兼容 |
| 含曲线或 MultiPatch 数据 | 复杂几何 | 验证 fallback |
| 35GB 级真实数据 | 任意 | 验证大文件、内存和长时间稳定性 |

### 3.3 数据完整性记录

每个数据集至少记录：

```text
数据路径：
文件总大小：
图层名：
要素数：
几何类型：
图层 extent：
是否存在 .spx：
是否存在删除记录：
是否包含 Z：
是否包含 M：
是否包含曲线：
```

---

## 4. 构建验收

### 4.1 Linux / macOS Release 构建

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

### 4.2 Windows Release 构建

```powershell
cmake -S . -B build-spatial -G "Visual Studio 17 2022" -A x64 `
  -DFAST_GDB_WITH_GDAL=ON `
  -DFAST_GDB_CURVE_BACKEND=BUILTIN `
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB `
  -DFAST_GDB_BUILD_TOOLS=OFF `
  -DFAST_GDB_BUILD_FULL_TESTS=ON `
  -DBUILD_TESTING=ON

cmake --build build-spatial --config Release --target gdb_tutorial_test_runner --parallel
```

### 4.3 构建通过标准

必须满足：

- 无编译错误；
- 无链接错误；
- 无新增高风险警告；
- `query_engine_geometry.cpp`、`query_engine.h`、空间测试文件均进入实际构建；
- Release 测试可执行文件生成成功。

记录：

```text
Linux/macOS 构建：通过 / 失败
Windows 构建：通过 / 失败
主要警告：
失败日志位置：
```

---

## 5. 单元与正确性验收

### 5.1 自适应空间查询测试

```bash
ctest --test-dir build-spatial --output-on-failure \
  -R '^full\.SpatialQueryAdaptiveTest\.'
```

Windows 多配置构建：

```powershell
ctest --test-dir build-spatial -C Release --output-on-failure `
  -R '^full\.SpatialQueryAdaptiveTest\.'
```

### 5.2 必须检查的测试项

| 测试项 | 预期 |
|---|---|
| Geometry Blob 指针稳定 | 连续读取不同 FID 后首个 Blob 内容不变化 |
| 高覆盖率查询规划 | `spx_bypassed=true` |
| 高覆盖率执行路径 | `bbox:model:sequential-planned` |
| 高覆盖率结果 | FID 从 0 到 feature_count-1，递增且唯一 |
| bbox 完全包含 | `bbox_contained == feature_count`（Point 全范围测试） |
| 完整模型调用 | Point 全范围 `exact_tested == 0` |
| 低覆盖率规划 | `spx_bypassed=false` |
| 低覆盖率执行路径 | `bbox:model:spx-candidates` |
| 非法几何 | `invalid_geometries == 0`（正常测试数据） |
| Legacy API | `query_bbox()` 与 `query_bbox_unified()` 结果一致 |

### 5.3 通用回归测试

至少运行：

```bash
ctest --test-dir build-spatial --output-on-failure
```

若全量测试过多，最低要求包括：

```text
geometry
spatial predicate
WKB
Hybrid fallback
curve geometry
FID read
sequential scan
attribute query
```

### 5.4 正确性通过标准

必须满足：

- fast-gdb 与 GDAL 的完整 0-based FID 集合完全一致；
- 数量一致但 FID 不一致仍判定失败；
- 不允许重复 FID；
- FID 必须递增；
- 不允许崩溃、超时、异常退出；
- 不允许因为优化而跳过合法曲线、MultiPatch 或 Z/M/ZM 要素；
- fallback 数量不得无原因增加。

记录：

```text
Adaptive tests：通过 / 失败
全量 CTest：通过 / 失败
GDAL FID 一致性：通过 / 失败
发现的不一致 FID：
异常几何数量：
Fallback 数量：
```

---

## 6. 查询规划验收

### 6.1 默认阈值

```text
FAST_GDB_SPATIAL_DIRECT_SCAN_COVERAGE=0.35
FAST_GDB_SPATIAL_SCAN_DENSITY=0.50
```

### 6.2 覆盖率与预期路径

| 查询覆盖率 | 默认预期路径 | 关键检查 |
|---:|---|---|
| 1% | `.spx` candidates | `spx_bypassed=false` |
| 10% | `.spx` candidates | `spx_bypassed=false` |
| 30% | 可能 `.spx`，用于确定交叉点 | 对比两条路径耗时 |
| 80% | sequential planned | `spx_bypassed=true` |
| 100% | sequential planned | `spx_bypassed=true` |

### 6.3 查询规划指标

每个场景必须记录：

```text
execution_path
estimated_coverage
spx_bypassed
candidate_count
candidate_ratio
candidate_lookup_ms
```

### 6.4 规划通过标准

- 1% 和 10% 不得绕过 `.spx`；
- 80% 和 100% 必须在 `.spx` 候选物化前进入顺序扫描；
- 80% 和 100% 的 `candidate_lookup_ms` 应接近 0，或显著低于旧基线；
- 规划器不得改变最终 FID 集合；
- extent 无效时应安全回到现有索引/扫描逻辑，不能错误选择路径。

---

## 7. 1M 性能验收

### 7.1 命令

```bash
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 \
FAST_GDB_SPATIAL_PROFILE=1 \
ctest --test-dir build-spatial --output-on-failure \
  -R '^full\.SpatialDensityBenchmark\.DensityMatrix1M$'
```

Windows：

```powershell
$env:FAST_GDB_RUN_SPATIAL_BENCHMARKS="1"
$env:FAST_GDB_SPATIAL_PROFILE="1"
ctest --test-dir build-spatial -C Release --output-on-failure `
  -R '^full\.SpatialDensityBenchmark\.DensityMatrix1M$'
```

### 7.2 必须记录

| 覆盖率 | fast-gdb ms | GDAL ms | fast/GDAL | 路径 | FID一致 |
|---:|---:|---:|---:|---|---|
| 1% |  |  |  |  |  |
| 10% |  |  |  |  |  |
| 30% |  |  |  |  |  |
| 80% |  |  |  |  |  |
| 100% |  |  |  |  |  |

同时记录：

```text
candidate_lookup_ms
blob_lookup_ms
bbox_filter_ms
exact_filter_ms
bbox_rejected
bbox_contained
exact_tested
invalid_geometries
```

### 7.3 1M 通过标准

- 所有 FID 集合一致；
- 1% 和 10% 相比优化前不得回归超过 5%；
- 80% 和 100% 必须走 `sequential-planned`；
- 不要求 1M 单独决定最终是否超过 GDAL，但不得出现结构性退化。

---

## 8. 10M 核心性能验收

### 8.1 命令

```bash
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 \
FAST_GDB_RUN_10M_BENCHMARKS=1 \
FAST_GDB_SPATIAL_PROFILE=1 \
ctest --test-dir build-spatial --output-on-failure \
  -R '^full\.SpatialDensityBenchmark\.DensityMatrix10M$'
```

Windows：

```powershell
$env:FAST_GDB_RUN_SPATIAL_BENCHMARKS="1"
$env:FAST_GDB_RUN_10M_BENCHMARKS="1"
$env:FAST_GDB_SPATIAL_PROFILE="1"
ctest --test-dir build-spatial -C Release --output-on-failure `
  -R '^full\.SpatialDensityBenchmark\.DensityMatrix10M$'
```

### 8.2 重复次数

每个完整矩阵至少运行 5 次：

1. 第 1 次作为冷启动参考；
2. 第 2～5 次计算中位数；
3. 同时记录最慢值，用于观察 P95 趋势；
4. 不允许只挑最快结果。

### 8.3 10M 结果表

| 覆盖率 | Run1 | Run2 | Run3 | Run4 | Run5 | 中位数 | GDAL中位数 | fast/GDAL |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1% |  |  |  |  |  |  |  |  |
| 10% |  |  |  |  |  |  |  |  |
| 30% |  |  |  |  |  |  |  |  |
| 80% |  |  |  |  |  |  |  |  |
| 100% |  |  |  |  |  |  |  |  |

### 8.4 10M 性能目标

| 场景 | 验收目标 |
|---|---:|
| 1% | 不慢于 GDAL，且相对优化前回归不超过 5% |
| 10% | 不慢于 GDAL，且相对优化前回归不超过 5% |
| 30% | `fast-gdb <= GDAL × 0.90` |
| 80% | `fast-gdb <= GDAL × 0.80` |
| 100% | `fast-gdb <= GDAL × 0.80` |

### 8.5 10M 关键内部指标

80% 和 100% 必须重点记录：

```text
execution_path=bbox:model:sequential-planned
spx_bypassed=true
candidate_lookup_ms
bbox_contained
exact_tested
bbox_contained / feature_count
exact_tested / feature_count
```

预期趋势：

- `candidate_lookup_ms` 接近 0；
- `bbox_contained` 占绝大多数命中；
- `exact_tested` 明显低于 feature_count；
- 若 `exact_tested` 仍很高，说明需要继续实现流式边界谓词；
- 若 `exact_tested` 已很低但新增规模总耗时仍高，检查已合入的 Geometry-only Scanner 的记录布局和 I/O 批量读取。

---

## 9. 阈值校准验收

分别设置：

```bash
FAST_GDB_SPATIAL_DIRECT_SCAN_COVERAGE=0.20
FAST_GDB_SPATIAL_DIRECT_SCAN_COVERAGE=0.35
FAST_GDB_SPATIAL_DIRECT_SCAN_COVERAGE=0.50
FAST_GDB_SPATIAL_DIRECT_SCAN_COVERAGE=0.70
```

每个阈值至少运行 3 次 10M 矩阵。

记录：

| 阈值 | 10% | 30% | 80% | 100% | 最佳总体表现 |
|---:|---:|---:|---:|---:|---|
| 0.20 |  |  |  |  |  |
| 0.35 |  |  |  |  |  |
| 0.50 |  |  |  |  |  |
| 0.70 |  |  |  |  |  |

阈值选择原则：

- 不能牺牲 1% 和 10%；
- 以 30%、80%、100% 的综合中位数最优为主；
- 若不同几何类型交叉点差异明显，后续改为按几何类型配置，而不是继续使用单一全局阈值。

---

## 10. 冷缓存与热缓存验收

### 10.1 热缓存

同一进程或连续重复执行，观察稳定中位数。

### 10.2 冷缓存

条件允许时，在每次运行前清理系统文件缓存，或重启机器后执行第一轮。

Linux 示例：

```bash
sync
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
```

注意：清缓存需要管理员权限，不具备条件时明确标记“未测试冷缓存”。

### 10.3 通过标准

- 热缓存结果波动可控；
- 冷缓存不得发生异常放大、崩溃或超时；
- fast-gdb 与 GDAL 必须在相同缓存条件下比较。

---

## 11. 内存验收

### 11.1 观察项

- 峰值 RSS；
- 候选 FID vector 大小；
- 结果 FID vector 大小；
- 是否发生 swap；
- 是否出现 `std::bad_alloc`；
- 80% 和 100% 是否仍分配数百万 `.spx` 候选。

### 11.2 通过标准

- `spx_bypassed=true` 时不得物化 `.spx` 候选 vector；
- 峰值 RSS 不得相对旧实现异常增加；
- 10M 全范围测试不得触发 swap；
- 不允许内存持续增长或重复查询后泄漏。

Linux 可使用：

```bash
/usr/bin/time -v <benchmark-command>
```

Windows 可用任务管理器、Performance Monitor 或 Process Explorer 记录峰值 Working Set。

---

## 12. 稳定性验收

### 12.1 重复执行

连续执行 20 次高覆盖率查询：

```text
80% × 10 次
100% × 10 次
```

### 12.2 通过标准

- 无崩溃；
- 无结果漂移；
- 无 FID 数量变化；
- 无明显持续变慢；
- 无内存持续增长；
- 每次 `execution_path` 与规划条件一致。

记录：

```text
首次耗时：
最后一次耗时：
最小值：
最大值：
中位数：
FID数量是否一致：
RSS是否持续增长：
```

---

## 13. 异常与回退验收

必须验证：

| 场景 | 预期 |
|---|---|
| `.spx` 缺失 | 顺序扫描，结果与 GDAL 一致 |
| `.spx` 解析失败 | 顺序 fallback，错误原因明确 |
| mmap 不可用 | 候选/FID fallback，结果正确 |
| 无 Geometry 字段 | 返回 unavailable，不崩溃 |
| 空表 | 返回 empty |
| 非法 bbox | 返回 invalid |
| extent 无效 | 不使用错误覆盖率估算 |
| 曲线数据 | Builtin 正确处理或明确 fallback |
| MultiPatch | 保留原语义 |
| Z/M/ZM | XY 空间判断正确，维度不破坏 |
| 非法 Geometry Blob | 计入 invalid，不越界、不崩溃 |

通过标准：所有异常路径均可解释、可复现、结果安全，不允许静默返回错误 FID。

---

## 14. 小范围性能回归验收

1% 和 10% 查询是回归门禁。

通过标准：

- 仍走 `.spx`；
- `spx_bypassed=false`；
- 相对优化前中位数回归不超过 5%；
- FID 集合完全一致；
- 不因为规划器计算 extent 覆盖率产生显著固定开销。

若回归超过 5%，必须单独分析：

```text
planner overhead
spx initialization
candidate allocation
blob lookup
bbox peek
profiling instrumentation
```

---

## 15. 性能瓶颈判定规则

验收后按以下规则决定下一轮优化：

### 情况 A：`candidate_lookup_ms` 仍高

说明没有真正绕过 `.spx`，优先检查规划器和路径切换。

### 情况 B：`candidate_lookup_ms` 接近 0，但总耗时仍高

说明瓶颈已转移到已合入的 Geometry-only Scanner，需要继续分析记录布局、I/O 批量读取或缓存行为。

### 情况 C：`exact_tested` 占比仍高

下一步实现 Point/MultiPoint/Polyline/Polygon Streaming Predicate。

### 情况 D：单线程已接近 GDAL但仍稍慢

下一步实现固定分区并行扫描。

### 情况 E：80% / 100% 已超过 GDAL，但 30% 未达标

优先校准路径切换阈值和中密度执行策略。

---

## 16. 最终验收记录模板

```text
# fast-gdb 空间查询最终验收记录

测试日期：
提交 SHA：
测试人：
测试机器：
操作系统：
CPU：
内存：
磁盘：
编译器：
GDAL版本：

## 构建
Linux/macOS Release：通过 / 失败
Windows Release：通过 / 失败

## 正确性
Adaptive tests：通过 / 失败
全量 CTest：通过 / 失败
1M FID 一致性：通过 / 失败
10M FID 一致性：通过 / 失败
异常与 fallback：通过 / 失败

## 性能中位数
| 覆盖率 | fast-gdb | GDAL | 比率 | 是否达标 |
|---:|---:|---:|---:|---|
| 1% | | | | |
| 10% | | | | |
| 30% | | | | |
| 80% | | | | |
| 100% | | | | |

## 关键指标
80% execution_path：
80% spx_bypassed：
80% bbox_contained：
80% exact_tested：
100% execution_path：
100% spx_bypassed：
100% bbox_contained：
100% exact_tested：
峰值 RSS：

## 最终结论
通过 / 条件通过 / 不通过 / 无法验收

## 未通过项

## 下一步优化建议
```

---

## 17. 验收完成条件

以下项目必须全部完成：

- [ ] Linux/macOS Release 构建通过
- [ ] Windows Release 构建通过
- [ ] Adaptive 空间查询测试通过
- [ ] 现有几何与 Hybrid 回归通过
- [ ] 1M 完整矩阵完成
- [ ] 10M 完整矩阵完成
- [ ] fast-gdb 与 GDAL 的完整 FID 集合一致
- [ ] 1% 和 10% 无明显回归
- [ ] 30% 达到 `<= GDAL × 0.90`
- [ ] 80% 达到 `<= GDAL × 0.80`
- [ ] 100% 达到 `<= GDAL × 0.80`
- [ ] 高覆盖率成功绕过 `.spx`
- [ ] 峰值内存可接受
- [ ] 重复运行稳定
- [ ] 异常与 fallback 路径通过
- [ ] 性能和正确性结果写入最终验收记录

未勾选完所有必须项前，PR 保持 Draft。
