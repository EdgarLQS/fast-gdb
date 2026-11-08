# SPX 源目录同代缓存

状态：实际源回调、完整证明及有界缓存已接通；普通/融合 QueryEngine 尚未接入，不代表现有产品查询已解决有效缺项/错键。

## 实现与边界

Catalog 每次 `scan()` 新建 `SpatialSourceCache`；复制 Catalog 共享当前快照缓存，任一副本重新扫描不影响旧快照。即使文件身份元数据完全相同，新扫描也不会命中前代证明。实际 Reader 的 refresh 到普通/融合查询的完整验收仍待下一步接入。

缓存键包含 SPX/table/tablx 文件身份、Geometry 字段位置和字段总数、逻辑槽位/活动记录数、完整几何类型及 XY/Z/M 原点与比例。实际表键工厂固定这些值，非有限变换和与输入计数不符的键不进入缓存。`source_stable` 仍由调用方绑定真实源的身份；缓存不独立信任任意伪造回调。

只缓存完整且前后稳定的成功证明。任何构建失败（包括损坏、预算、回调失败、源变化）均可在同一快照重试，不将临时失败永久记成无效目录。冷建串行并二次查缓存，并发读者复用同一成功证明；命中时仅核对身份，不再调用借用的批量源解码/全扫回调。候选读取仍由不可变对象核对固定句柄和路径身份，不保留表、回调或 mmap 字段视图。

LRU 最多 8 个条目、16 MiB 已核算保留载荷；调用方可进一步降低字节限额，0 表示不保留缓存。超出缓存预算时已完成的证明仍可供当前调用使用。字节核算包含目录、索引/解析器对象、两个路径字符串容量及键/条目载荷；不将 allocator、哈希桶、共享指针控制块、构建暂存或映射中的源表计为整体 RSS。淘汰不使已有读者持有的证明失效，活跃外部引用不属于缓存保留上限。

源适配器只使用 pread 与解析元数据，现已立即释放原始 SPX 解析器的 mmap 和 16 个页缓冲，缓存不再保留这些多余资源。每个保留证明仍拥有只读文件句柄。不存在活动外部 Writer 并发改写安全保证；文件身份依赖设备/inode/大小/mtime/ctime，不能保证检测元数据完全相同的改写，此时需关闭 Writer 后 refresh/新快照。

## 验证

新增 8 项缓存合同：暖证明不重读源回调；table/tablx/schema/transform/计数变化不误命中；临时失败及源变化不污染快照；非有限解释拒绝；构建中变化不发布；LRU/字节限额淘汰及已有引用有效；8 个并发冷读仅完整扫描一次；Catalog 复制及身份不变的 rescan 建新证明。

固定 GDAL-free 夹具验证实际表键中的字段、类型及七个变换参数。GDAL 创建并重新只读打开的 Point、MultiPoint、LineString、带孔 Polygon、Point ZM、LineString ZM 六层，现均经 Catalog 缓存冷建和暖命中，再查询源包络候选。既有 NULL/Empty、坏行和未知几何合同保留。全部源目录/适配器/表/缓存及原生图层定向测试在 macOS、Linux 各 23/23 通过。

测试发现 Linux 快速等长改写不一定推进时间戳；身份变化测试显式推进 mtime，独立的 rescan 测试保持元数据不变，分别验证两种合同，不以测试时钟运气证明文件变化检测。

最终 Release 全套均配置用户 testcurve.gdb、固定 acceptance_metadata.gdb 及 9.2 历史参考库：macOS GDAL 3.13.0 共 636 项，618 PASS / 18 SKIP / 0 FAIL，78.20 秒；Linux ARM64 GDAL 3.10.3 共 677 项，658 PASS / 19 SKIP / 0 FAIL，32.52 秒。见 [macOS](2026-11-08-spx-source-cache/macos-release.log)、[Linux](2026-11-08-spx-source-cache/linux-release.log)；定向日志见 [macOS 23/23](2026-11-08-spx-source-cache/macos-focused.log)、[Linux 23/23](2026-11-08-spx-source-cache/linux-focused.log)。

9.2 库的旧属性索引布局按既有边界跳过属性索引对照；另以现代 testopenfilegdb.gdb 补跑全部 6 项历史对照，两平台各 5 PASS / 1 SKIP / 0 FAIL，属性索引对照实际执行；跳过的是该库没有未知布局索引。见 [macOS 现代参考](2026-11-08-spx-source-cache/macos-modern.log)、[Linux 现代参考](2026-11-08-spx-source-cache/linux-modern.log)。原 9.2 全套的未知索引边界测试已执行，不能将现代库的通过扩大为 9.2 旧索引可解析。

Linux GDAL-free Debug ASan/UBSan/LSan 全套 466 项，461 PASS / 5 SKIP / 0 FAIL，17.15 秒；开启 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`，新增缓存合同和固定表键测试实际执行，无 sanitizer 诊断。原生 GDAL 六图层测试不属于该配置。见 [构建](2026-11-08-spx-source-cache/asan-build.log)、[测试](2026-11-08-spx-source-cache/asan-ctest.log)。本次没有 QueryEngine 行为、性能、TSan、真实 Windows 或安装 consumer 的新验收结论。

## 后续

按[源目录方案](../25_SPX源几何验证目录实施方案.md)接入普通及融合空间查询；有效错键与缺项临时库必须逐窗口对照独立源全扫，分别验证成功恢复与完整回退。补齐缓存暖命中、外部修改关闭后 refresh、源代变化、预算/曲线回退的端到端合同。曲线安全源包络、真实非空 MultiPatch 与 ArcGIS 原生多级索引仍未验收。
