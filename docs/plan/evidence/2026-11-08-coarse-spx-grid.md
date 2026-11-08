# SPX 粗网格偏移与键顺序修复

继续修复已复现的多级空间索引漏查。总计划仍未完成，本次不是任意 SPX 源覆盖完整性证明。

## 根因与修改

对照 [GDAL 3.10.3 GetScaledCoord](https://github.com/OSGeo/gdal/blob/v3.10.3/ogr/ogrsf_frmts/openfilegdb/filegdbindex.cpp#L2366) 和 [CreateSpatialIndex](https://github.com/OSGeo/gdal/blob/v3.10.3/ogr/ogrsf_frmts/openfilegdb/filegdbindex_write.cpp#L779)：偏移应参与分辨率缩放，不能在坐标缩放完成后再加固定偏移。

旧实现把粗层 cell 近似为 `floor(floor(coord/grid0)/ratio) + 2^29`；现在使用 `floor((coord/grid0 + 2^29)/ratio)`，用 long double 计算中间量，保持现有 31 位钳位及保守上界邻格。前次 `[1,4,16]`、level 2、原点键的零候选探针现在命中 row slot 0。

同时发现混合 level 的排序问题：GDAL 使用有符号 int64 网格键排序，level 2 的最高位使其排在 level 0/1 前。叶页二分、结果范围比较和内部页导航现在统一用符号位翻转后的无符号值比较。有内部页时按完整键定位范围，而不是仅按 cell_x 比较；范围上界保守访问可能包含边界重复键的子页。

## 测试设计

新增 `SpatialIndexTest.CoarseGridOffsetAndSignedLevelsKeepPointCandidates`，独立按 Writer 的 `coord/grid[level] + shift/ratio` 公式编码、按 signed 顺序排列：v1/v2、单叶/双子页、整数与非整数比值 `[1,4,16]` / `[1,4,11.2]`、合并/非合并范围，以及负坐标、零、边界前后等 6 个坐标，共 96 组配置。每组明确检查三个 level 的 FID 候选和遍历成功状态。修复前失败见 [日志](2026-11-08-coarse-spx-grid/before.log)。

这些是明确合成的页，不能称为 ArcGIS 原生多级索引验收。CLI 回归另增加粗层单键探针；真实 GDB 全套回归检查现有样本没有倒退，但不据此宣称已覆盖所有实际多级索引分布。

## 验证

macOS 定向 31/31 通过，见[日志](2026-11-08-coarse-spx-grid/macos-focused.log)。macOS/Linux 真实 CLI 进程各 83 项通过，固定 GDB 111 个文件哈希不变，见 [macOS CLI](2026-11-08-coarse-spx-grid/macos-cli.log)及 [Linux CLI](2026-11-08-coarse-spx-grid/linux-cli.log)。

最终 macOS GDAL 3.13.0 完整回归 612 项：594 PASS / 18 SKIP / 0 FAIL，76.19 秒；Linux ARM64 GDAL 3.10.3 完整回归 653 项：634 PASS / 19 SKIP / 0 FAIL，32.66 秒，均配置用户曲线、固定元数据和历史参考库。见 [macOS](2026-11-08-coarse-spx-grid/macos-release.log)及 [Linux](2026-11-08-coarse-spx-grid/linux-release.log)。

Linux GDAL-free Debug 单线程重建当前源码，ASan/UBSan/LSan 完整回归 443 项：438 PASS / 5 SKIP / 0 FAIL，16.75 秒；启用 `detect_leaks=1:halt_on_error=1` 和 `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，无 sanitizer 诊断。新粗网格单元测试实际执行通过。见 [构建](2026-11-08-coarse-spx-grid/asan-build.log)及 [测试](2026-11-08-coarse-spx-grid/asan-ctest.log)。跳过项及 GDAL OFF 不覆盖的集成测试保持明确区分，没有重跑 TSan 或安装 consumer。

## 未完成事项

真实多级 ArcGIS SPX 的逐网格/逐窗口对照、不同图层投影的 Y 限制与 Writer 行为、所有数值临界取整、有效但缺项/错误 FID/错键、源几何与网格覆盖完整性仍待验证。单次 bbox 能看到某个 FID 不能替代任意查询窗口无漏查证明。Windows 和总计划其他真实语料门禁不变。
