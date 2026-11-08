# 历史索引元数据调查与失败状态修复

总计划保持未完成，本次未实现 v9 索引元数据解析。

## 历史格式事实

已有固定 92/93 参考库各含 27 个 `.gdbindexes` 文件，全部前 16 字节为 `13988503a54dd11188240000f877762d`；逐文件大小及 SHA-256 见 [清单](2026-11-08-index-metadata-state/legacy-inventory.json)。这是序列化布局的识别事实，不能当作索引数量或字段结构。

[GDAL 3.10.3 的 GetIndexCount 源码](https://github.com/OSGeo/gdal/blob/v3.10.3/ogr/ogrsf_frmts/openfilegdb/filegdbtable.cpp#L2313) 明确未处理此 v9 结构，另有 raster block_key 特例。不能通过搜索 UTF-16 字段名、猜偏移或沿用现代 count 布局宣称完整解析。后续仍需建立各对象边界、类型标识、字符串及字段关联的可靠格式证据，并用独立真值验证。

## 本次产品修复

调查时复现现代解析器的两个失败状态问题：同一对象成功解析后，文件被移除，重读失败却保留旧条目；多条目文件后面的名称长度非法时，返回 false 却暴露前面的部分条目。见 [修复前日志](2026-11-08-index-metadata-state/before.log)。

现在每次 parse 先清空旧输入与输出，所有新条目先保存在局部 vector，只有整个解析成功才发布。失败时 entries 为空，不修改磁盘输入，也没有为未知历史格式放宽判断。

新增测试在同一解析器上反复成功读取后分别移除文件、注入后续负长度、截断后续字符串，检查失败且无旧值或部分结果。有效条目的字段表达式也明确核对，避免仅测试空文件。

## 验证

macOS 元数据定向 12/12 通过，见 [日志](2026-11-08-index-metadata-state/macos-focused.log)。最终完整回归均配置用户曲线、固定元数据及历史参考库：

- macOS GDAL 3.13.0：610 项，592 PASS / 18 SKIP / 0 FAIL，75.99 秒。[日志](2026-11-08-index-metadata-state/macos-release.log)
- Linux ARM64 GDAL 3.10.3：651 项，632 PASS / 19 SKIP / 0 FAIL，33.63 秒。[日志](2026-11-08-index-metadata-state/linux-release.log)
- Linux GDAL-free Debug ASan/UBSan/LSan：442 项，437 PASS / 5 SKIP / 0 FAIL。使用既有配置，`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，单线程重建和测试，无 sanitizer 诊断。[构建](2026-11-08-index-metadata-state/asan-build.log)、[测试](2026-11-08-index-metadata-state/asan-ctest.log)

跳过项保留在原始日志，不记作验收；GDAL-free sanitizer 不包含 GDAL 依赖集成用例。v9、ArcGIS 独立真值、SDC/CDF、Windows 和原计划其他门禁继续保留为未完成。
