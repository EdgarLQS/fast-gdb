# 3 个 CRS 图层源证明失败：诊断入口与阻塞说明

日期：2026-11-10。

状态：**根因未确定，样本缺失阻塞**。`/Users/edgarlqs/Downloads/testcurve.gdb` 当前不存在，无法读取 `CRS_WGS84_Point_FC`、`CRS_CGCS2000_Point_FC`、`CRS_Xian80_Point_FC` 的 SPX/tablx 实际内容。本轮完成了本地可做的诊断入口和合成签名复现，不宣称根因。

## 一、已确认的背景

2026-11-08 的记录显示：3 个 CRS 图层在旧物理网格路线下因"extent 中心无法映射到 level-0 31 位网格编号"而走顺序回退；源证明接入后仍无法建立证明，原因字段从物理网格范围改为 `spatial source proof failed`。两种可能必须用样本区分：

1. SPX 条目本身不完整/陈旧（源证明按设计拒绝）；
2. 条目到源行槽/FID 的映射或页链解析不匹配（可能是缺陷）；
3. 部分源行几何无法解码/包络不可证明（回退正确）。

## 二、本轮新增诊断

### 1. 真实数据测试输出完整原因

`RealDataReleaseContractTest.SuppliedGdbSpatialWindowsReturnSameFidsAsGdal` 对每个 `CRS_*` 图层/窗口输出：

```text
[crs-proof] <layer>/<window> path=<execution_path> reason=<fallback_reason>
  hit=<bool> bytes=<n> leaves=<n> entries=<n> pages=<n>
```

样本恢复后一条命令即可看到具体失败不变量，不需要先改代码。

### 2. 源目录构建失败原因细分

`SpatialSourceDirectoryBuilder` 现在保留首个失败不变量，`SpatialSourceIndex::build` 将其拼入 `reason`。新增/可区分的签名：

| 签名 | 含义 |
|---|---|
| `complete source scan failed: source coverage missing for active geometry` | 活动非空源行没有任何已验证叶页覆盖（SPX 缺项/陈旧） |
| `source leaf validation failed: indexed FID out of range` | SPX 条目 FID 超出源行槽 |
| `source leaf validation failed: duplicate leaf page` | 叶链重复/环 |
| `source leaf validation failed: indexed bounds non-finite or inverted` | 源包络非法 |
| `source coverage proof failed: index entry count differs from trailer` | 叶条目总数与 trailer 不一致 |
| `source coverage proof failed: indexed active rows differ from source` | 索引活动行集合与源扫描不一致 |
| `invalid SPX row slot` | 页内 ObjectID 越界（可能是稀疏 FID/映射问题） |
| `source bounds read failed` | 源行几何解码/包络不可证明 |

### 3. 合成复现（无 testcurve 也可运行）

`SpatialWhereIndexFallbackTest.MissingIndexEntryReportsCoverageProofFailure`
用 GDAL 生成 10,000 个点，删除一条 SPX 条目并保持页/总数自洽，断言：

- 执行路径 `bbox:model:sequential-fallback`；
- `fallback_reason` 含 `source coverage missing for active geometry`；
- 不发布目录缓存（`source_directory_bytes=0`、`hit=false`）；
- 结果仍由完整扫描给出。

实测输出：

```text
[missing-entry] path=bbox:model:sequential-fallback
reason=spatial source proof failed: complete source scan failed:
  source coverage missing for active geometry; sequential scan used
```

## 三、样本恢复后的执行步骤

```sh
FAST_GDB_CURVE_DATASET=/path/to/testcurve.gdb \
FAST_GDB_REAL_DATASET=$PWD/test_data/gdb/acceptance_metadata.gdb \
build-s4-release/bin/gdb_tutorial_test_runner \
  --gtest_filter='RealDataReleaseContractTest.SuppliedGdbSpatialWindowsReturnSameFidsAsGdal' \
  --gtest_brief=1
```

1. 记录 3 个 `CRS_*` 图层的 `[crs-proof]` 行。
2. 若为 `source coverage missing`：用 `explorgdb_cli dump-spx <spx>` 与源行扫描对比条目集合，确认 SPX 是否陈旧/缺项；预期保持完整回退，不以恢复旧网格查询绕过证明。
3. 若为 `invalid SPX row slot` 或行槽相关：检查 v1/v2/v3/v4 的 ObjectID 与 tablx 逻辑行槽映射，判断是否为 S3.4 稀疏 FID 缺口。
4. 若为 `source bounds read failed`：定位具体源行几何类型（曲线/未知结构），按曲线包络或 Unsupported 合同处理。
5. 只有确认是映射/解析缺陷时才改产品代码，并更新当前显式期望回退的真实数据断言。

## 四、本轮验证

- `MissingIndexEntryReportsCoverageProofFailure`：1/1 通过（`missing_entry.log`）。
- macOS Release 全套：650 项，0 失败（`ctest_macos3.log`）。
- GDAL-free ASan/UBSan 全套：471 项，0 失败（`ctest_sanitize3.log`）。
- 3 个 CRS 图层的真实数据用例未运行（样本缺失），保持未验收。

## 五、边界

- 本文不把合成缺项复现等同于 3 个 CRS 图层的根因。
- 没有样本时不猜测 SPX 布局或恢复旧网格键查询。
- 3 个 CRS 图层的 FID 与 GDAL 一致性在 2026-11-08 已由完整回退保证；本轮未重跑该样本。
