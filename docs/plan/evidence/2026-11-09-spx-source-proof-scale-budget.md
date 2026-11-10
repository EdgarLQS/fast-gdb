# 空间源证明：规模、内存与候选预算端到端验证

日期：2026-11-09。适用源码：`ffc6240` 之后的本地工作区（源证明查询接入已完成）。

状态：本地规模矩阵、冷建/暖查指标、缓存与候选预算端到端已取得证据；物理冷启动 I/O（清 OS 文件缓存）、真实 ArcGIS 多级索引、Windows/特殊语料仍未验收。本文数字只描述本机、本夹具、本配置，不构成全面提速结论。

## 一、方法与环境

- 平台：macOS（Apple Silicon），GDAL 3.13.0 "Iowa City"。
- 构建：`build-s4-release`（Release，`gdb_tutorial_test_runner`）。
- 夹具：`tests/createdata/cpp/generate_large_gdb` 生成 uniform Point 图层 `features`，范围 `[0,0,100000,100000]`，固定随机种子。规模 1K/10K/100K/1M/10M；10M 夹具 403 MB，其中 `.spx` 约 120.8 MB。夹具位于 `/private/tmp`，不提交。
- 冷 `cold`：每个样本新建 `GdbCatalog` + `QueryEngine`，首次查询计时；`build_ms` 为 `source_directory_build_ms`，即同代源目录构建与完整覆盖证明耗时。
- 暖 `warm`：同一引擎先预热，再重复查询；只计查询本身。
- 默认 5 轮取中位数；10M 为 5 轮；隔离 RSS 运行使用 3 轮并在下表中注明。
- `fresh-open` 不等于物理冷 I/O：本轮未清 OS 文件缓存（strict cold 需要可用的外部清缓存命令/权限，本轮未执行）。
- 覆盖率档位由图层 extent 计算；`kDefaultDirectScanCoverage=0.29`，达到阈值的 80%/全窗口由计划器直接顺序扫描，不构建源证明。
- FID 真值：同进程 GDAL `SetSpatialFilterRect` 结果；每轮断言 Fast/GDAL FID 集合相等（10M 全窗口 800 万/1000 万级集合亦比较通过）。

## 二、规模矩阵（中位数）

### 冷建（fresh-open，每个样本重新打开）

| 规模 | 窗口 | 候选 | 候选比例 | 路径 | 总耗时 ms | 建证明 ms | 目录字节 | 叶页 | 页读 |
|---|---|---:|---:|---|---:|---:|---:|---:|---:|
| 1K | 1% | 340 | 34.00% | spx-candidates | 0.7 | 0.2 | 1,678 | 3 | 5 |
| 1K | 10% | 680 | 68.00% | spx-candidates | 0.9 | 0.3 | 1,678 | 3 | 6 |
| 1K | 25% | 1000 | 100% | spx-candidates | 0.8 | 0.2 | 1,678 | 3 | 7 |
| 1K | 80% | 1000 | 100% | spx-candidates | 0.8 | 0.3 | 1,678 | 3 | 7 |
| 10K | 1% | 1360 | 13.60% | spx-candidates | 1.6 | 1.2 | 3,206 | 30 | 35 |
| 10K | 10% | 3400 | 34.00% | spx-candidates | 1.7 | 1.2 | 3,206 | 30 | 41 |
| 10K | 25% | 5440 | 54.40% | sequential-adaptive | 1.8 | 1.2 | 3,206 | 30 | 47 |
| 10K | 80% | 10000 | 100% | sequential-planned | 0.5 | 0 | 0 | 0 | 0 |
| 100K | 1% | 10200 | 10.20% | spx-candidates | 12.5 | 11.6 | 18,046 | 295 | 326 |
| 100K | 10% | 32300 | 32.30% | spx-candidates | 13.5 | 11.6 | 18,046 | 295 | 391 |
| 100K | 25% | 50660 | 50.66% | sequential-adaptive | 14.8 | 11.4 | 18,046 | 295 | 445 |
| 100K | 80% | 100000 | 100% | sequential-planned | 2.3 | 0 | 0 | 0 | 0 |
| 1M | 1% | 79220 | 7.92% | spx-candidates | 161.9 | 156.4 | 166,782 | 2,942 | 3,177 |
| 1M | 10% | 310760 | 31.08% | spx-candidates | 170.1 | 153.2 | 166,782 | 2,942 | 3,858 |
| 1M | 25% | 501500 | 50.15% | sequential-adaptive | 190.6 | 159.5 | 166,782 | 2,942 | 4,419 |
| 1M | 80% | 1000000 | 100% | sequential-planned | 21.1 | 0 | 0 | 0 | 0 |
| 10M | 1% | 314500 | 3.15% | spx-candidates | 2048.6 | 2011.7 | 1,653,470 | 29,412 | 30,339 |
| 10M | 10% | 1677560 | 16.78% | spx-candidates | 2111.5 | 2009.6 | 1,653,470 | 29,412 | 34,348 |
| 10M | 25% | 10000000 | 100% | sequential-fallback | 2280.9 | 2036.8 | 1,653,470 | 29,412 | 35,583 |
| 10M | 80% | 10000000 | 100% | sequential-planned | 211.8 | 0 | 0 | 0 | 0 |
| 10M | 全窗口 | 10000000 | 100% | sequential-planned | 204.3 | 0 | 0 | 0 | 0 |

说明：1K/10K 的叶页粒度粗，1% 窗口的候选比例仍然很高；25% 在 10K/100K/1M 被候选密度计划器转为 `sequential-adaptive`；10M 的 25% 窗口超过 2,097,152 候选预算，完整回退为 `sequential-fallback`（见第三节）。页读包含证明构建与候选读取。

### 暖查（同一引擎 / 同代缓存）

| 规模 | 1% 中位 ms | 10% 中位 ms | 25% 中位 ms | 80% 中位 ms | 全窗口中位 ms | 建证明 ms |
|---|---:|---:|---:|---:|---:|---:|
| 1K | 0.2 | 0.2 | 0.2 | 0.2 | 0.4 | 0 |
| 10K | 0.2 | 0.3 | 0.4 | 0.2 | 0.2 | 0 |
| 100K | 0.5 | 1.6 | 2.9 | 2.0 | 1.8 | 0 |
| 1M | 3.7 | 15.4 | 29.7 | 19.1 | 17.9 | 0 |
| 10M | 22.9 | 86.2 | 220.9 | 197.3 | 179.0 | 0 |

暖查 `source_directory_hit=true`（除 80%/全窗口直接计划扫描，不构建证明）；构建耗时只在冷样本计入，暖查询不重复构建。p95 仅 5 个样本，见日志，不单独作为稳定分布结论。

### 峰值 RSS（进程级 `getrusage`，含 mmap 驻留页）

| 规模 | 主运行（Fast+GDAL 同进程，冷/暖） | Fast-only（3 轮，冷/暖） | GDAL-only（3 轮，冷） |
|---|---|---|---|
| 1K | 51.4 / 51.5 MB | — | — |
| 10K | 51.6 / 51.7 MB | — | — |
| 100K | 59.7 / 59.9 MB | — | — |
| 1M | 120.7 / 124.0 MB | 94.6 / 103.3 MB | — |
| 10M | 662.8 / 746.3 MB | 486.5 / 570.1 MB | 412.0 MB |

主运行同进程包含 GDAL 参考查询，不能当作 Fast 私有堆内存；Fast-only/GDAL-only 分进程结果来自 `fast_*.log` / `gdal_10000000.log`。10M 的峰值主要来自 Fast mmap 的文件驻留页；GDAL 10M 全窗口冷样本同为数百 MB，说明该指标包含映射/缓存页，不能解释为私有堆分配。

## 三、候选预算端到端

命令：

```sh
FAST_GDB_RUN_BUDGET_TESTS=1 \
FAST_GDB_BENCHMARK_PATH=/private/tmp/fast-gdb-source-proof-10000000.gdb \
build-s4-release/bin/gdb_tutorial_test_runner \
  --gtest_filter='SpatialSourceBudgetTest.*' --gtest_brief=1
```

输出（10M uniform Point）：

```text
[budget] features=10000000 coverage=25.00% cold_path=bbox:model:sequential-fallback
  cold_candidates=10000000 build_ms=3177.3 dir_bytes=1653470 leaves=29412
  entries=10000000 pages=35583 matched=2501007 warm_hit=true
  warm_path=bbox:model:sequential-fallback
  reason=spatial source candidate selection failed: spatial source candidate budget exceeded; sequential scan used
[selective] features=10000000 coverage=0.100% path=bbox:model:spx-candidates
  candidates=78540 ratio=0.785% dir_bytes=1653470 pages=29645 matched=9880 reason=
```

结论：证明先完整构建并缓存；25% 窗口的叶页候选超过 16 MB / 2,097,152 上限时，`SpatialSourceIndex::query` 明确返回 `spatial source candidate budget exceeded`，引擎完整回退，不发布部分候选；第二次查询命中缓存但仍在候选阶段失败并回退；低覆盖窗口正常使用证明候选。两条路径的 FID 均与 GDAL 一致。目录 16M slots / 16 MB 预算仍由单元测试覆盖，本轮未生成 16M+ slots 端到端夹具。

缓存淘汰与活跃共享引用：现有单测 `LruAndByteBudgetEvictionKeepExistingReadersValid`、`ConcurrentColdReadersShareOneProof`、`CatalogCopiesShareProofsButRescanStartsEmptyEvenForUnchangedIdentity` 继续通过；本轮未新增多图层淘汰端到端用例。

## 四、回归

- macOS Release 全套（最终源码）：646 项，100% 通过，0 失败，110 跳过（`ctest_macos_final.log`）。
- GDAL-free 构建与全套：147 项，100% 通过（`ctest_off.log`）。
- 安装后 `linear` package consumer：configure/build/1 项 CTest 通过（`install_linear.log`、`consumer_linear_*.log`）；`hybrid`/`adaptive`/`unified` consumer 本轮未重跑，沿用 2026-11-08 证据，不把线性结果扩大为四类全部通过。
- 新增/修改定向：`SpatialSourceIndexTest.*`、`SpatialSourceDirectoryTest.*`、`SpatialSourceCacheTest.*`、`SpatialWhereIndexFallbackTest.SpatialBboxReportsSourceProofBuildAndQueryDiagnostics`、`SpatialSourceBudgetTest.*`。公开 `SpatialQueryMetrics` 增加诊断字段，下游需要重新编译；不承诺二进制 ABI 不变。
- **环境缺口**：`/Users/edgarlqs/Downloads/testcurve.gdb` 当前不存在。首次全量运行带 `FAST_GDB_CURVE_DATASET` 时，21 个依赖该样本的用例（hybrid 10、full 11，含 `IndependentReadersReturnIdenticalFeatureDigests`）因 GDAL 打不开路径失败；去掉该变量后相关曲线/真实数据/并发用例跳过，其余 646 项全部通过。该失败是样本缺失，不是代码回归，也不能记为通过。
- Linux Release / sanitizer：本机 Docker daemon（colima）未运行，本轮未取得 Linux 证据；不把 macOS 结果替代跨平台验收。
- strict cold（清 OS 缓存）：未执行，本轮所有冷样本都是 fresh-open。

## 五、结论与边界

1. 源证明冷建成本随规模近似线性：1K≈0.2 ms、100K≈12 ms、1M≈156 ms、10M≈2.0 s；暖查只读候选叶页：10M 的 1%/10% 窗口约 23/86 ms。
2. 10M 全窗口直接计划扫描约 204 ms（冷 fresh-open）/179 ms（暖）；GDAL fresh-open 同窗口约 1.1 s。该对比未清 OS 缓存、Fast 使用自有 mmap/索引，不作为全面性能结论。
3. 候选预算按设计 fail closed：超大候选完整回退且 FID 与 GDAL 一致；证明缓存不会把预算失败伪装成成功。
4. 真实 ArcGIS 多级索引、物理冷 I/O、Linux/Windows、曲线保守包络与 3 个 CRS 图层根因仍未验收；总计划 7 个外部项保持未勾选。

## 六、复现命令

```sh
# 1. 生成夹具（1K/10K/100K/1M/10M，uniform Point）
./build-s4-release/bin/generate_large_gdb --count 10000000 --geometry point \
  --distribution uniform --output /private/tmp/fast-gdb-source-proof-10000000.gdb

# 2. 冷建 + 暖查矩阵（5 轮；Fast/GDAL 同进程）
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 \
FAST_GDB_BENCHMARK_PATH=/private/tmp/fast-gdb-source-proof-10000000.gdb \
FAST_GDB_BENCHMARK_LABEL=10000000_point FAST_GDB_BENCHMARK_TRIALS=5 \
build-s4-release/bin/gdb_tutorial_test_runner \
  --gtest_filter='SpatialSourceProofBenchmark.*' --gtest_brief=1

# 3. 分离 backend 记录峰值 RSS（各自独立进程）
FAST_GDB_BENCHMARK_BACKEND=fast ...   # 运行两个 matrix
FAST_GDB_BENCHMARK_BACKEND=gdal ...   # 只运行 ColdBuildMatrix

# 4. 候选预算端到端（10M）
FAST_GDB_RUN_BUDGET_TESTS=1 \
FAST_GDB_BENCHMARK_PATH=/private/tmp/fast-gdb-source-proof-10000000.gdb \
build-s4-release/bin/gdb_tutorial_test_runner \
  --gtest_filter='SpatialSourceBudgetTest.*' --gtest_brief=1

# 5. 全量回归
FAST_GDB_REAL_DATASET=$PWD/test_data/gdb/acceptance_metadata.gdb \
  ctest --test-dir build-s4-release --output-on-failure
```

## 七、日志

- `2026-11-09-spx-source-proof-scale-budget/bench_1000.log` … `bench_10000000.log`
- `2026-11-09-spx-source-proof-scale-budget/budget_10m.log`
- `2026-11-09-spx-source-proof-scale-budget/fast_*.log`、`gdal_10000000.log`（隔离 RSS）
- `2026-11-09-spx-source-proof-scale-budget/ctest_macos.log`（带缺失 `FAST_GDB_CURVE_DATASET`，21 项失败）
- `2026-11-09-spx-source-proof-scale-budget/ctest_macos_nocurve.log`（去掉缺失样本变量后 646 项通过）
- `2026-11-09-spx-source-proof-scale-budget/ctest_macos_final.log`、`ctest_off.log`
- `2026-11-09-spx-source-proof-scale-budget/install_linear.log`、`consumer_linear_*.log`
- `2026-11-09-spx-source-proof-scale-budget/environment.txt`
