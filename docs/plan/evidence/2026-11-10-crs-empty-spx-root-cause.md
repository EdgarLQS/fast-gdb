# 3 个 CRS 图层源证明失败根因：语料自带空 `.spx`

日期：2026-11-10。

状态：**根因已确定，属于语料属性，不是 Reader 缺陷**。样本恢复后本轮完成了实际验证；产品代码不需要修改，完整回退行为保持不变。

## 一、样本与定位

用户恢复的样本为 `test_data/gdb/testcurve.gdb`（42 MB、279 个文件、53 个 `.gdbtable`），GDAL 3.13.0 可打开，共 44 个图层，含 `CRS_WGS84_Point_FC`、`CRS_CGCS2000_Point_FC`、`CRS_Xian80_Point_FC`。

图层到物理表的对应关系由 `.gdbtable` 头部 WKT 与要素数确定：

| 图层 | 物理表 | 要素数 | 源几何 |
|---|---|---|---|
| `CRS_WGS84_Point_FC` | `a0000001d` | 1 | `POINT (1 1)`，GCS_WGS_1984 |
| `CRS_CGCS2000_Point_FC` | `a0000001e` | 1 | `POINT (1 1)`，CGCS2000 |
| `CRS_Xian80_Point_FC` | `a0000001f` | 1 | `POINT (1 1)`，Xian 1980 |

## 二、诊断输出

`RealDataReleaseContractTest.SuppliedGdbSpatialWindowsReturnSameFidsAsGdal` 对 3 个图层 × 5 个窗口全部输出同一结论：

```text
[crs-proof] <layer>/<window> path=bbox:model:sequential-fallback
  reason=spatial source proof failed: complete source scan failed:
  source coverage missing for active geometry; sequential scan used
  hit=false bytes=0 leaves=0 entries=0 pages=0
```

`leaves=0`、`entries=0`、`pages=0` 说明源证明没有读到任何叶页，而不是读了叶页后校验失败。

## 三、`.spx` 实际内容

| 文件 | 大小 | trailer | 头部页 |
|---|---|---|---|
| `a0000000b.spx`（3 要素点层，对照） | 4118 B | `depth=1, total_entries=3` | 含页级/层级元数据 |
| `a0000001d.spx` | 4118 B | `depth=1, total_entries=0` | 4096 字节整页全零 |
| `a0000001e.spx` | 4118 B | `depth=1, total_entries=0` | 4096 字节整页全零 |
| `a0000001f.spx` | 4118 B | `depth=1, total_entries=0` | 4096 字节整页全零 |

`.horizon` 三者均为 32 字节，记录 ±400/±90 的范围，说明索引对象存在但没有任何条目。表内确有 1 条活动要素，所以这是**空/未构建的索引**，不是"没有索引文件"。

## 四、不是测试写坏

[2026-11-08 SPX 语料清单](2026-11-08-spx-corpus-levels/macos-inventory.json) 记录的是同一路径 `/Users/edgarlqs/Downloads/testcurve.gdb`，当时这 3 个文件已经是 `entries=0, pages=1, depth=1`。本轮恢复的样本与当时一致，说明空索引是**原始语料内容**，不是本轮测试改写。

同一语料共有 45 个 `.spx`，其中 11 个 `total_entries=0`：

- `a00000004`：目录表，无几何要素，0 条目属正常；
- `a00000014`：0 要素，属正常；
- `a00000016/17/18/1d/1e/1f/2e/30/32`：各 1 要素，空索引。

即"单要素图层索引为空"在该语料中并非 CRS 三图层的特例。

## 五、结果一致性

- GDAL 全量扫描与 `-spat 0 0 2 2` 过滤都返回 1 条 `POINT (1 1)`；Fast 通过完整回退返回相同 FID，真实数据用例 1/1 通过。
- 因此不存在"Fast 漏结果"：源证明拒绝使用无法证明覆盖的索引，回退给出正确结果。
- 早期"extent 中心无法映射到 level-0 31 位网格编号"是旧物理网格路线的现象，本轮已不再相关。

## 六、结论与后续

1. 3 个 CRS 图层无法建立源证明的原因是**语料的 `.spx` 为空索引**，源覆盖证明按设计 fail-closed，行为正确。
2. 不修改产品代码，也不通过恢复旧网格键查询绕过证明；完整回退保留。
3. 后续若要减少这类语料上的重复证明开销，可在诊断上把"空索引"与"部分覆盖"区分开（仅是报错措辞增强），不改变回退语义；本轮未实现。
4. 真实 ArcGIS 多级索引与任意窗口覆盖验收仍需其它语料，不因本结论提前勾选。
