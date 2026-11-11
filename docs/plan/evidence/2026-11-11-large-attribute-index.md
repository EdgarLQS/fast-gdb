# 真实 200 万行属性索引：冷/暖与选择性矩阵

日期：2026-11-11。

状态：本机 macOS、GDAL 3.13.0 Release 实测完成。数据集可用，已取得大索引冷/暖矩阵与"声明索引但缺 `.atx`"的 fail-closed 证据。总计划仍未完成，本文不勾选外部 ArcGIS/Windows 门禁。

## 一、数据集

路径：`test_data/gdb/dltb_999999.gdb/dltb_999999.gdb`（外层多套了一层同名目录，直接指外层会被 GDAL 判为不支持的格式）。

| 项目 | 值 |
|---|---|
| 大小 / 文件 | 1.3 GB，21 个文件（8 `.gdbtable`、8 `.gdbtablx`、1 `.atx`、1 `.gdbindexes`、1 `.horizon`、`gdb`、`timestamps`） |
| 图层 | `dltb_999999`，Multi Polygon，1,999,998 个要素 |
| 字段 | 71 个（SHAPE、OBJECTID、Int32、String、Real、DateTime） |
| 空间参考 | WGS 84 / Pseudo-Mercator |
| 空间索引 | **无 `.spx`**，空间过滤只能顺序回退 |
| 属性索引声明 | `FDO_OBJECTID`、`FDO_SHAPE`、`bsm_index`(col=bsm)、`dlbz_index`(col=dlbz) |
| 实际存在的 `.atx` | 仅 `a00000009.bsm_index.atx`：16,101,398 B，`depth=3`，`total_entries=1,999,998`，3,931 页 |

`dlbz_index` 有声明但没有文件；`bsm` 的索引条目数与要素数相等，索引可完整读出。

## 二、矩阵方法

新基准 `AttributePlanBenchmark.ConfiguredPredicate`（见 [tests/INDEX.md](../../../tests/INDEX.md)）：

- 冷路径：每次 trial 重新建 Catalog + QueryEngine 并执行首次查询，因此包含验证目录构建；
- 暖路径：一次打开后先做不计时预热（构建目录），再计时重复查询；
- 真值：GDAL 全量扫描 2M 行独立计算 FID，逐条比较；
- 对照：同一谓词经 `WHERE` 顺序入口计时；
- 每次运行 `trials=3`，取中位数；
- 计时拆分为：`open_median`（建 Catalog/Engine）、`first_query_median`（首次查询，含目录构建）、`warm_median`。

命令示例：

```sh
FAST_GDB_RUN_ATTRIBUTE_BENCHMARKS=1 \
FAST_GDB_ATTR_BENCHMARK_PATH=$PWD/test_data/gdb/dltb_999999.gdb/dltb_999999.gdb \
FAST_GDB_ATTR_BENCHMARK_LAYER=dltb_999999 \
FAST_GDB_ATTR_BENCHMARK_FIELD=bsm \
FAST_GDB_ATTR_BENCHMARK_OP='>=' FAST_GDB_ATTR_BENCHMARK_VALUE=1200000 \
FAST_GDB_ATTR_BENCHMARK_TRIALS=3 \
build-s4-release/bin/gdb_tutorial_test_runner \
  --gtest_filter='AttributePlanBenchmark.*'
```

## 三、结果（bsm，INT32，2M 条目）

| 谓词 | 命中 | 路径 | 叶页读 | open ms | 首次查询 ms | 暖查 ms | WHERE ms |
|---|---:|---|---:|---:|---:|---:|---:|
| `= 10591` | 2 | `attribute:atx` | 1/3931 | 3.1 | 907.0 | 0.10 | 427.9 |
| `>= 1629000` | 9,242（0.46%） | `attribute:atx` | 19/3931 | 3.0 | 901.8 | 1.43 | 433.7 |
| `>= 1500000` | 64,632（3.2%） | `attribute:atx` | 128/3931 | 3.1 | 912.2 | 9.82 | 430.0 |
| `>= 1200000` | 504,936（25%） | `attribute:sequential` | 0 | 3.5 | 1,000.4 | 431.5 | 437.2 |
| `>= 800000` | 1,510,660（75%） | `attribute:sequential` | 0 | 3.3 | 1,155.0 | 434.3 | 443.2 |
| `>= 0` | 1,999,998（100%） | `attribute:sequential` | 0 | 3.3 | 1,209.0 | 427.4 | 429.9 |

原始日志：[bsm_matrix.log](2026-11-11-large-attribute-index/bsm_matrix.log)。6/6 用例的 FID 均与 GDAL 全扫真值完全相等。

观察：

1. **打开本身很便宜**：`open_median` 约 3 ms，说明后续耗时不是打开目录造成的。
2. **首次查询固定要 0.90–1.21 s**，与选择性无关：等值只命中 2 行也要 907 ms。这段时间用于构建验证目录——读完 3,931 页索引并校验 2,000,000 行源值。
3. **首次查询比全表扫描慢约 2.1–2.8 倍**：同一谓词的 `WHERE` 顺序扫描是 428–443 ms。即"只用一次索引"是亏的，索引只有在同一会话内重复查询时才划算。
4. **暖查走目录很快，且随命中叶页线性增长**：等值 1 页 0.10 ms，3.2% 命中 128 页 9.82 ms。
5. **非选择性切换按设计生效**：25% 起 `validated_directory_scan_preferred=true`，路径改为 `attribute:sequential`，原因写明 `nonselective`，不再物化几十万到两百万 FID；暖查与 `WHERE` 入口基本一致（431 vs 437 ms）。
6. 目录没有超出预算：`budget_exceeded=false`、`directory=hit`、暖查 `validation_rows=0`。瓶颈不是 16 MiB 预算，而是构建时的全量校验。

## 四、声明索引但缺 `.atx`

`dlbz`（STRING，实际取值如 `K`，另有大量 NULL）声明了 `dlbz_index`，磁盘上没有对应 `.atx`：

| 项目 | 结果 |
|---|---|
| 绑定 | `atx=missing index=dlbz_index` |
| 路径 / 原因 | `attribute:sequential` / `attribute index missing` |
| 命中 | 133,346，等于 GDAL 全扫真值（NULL 行未被误判） |
| 冷 / 暖 / WHERE | 3.7 / 486.9（open / 首次查询）· 461.0 · 432.2 ms |

日志：[dlbz_missing_atx.log](2026-11-11-large-attribute-index/dlbz_missing_atx.log)。即"声明存在但文件缺失"不会报错或漏行，而是完整顺序回退；该行为符合 fail-closed 合同。

## 五、边界

1. OS 文件缓存未清；冷路径只表示"重建 Reader 图并首次查询"，不是物理冷 I/O。
2. 只测了 INT32 与 STRING 两个字段；Float/Date/Int64 与多字段组合未测。
3. 该库没有 `.spx`，本文不涉及空间索引；空间过滤会完整回退。
4. 采样为单一机器、`trials=3` 的中位数；不是通用性能承诺。
5. 数据集是否由 ArcGIS 原生创建未独立确认；不据此宣布 ArcGIS 大索引验收通过。

## 六、下一步

1. 针对第 3 点做产品优化：降低大索引首次查询的全量校验成本（例如用分隔键/叶范围证明覆盖，而不是逐页逐行验证），并以本库复测首次查询。
2. 用同一库补 Float/Date/Int64 与字符串大索引（如提供 `dlbz_index.atx`）的矩阵。
3. 补 `.spx` 或另一份带空间索引的同类库，验证大表空间路径。
