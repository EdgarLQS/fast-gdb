# 规范 GDB 几何源回调

状态：已接通实际表读取及规范模型解码到源目录适配器；Catalog 缓存与普通/融合 QueryEngine 尚未接入。总计划仍未完成。

## 本次实现

`make_table_spatial_source_input` 借用已打开的表，固定 Geometry 字段、几何类型的 Z/M 标记和坐标变换，构造批量读取与完整源扫描回调。批量扫描可能按物理位置重排，同一批重复 FID 只解码一次，输出再按请求 FID 对齐，重复 FID 也保持对应。

非空 blob 必须完整解码成有效规范模型；点、多点、折线和多面按实际顶点取 XY 包络，完整已知 MultiPatch 按其顶点取范围。不使用 SPX 网格键或未经模型验证的 header bbox 作为源证明。坐标非有限、变换无效、未知模型或解码失败返回失败。

只有确认的 NULL、零长度几何及明确 Empty/Null 模型返回无包络；未知或损坏内容不能变成 NULL。曲线/线性化模型暂不以采样顶点证明原曲线包络，回调拒绝该优化，后续查询接入必须保留完整扫描回退。这不意味着产品停止支持既有曲线读取。

调用方仍负责在 `source_stable` 中绑定 table/tablx/schema/transform 同代身份。回调只在目录构建期间借用表，不应被跨 Reader 生命周期或并发写入使用；目录对象不会保留这些回调。尚未实现全局缓存、generation 失效和查询计划选择。

## 实际数据验证

GDAL 创建六个真实临时图层：Point、MultiPoint、LineString、带孔 Polygon、Point ZM、LineString ZM。每层含两个非空几何、一个 NULL、一个 Empty；先关闭 Writer，更新/关闭后读取实际 SPX，另重新以 GDAL 只读打开取得包络及空值真值，并断言 Z/M 标记实际保留。

源回调的 XY 范围与 GDAL 实际读回相符；乱序/重复请求保持 FID 对应；NULL/Empty 均无包络；完整源扫描数量正确；实际 SPX 与该表回调成功建立目录，候选包含非空源记录、不包含空行。候选仍可能含额外几何，最终精确过滤尚待 QueryEngine 接入。

既有真实坏行和未知几何类型测试另断言实际源回调的批量读取、完整扫描都失败，而不是发布无包络。新增 GDAL-free 固定 wide_50 夹具对比批量与全扫包络，供 sanitizer 实际执行新解码路径。当前定向 9/9 通过，见[日志](2026-11-08-spx-table-source/macos-focused.log)。

实际非空 ArcGIS MultiPatch 与原曲线保守包络未验收，不将代码分支存在视作真值证明。本次没有扩展原始格式支持范围。

## 验证记录

最终产品实现的完整回归：macOS GDAL 3.13.0 共 628 项，610 PASS / 18 SKIP / 0 FAIL，78.19 秒；Linux ARM64 GDAL 3.10.3 共 669 项，650 PASS / 19 SKIP / 0 FAIL，33.27 秒。均配置用户曲线、固定元数据和历史参考库。见 [macOS](2026-11-08-spx-table-source/macos-release.log)与 [Linux](2026-11-08-spx-table-source/linux-release.log)。全套完成后仅补充测试中的 Z/M 读回标记断言，产品代码未再修改；最终 macOS 定向 9/9、[Linux 原生对照 1/1](2026-11-08-spx-table-source/linux-focused.log)通过。

Linux GDAL-free Debug ASan/UBSan/LSan 最终代码完整回归 458 项：453 PASS / 5 SKIP / 0 FAIL，16.74 秒。开启 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`；固定夹具的新源回调测试实际执行，无 sanitizer 诊断。GDAL 原生六图层测试不属于该 GDAL-free 配置。见 [构建](2026-11-08-spx-table-source/asan-build.log)与[测试](2026-11-08-spx-table-source/asan-ctest.log)。没有安装 consumer、性能、TSan 或普通/融合查询端到端行为变化结论。

后续按[源目录实施方案](../25_SPX源几何验证目录实施方案.md)完成缓存、实际查询接入、损坏 SPX 端到端恢复及写后 refresh 合同；Windows/ArcGIS 和特殊语料缺失的门禁保持不变。
