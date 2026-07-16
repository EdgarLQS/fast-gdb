# Reader 10M fresh-open macOS 验收记录（2026-07-15）

## 结论

Point、MultiPoint、Polyline 的 10M fresh-open 五档矩阵均完成 5 次采样。所有场景的 fast-gdb FID
集合与 GDAL 完全一致，`invalid_geometries=0`；三类数据均有性能档位未达到现有门禁，因此本次结论是
“正确性通过、性能未验收”，不能声明 fresh-open 全矩阵优于 GDAL。

环境：macOS Darwin 25.4.0 arm64、Apple M5、16 GiB RAM、GDAL 3.13.0、提交 `6e3597a`。
模式为 `fresh-open`，未清理 OS page cache，不标记为 strict-cold。

## 中位数结果

单位为毫秒；比值为 fast-gdb / GDAL，小于 1 表示 fast-gdb 更快。

| 几何 | 覆盖率 | fast-gdb | GDAL | 比值 | 门禁 |
|---|---:|---:|---:|---:|---|
| Point | 1% | 120.3 | 51.2 | 2.350 | 通过 +200ms 小查询容差 |
| Point | 10% | 545.0 | 219.7 | 2.480 | 失败 |
| Point | 30% | 1363.0 | 571.2 | 2.386 | 失败 |
| Point | 80% | 1406.4 | 1446.6 | 0.972 | 失败，未达到快 10% |
| Point | 100% | 1429.0 | 1444.0 | 0.990 | 失败，未达到快 10% |
| MultiPoint | 1% | 197.4 | 101.3 | 1.949 | 通过 +200ms 小查询容差 |
| MultiPoint | 10% | 871.5 | 540.9 | 1.611 | 失败 |
| MultiPoint | 30% | 1838.4 | 1422.8 | 1.292 | 失败 |
| MultiPoint | 80% | 1914.6 | 3651.0 | 0.524 | 通过 |
| MultiPoint | 100% | 1935.8 | 3253.8 | 0.595 | 通过 |
| Polyline | 1% | 187.1 | 127.3 | 1.470 | 通过 +200ms 小查询容差 |
| Polyline | 10% | 808.9 | 646.0 | 1.252 | 通过 +200ms 小查询容差 |
| Polyline | 30% | 2097.1 | 1529.2 | 1.371 | 失败 |
| Polyline | 80% | 2274.3 | 3585.6 | 0.634 | 通过 |
| Polyline | 100% | 2101.8 | 3309.6 | 0.635 | 通过 |

门禁规则保持不变：覆盖率不超过 10% 时允许 `GDAL + 200ms` 或 fast-gdb 不超过 GDAL 的 90%；
更高覆盖率要求 fast-gdb 不超过 GDAL 的 90%。本次没有为了得到通过结果修改阈值。

## 执行命令

每类数据分别执行：

```bash
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 \
FAST_GDB_BENCHMARK_PATH="$PWD/test_data/spatial_matrix/<geometry>_10000000.gdb" \
FAST_GDB_BENCHMARK_LABEL='macOS / <geometry> / 10m / fresh-open' \
FAST_GDB_BENCHMARK_MODE=fresh-open \
FAST_GDB_BENCHMARK_TRIALS=5 \
./build/bin/gdb_tutorial_test_runner \
  --gtest_filter='SpatialDensityBenchmark.DensityMatrixConfigured'
```

`<geometry>` 依次为 `point`、`multipoint`、`line`。数据为本地可复用 fixture，不提交仓库。

## 后续触发点

1. 10%/30% 的主要差距需要分别拆分 catalog/table 打开、`.spx` 载入、候选读取和顺序扫描成本；
2. Point 的高覆盖率只与 GDAL 接近，MultiPoint/Polyline 高覆盖率已有明确优势，不应使用同一优化假设；
3. 优化必须保持完整 FID 集合和 `invalid_geometries=0`，并以同数据、同模式复跑本表；
4. Windows/Linux 后续使用相同场景名和门禁，不拿本机绝对耗时作为跨平台基线。

## 第一轮定位与优化

将 Point 10M、10%、fresh-open、单次采样缩为约 2 秒的红灯。profile 显示原始约 536ms 中，`.spx`
候选读取约 200ms、百万候选 Blob 定位约 218ms、bbox 判断约 76ms；steady-state 与 fresh-open 接近，
说明 Catalog/打开阶段不是主要瓶颈。

`.spx` 改为只读 mmap 并保留 `pread` 回退后，五次采样中位数从 536ms 级降至 419.4ms，`.spx`
阶段单次降至约 85ms，约改善 22%。GDAL 中位数为 216.9ms，该场景仍比 `GDAL + 200ms` 门禁慢
约 2.5ms，仍判失败。30% 场景主要走顺序几何扫描，mmap `.spx` 不解决该档；后续应优化通用
geometry-only scan/Point 快速路径，而不是继续调整索引阈值或放宽门禁。
