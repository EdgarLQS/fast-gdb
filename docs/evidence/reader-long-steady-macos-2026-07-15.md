# Reader 30 分钟长稳 macOS 验收记录

- **日期**：2026-07-15
- **代码版本**：`74a7eea`
- **环境**：macOS arm64、Apple M5、16 GiB、Clang 21.0、GDAL 3.13.0
- **入口**：`PerformanceBenchmarkFixture.R7_ReaderLongSteady_100KCycles`

## 方法

现场生成并建立空间索引的 100K 四字段 Polygon GDB。预热后，每轮重新扫描 catalog、解析图层并打开
QueryEngine，依次执行固定 bbox 空间查询、FID 0 回读和 100K 全表扫描。空间查询必须始终返回预热时
的 4023 个 FID，FID 记录必须有效，全表扫描必须正好返回 100,000 行；任一偏差立即停止。RSS 从预热
后开始计算，默认增长门槛 32 MiB。

## 结果

| 指标 | 结果 |
|---|---:|
| 门禁内总时长 | 1800.003 秒 |
| 完整循环数 | 51,904 |
| 累计全表扫描 | 5,190,400,000 行 |
| 循环中位数 | 34.416 ms |
| 循环 p95 | 34.667 ms |
| 折算扫描吞吐 | 2,905,594 features/s |
| 固定空间命中 | 4023 FID/轮 |
| 预热后 RSS 起点 | 93.609 MiB |
| 峰值 RSS | 93.625 MiB |
| RSS 增长 | 0.016 MiB |
| 最终正确性 | 通过 |

Google Test 总进程时间为 1800.915 秒，包含数据生成、索引和预热。51,904 轮均通过空间结果、FID 和
全表数量门禁，没有发现重开泄漏或结果漂移。本结果不改变 10M fresh-open 某些覆盖档慢于 GDAL 的
已知性能结论，也不构成共享单个 QueryEngine 实例的线程安全承诺。

原始 schema v2 JSON/CSV 位于本地忽略目录
`benchmark-results/reader-long-steady-2026-07-15/`，不提交仓库；本文保留可审核摘要。
