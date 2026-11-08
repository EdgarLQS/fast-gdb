# S4.2 低选择性数值查询计划切换

依据[宽范围实测](2026-11-08-wide-attribute-queries.md)实现普通属性查询的扫描计划选择，总计划和 S4.2 仍未完成。

## 实现与合同

- 仅普通绑定 Int16/Int32/Int64/Float32/Float64 属性入口允许切换。原始 ATX、融合查询、字符串、GUID 和日期不受该策略影响。
- 仅使用已经成功建立的同代验证目录；首次冷验证继续既有路径，不提前假定索引完整。至少 4096 个活动行才启用估算。
- 对可能命中的叶页累加条目数量作为保守上界，超过活动行数四分之一时，先复核身份稳定，再请求完整字段扫描。不生成宽候选 FID 列表，不读取这些叶页，也不把成功目录写为无效或负缓存。
- 新指标 `validated_directory_scan_preferred` 明确报告策略选择；路径为 `attribute:sequential`，原因含 `nonselective`，不伪称索引损坏。
- 扫描仍使用相同绑定字段比较函数，保留精确整数、NULL、源变化前后 guard 和无法完整读取时 Unsupported/清空部分结果合同。策略估算只影响执行方式，不能改变匹配结果。

四分之一是依据当前普通数值 fixture 的保守启发式，并非全局最优成本模型。叶页原值区间较宽或值乱序时可能高估命中，结果正确但计划未必最快。首次宽条件依然可能付出验证和候选成本；本次不宣称优化了冷查询。

## 验证

六操作符三轮完整 FID 对照继续使用独立 GDAL 全量字段真值与 WHERE 扫描；新增断言在满足规模的五种宽条件中选择 sequential、指标为 true、叶页读取为零、原因明确。63 个等值暖探针仍要求 ATX 路径与目录命中。

macOS 定向 27/27 通过。最终完整回归：macOS GDAL 3.13.0，610 项，592 PASS / 18 SKIP / 0 FAIL，79.64 秒；Linux ARM64 GDAL 3.10.3，651 项，632 PASS / 19 SKIP / 0 FAIL，33.19 秒，均配置用户曲线、固定元数据和历史参考库。见 [定向](2026-11-08-adaptive-attribute-scan/macos-focused.log)、[macOS 完整](2026-11-08-adaptive-attribute-scan/macos-release.log)、[Linux 完整](2026-11-08-adaptive-attribute-scan/linux-release.log)。

重新安装并重编译 macOS linear/hybrid/unified Consumer 3/3、Linux linear/hybrid/adaptive/unified 4/4 通过，见 [macOS](2026-11-08-adaptive-attribute-scan/macos-consumers.log)、[Linux](2026-11-08-adaptive-attribute-scan/linux-consumers.log)。新增公开指标后下游应重新编译；没有承诺二进制 ABI 不变。本次尚未重跑 sanitizer，不能沿用此前结果作为当前源码证明。

## 规模与边界

百万行三轮 median：绑定属性 `!=` 20.5032 ms（此前 58.7811），同轮 WHERE 22.1316 ms；四个半范围约 20.9–21.5 ms，同轮 WHERE 22.4–23.0 ms。等值 .044583 ms，继续走索引。见 [1M](2026-11-08-adaptive-attribute-scan/linux-1m.log)。这是暖 OS 缓存、固定执行顺序的单组比较，不是冷 I/O 或通用倍数承诺。

千万行六操作符及既有等值探针全部通过，见 [10M](2026-11-08-adaptive-attribute-scan/linux-10m.log)。`!=` median 223.109 ms（此前 695.402），同轮 WHERE 239.096 ms；四个半范围约 209–211 ms（此前约 308–312），同轮 WHERE 约 227–228 ms；等值 .053459 ms，继续走索引。日志中的 `indexed_median_ms` 表示绑定属性公开入口，此时宽条件实际已经选择 sequential。

真实 ArcGIS 大索引、百万长键、其他字段与分布的策略调优、空间索引候选完整性、Windows 和原计划真实语料门禁继续保留为未完成。
