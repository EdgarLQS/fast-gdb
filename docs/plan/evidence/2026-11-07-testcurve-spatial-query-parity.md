# testcurve 空间窗口查询与 `.spx` 回退对照

## 样本与方法

- 只读使用 `/Users/edgarlqs/Downloads/testcurve.gdb`；原始样本未修改。为隔离空间查询语义，测试临时设置 `OPENFILEGDB_USE_SPATIAL_INDEX=NO`，让 GDAL 通过其几何过滤回退路径生成参考 FID。
- 默认 GDAL `.spx` 对照最初有 33 个窗口结果不同。关闭 GDAL `.spx` 后，剩余 15 个差异全部集中在 3 个 CRS 点图层；另用移除 `.spx` 的临时副本确认，FastGDB 和 GDAL 的回退路径结果一致。
- GDAL OpenFileGDB 在初始化空间索引时验证图层 extent 中心能否映射到 level-0 31 位网格编号；编号超出 `[0, INT_MAX]` 时拒绝使用 `.spx`。FastGDB 原先仍查询该索引，并把越界坐标夹到边界，造成 CRS 图层候选集为空。参考：[GDAL 空间索引初始化与坐标范围检查](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/filegdbindex.cpp#L2304-L2383)、[GDAL 图层初始化的空间索引回退](https://github.com/OSGeo/gdal/blob/master/ogr/ogrsf_frmts/openfilegdb/ogropenfilegdblayer.cpp#L408-L429)。
- fast-gdb 现在会在网格元数据无效或图层 extent 中心不可表示时跳过 `.spx`，执行顺序空间过滤并报告降级诊断；SpatialWhere 的融合候选路径使用相同安全门槛。
- 增加 31 位 cell 范围单测，并在 3 个 CRS 图层上验证 SpatialWhere 回退结果。

## 结果

- 26 个可比图层、130 个空间窗口全部与 GDAL 的索引关闭参考结果一致；共核对 2,273,088 个命中 FID。
- 跳过 27 条曲线记录、2 个已单独分类的坏拓扑图层，以及 5 个 GDAL 无法提供 extent 的图层。
- 3 个 CRS 图层均使用带诊断的顺序回退；3 个图层的空间加属性联合查询也与 GDAL 过滤结果一致。
- 定向单测与真实数据合同测试 2/2 通过；GDAL 3.13.0 Release 全量 CTest 为 533 项，521 通过、12 跳过、0 失败。
- 对照边界：此记录验证几何空间筛选语义，不宣称与该样本 GDAL 默认 `.spx` 的候选结果逐项一致；默认 GDAL `.spx` 与索引关闭回退在本样本上也存在结果差异。

## 验证命令

```sh
cmake --build build-s4-release --target gdb_tutorial_test_runner -j 6
FAST_GDB_CURVE_DATASET=/Users/edgarlqs/Downloads/testcurve.gdb \
  build-s4-release/bin/gdb_tutorial_test_runner \
  --gtest_filter='SpatialIndexTest.RejectsLayerExtentsOutsideLevelZeroCellRange:RealDataReleaseContractTest.SuppliedGdbSpatialWindowsReturnSameFidsAsGdal'
FAST_GDB_CURVE_DATASET=/Users/edgarlqs/Downloads/testcurve.gdb \
FAST_GDB_REAL_DATASET=/Users/edgarlqs/Downloads/fast-gdb/test_data/gdb/acceptance_metadata.gdb \
  ctest --test-dir build-s4-release --output-on-failure
```
