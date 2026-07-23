> **历史归档**：本文记录已废弃的 Writer 方案或阶段性证据，不代表当前产品能力。fast-gdb 当前仅提供 Reader；现行边界见 [ADR-007](../../../adr/ADR-007-reader-only-gdal-edit-boundary.md)，归档说明见 [Writer 历史索引](../README.md)。

# Writer macOS 性能专项（M18.3）

本文冻结 M18.3 的 macOS 性能矩阵、current/main/GDAL 比较规则、profile 证据和回归判定。它不改变 Writer 的正确性契约，也不把单次耗时或历史结果当作当前验收。

## 1. 固定入口

- 性能 manifest：`tests/contracts/writer-macos-performance-v1.json`
- current/main/GDAL 比较器：`scripts/compare_writer_performance.py`
- macOS `sample(1)` 执行器：`scripts/run_macos_sample_profile.py`
- Writer-only 分段 profile 驱动：`tools/writer_profile/`
- GitHub Actions：`.github/workflows/writer-macos-performance.yml`
- 自动证据目录：`writer-performance/`

普通性能矩阵继续复用 `PerformanceBenchmarkFixture.W7_*`、`W8_*` 和 `W9_*`，避免为性能专项复制业务断言。profile 驱动只用于把稳定 `WriterSession` 的 schema、open、行循环、commit 和 GDAL reopen 分段，并生成足够长的 Writer-only 进程供 macOS `sample` 采样。

## 2. 场景范围

### P0

| 场景 | 数据 | 既有 macOS 结论 |
|---|---|---|
| `P0-WIDE-010/050/100` | 10K Point，10/50/100 个 Float64 | Writer/GDAL 中位耗时约 6.2–7.6 倍 |
| `P0-POINT-XY/XYZ/XYM/XYZM` | 10K Point，仅改变 Z/M 维度 | Writer/GDAL 中位耗时约 10.1–11.8 倍 |

### P1

- Point、10 点 Polyline、Polygon、3×10 multipart line、带洞 Polygon；
- Reader 10M fresh-open 只保留为显式手工观察项，不与 Writer 在同一优化提交中改动。

Linux、Windows、50M 和跨平台绝对性能比较仍不进入本里程碑。

## 3. current/main/GDAL 判定

同一个 macOS runner 上分别构建：

1. 当前提交；
2. 运行时 `main`；
3. 每个构建中的 GDAL OpenFileGDB 基线。

所有结果必须使用同一 manifest、Release 构建、GDAL 版本、架构、样本数和 `warm-recreate` 缓存状态。每个业务场景必须先满足：

- `correct=true`；
- manifest 完全一致；
- 数量、FID、属性、几何和 GDAL reopen 断言通过；
- `median_ms` 为正数且证据是 schema v2。

比较器仅对 **current Writer 相对 main Writer** 应用默认 5% 回退门禁：

```text
(current_median - main_median) / main_median <= 5%
```

`current/GDAL` 只记录真实倍数，不要求所有场景快于 GDAL。current 与 main 的 GDAL 差异也写入结果，用于识别 runner 噪声，但当前不作为独立失败门禁。

## 4. profile 证据

自动 profile 先覆盖两个 P0 放大场景：

- `P0-WIDE-100-PROFILE`：100 个 Float64，100K 行；
- `P0-POINT-XYZM-PROFILE`：XYZM Point，2M 行。

每项生成：

- `*.phases.json`：`schema_ms`、`open_ms`、`write_loop_ms`、`commit_ms`、`reopen_ms`、吞吐、磁盘和正确性；
- `*.sample.txt`：macOS `sample(1)` 原始调用树；
- `*.profile.json`：按字段校验、几何编码、RowBuffer/内存复制、buffered write、flush/close/tablx、open discovery、publish、GDAL schema/reopen 分类的 inclusive sample 计数；
- `*.command.log`：完整命令及程序输出。

`sample(1)` 分类是调用树 inclusive 计数，会在父子栈中重复出现，不能当作互斥 wall-clock 百分比。优化决策必须同时查看原始调用树、分段 JSON 和 W7/W8/W9 的 current/main/GDAL 中位数。

## 5. 自动工作流

`writer-macos-performance` 在相关 Writer、benchmark、manifest 或性能脚本变化的 PR 上运行，也支持手工触发。流程为：

1. 同时 checkout current 和 `main`；
2. 使用完全相同的 Release/GDAL 配置构建两个完整测试 runner；
3. current 与 main 分别执行 W7/W8/W9，样本默认 3 次；
4. 比较器输出 JSON、CSV 和 Markdown，并执行 5% current-vs-main 门禁；
5. 安装 current package，构建稳定 API profile 驱动；
6. 对宽字段和 XYZM Point 采集 Writer-only 分段与 `sample(1)`；
7. 上传 manifest、benchmark JSON、Google Test JSON、日志、比较结果和 raw profile。

手工输入 `run_reader_10m=true` 时，额外执行 Reader 10M fresh-open 观察项。该项不改变 Writer 回归门禁。

## 6. 本地 current/main 比较

先以相同配置分别构建当前工作树与 main 工作树，然后设置独立输出目录：

```bash
export FILTER='PerformanceBenchmarkFixture.W7_WideAttributes_*:PerformanceBenchmarkFixture.W8_Geometry_*:PerformanceBenchmarkFixture.W9_Dimension_*'

FAST_GDB_RUN_FULL_BENCHMARKS=1 \
FAST_GDB_BENCHMARK_SAMPLES=3 \
FAST_GDB_BENCHMARK_OUTPUT_DIR="$PWD/writer-performance/current" \
FAST_GDB_BENCHMARK_CODE_VERSION="$(git rev-parse HEAD)" \
FAST_GDB_BENCHMARK_CACHE_STATE=warm-recreate \
./build-perf-current/bin/gdb_tutorial_test_runner \
  --gtest_filter="$FILTER"
```

main 工作树使用相同命令，只替换 binary、输出目录和 code version。随后执行：

```bash
python3 scripts/compare_writer_performance.py \
  --manifest tests/contracts/writer-macos-performance-v1.json \
  --current writer-performance/current \
  --main writer-performance/main \
  --output-dir writer-performance/comparison
```

## 7. 优化提交规则

M18.3 后续每个优化必须：

1. 先由本合同生成 profile 证据；
2. 只修改一个已证明瓶颈；
3. 独立提交，提交说明指向 profile 文件和主要调用栈；
4. 复跑 current/main/GDAL 及正确性矩阵；
5. current 相对 main 不得回退超过 5%；
6. 未达到 GDAL 时保留真实倍数和下一瓶颈，不删除或弱化场景。

本次先完成测量、比较和 profile 基础设施。由于仓库 Actions 当前存在“job 在任何 step 前失败”的外部问题（#12），在新的 macOS artifact 实际生成前，M18.3 不能标记为验收通过，也不基于猜测提交 Writer/Reader 算法优化。
