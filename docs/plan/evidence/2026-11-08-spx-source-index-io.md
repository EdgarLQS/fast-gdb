# SPX 源目录有界 I/O 与身份适配层

状态：接通真实 SPX 文件读取与回调输入，还未接入实际 GDB 规范几何解码、Catalog 缓存和 QueryEngine。总计划仍未完成，不能宣称现有查询已解决有效缺项/错键。

## 本次实现

私有 `SpatialSourceIndex` 在构建前捕获路径身份，重新打开并解析 SPX，核对已打开句柄与路径身份一致。它拥有独立只读句柄；构建及后续候选读取使用有界 pread，而不直接解引用 mmap 数据。按最左路径进入叶链，检查页范围、循环、页计数、FID 范围和回调结果数量/顺序，交给已有核心验证覆盖。

源回调提供同代规范包络和完整源扫描；前后必须 `source_stable()` 为 true。只有完整覆盖成功、SPX 句柄/路径身份仍一致、源快照仍稳定才发布只读对象；回调不被缓存或保留，避免持有调用方 Reader 的借用生命周期。

候选查询按真实源包络选页，重新读取并核对叶页计数，去重和排序 FID。错误网格键和错误分隔键不参与该目录的范围选择。查询前后核对索引及源稳定状态，任何失败保留调用方原有输出，不发布部分候选。源为空间谓词无法求值的几何时，未来接入必须失败/完整回退，不能伪装成 NULL 包络。

候选元素载荷最多 16 MiB，可由调用方进一步降低数量预算；超过预算返回失败，未来 QueryEngine 应完整扫描。去重位图受已有 16M 逻辑槽位上限约束。输出 vector 容量、allocator、映射页、源回调缓冲等不是整体 RSS 上限，目录全局 LRU 尚未实现。

## 测试与前提

新增 5 项适配测试，与核心合计 13 项定向通过：v1/v2 的真实文件 I/O；物理键/分隔键故意错误而源包络仍能覆盖内部窗口；有效计数下缺项和重复 FID 不能发布；越界和循环拒绝；短回调/短扫描/异常回调及源代变化拒绝；候选预算和前后身份失败保持调用方输出；源扫描期间 SPX 文件改变使证明失败。

这些源包络由合成回调提供，不是真实 GDB 解码验收。输入回调必须绑定同一规范源解释与变换；适配层无法识别回调伪造的包络。文件身份依赖当前设备/inode/大小/mtime/ctime，不能保证捕获元数据完全相同的改写，refresh/新快照失效仍必须在缓存接入落实。未知活动 Writer 不属于支持合同。

定向日志见[13 项结果](2026-11-08-spx-source-index-io/macos-focused.log)。最终完整回归均配置用户曲线、固定元数据和历史参考库：macOS GDAL 3.13.0 共 626 项，608 PASS / 18 SKIP / 0 FAIL，81.82 秒；Linux ARM64 GDAL 3.10.3 共 667 项，648 PASS / 19 SKIP / 0 FAIL，32.85 秒。见 [macOS](2026-11-08-spx-source-index-io/macos-release.log)及 [Linux](2026-11-08-spx-source-index-io/linux-release.log)。

Linux GDAL-free Debug ASan/UBSan/LSan 完整回归 457 项：452 PASS / 5 SKIP / 0 FAIL，17.25 秒。单线程构建，开启 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，无 sanitizer 诊断，新 I/O 测试实际执行。见 [构建](2026-11-08-spx-source-index-io/asan-build.log)及[测试](2026-11-08-spx-source-index-io/asan-ctest.log)。没有性能、TSan、安装 consumer 或 QueryEngine 行为变化结论。

后续接入依照[实施方案](../25_SPX源几何验证目录实施方案.md)，先补实际源包络读取和明确的 NULL/Empty/Unsupported 分类，再接普通/融合查询及有界同代缓存，保持最终精确谓词复核。
