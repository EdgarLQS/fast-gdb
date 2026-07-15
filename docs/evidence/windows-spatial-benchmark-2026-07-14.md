# Windows 空间查询性能测试记录（2026-07-14）

## 测试范围

本记录保存当前分支在 Windows 本地环境中的 1M 空间查询结果，用于与 GDAL OpenFileGDB 驱动做同机对照。测试采用 macOS/ POSIX 空间基准的 warm-cache 参数，但当前环境只有当前分支构建，没有 `main` 基线构建，因此不构成跨平台回归门禁。

## 环境

- 分支：`agent/windows-mmap-optimization`
- Windows + MSYS2 UCRT64
- GCC：16.1.0
- GDAL：3.13.1
- 构建：Release，`FAST_GDB_WITH_GDAL=ON`，`FAST_GDB_CURVE_BACKEND=BUILTIN`
- 测试运行器：`build/bin/gdb_tutorial_test_runner.exe`
- 数据：`test_data/large/large_test.gdb`
- 数据规模：1,000,000 features，约 190.7 MiB，包含 `.spx`
- 缓存模式：steady-state / warm-cache
- 每个覆盖率：20 次
- 正确性：每档 `invalid_geometries=0`，测试通过

## 运行方式

```powershell
$env:Path = "C:\msys64\ucrt64\bin;" + $env:Path
$env:FAST_GDB_RUN_SPATIAL_BENCHMARKS = "1"
$env:FAST_GDB_BENCHMARK_PATH = (Resolve-Path "test_data\large\large_test.gdb").Path
$env:FAST_GDB_BENCHMARK_TRIALS = "20"
$env:FAST_GDB_BENCHMARK_CASE = "coverage_10pct"
./build/bin/gdb_tutorial_test_runner.exe `
  --gtest_filter=SpatialDensityBenchmark.DensityMatrixConfigured
```

实际测试对以下覆盖率分别运行了一次：`coverage_01pct`、`coverage_10pct`、`coverage_30pct`、`coverage_80pct`、`coverage_full`。

## 结果

| 覆盖率 | fast-gdb 中位数 | GDAL 中位数 | fast-gdb / GDAL | 执行路径 |
|---:|---:|---:|---:|---|
| 1% | 60.8 ms | 113.9 ms | 0.533 | `bbox:model:spx-candidates` |
| 10% | 204.6 ms | 922.2 ms | 0.222 | `bbox:model:spx-candidates` |
| 30% | 110.0 ms | 2687.1 ms | 0.041 | `bbox:model:sequential-planned` |
| 80% | 104.9 ms | 6574.8 ms | 0.016 | `bbox:model:sequential-planned` |

已完成的四档测试均显示 fast-gdb 快于 GDAL，且 FID 结果对照通过。按中位数计算，fast-gdb 约为 GDAL 的 53.3%、22.2%、4.1% 和 1.6%。

## 未完成项与限制

- `coverage_full` 在 20 次矩阵运行中因单轮总耗时达到 6 分钟而被外部超时终止；该档没有完整中位数，不能宣称通过。
- `test_data/large_10m/large_10m_test.gdb` 只有 `a00000009.gdbtable`、`a00000009.gdbtablx` 和 `.spx`，缺少 benchmark 所需的完整目录/元数据表，当前 `SpatialDensityBenchmark` 无法按 `features` 图层打开。因此本记录不填入 10M 结果。
- 以上是当前 Windows 机器上的 warm-cache 结果，不代表冷启动、其他硬件、其他 GDAL 版本或生产数据的保证值。
- macOS/POSIX 正式脚本还会使用独立的 10M `point_10m` fixture 并与 `main` 基线比较；本次 Windows 本地运行未完成该两项条件。
