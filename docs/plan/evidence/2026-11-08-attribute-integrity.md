# 2026-11-08：属性索引完整性与源行失败处理

状态：本次实现已完成；总计划和 S4.2 保持未完成。提交以包含本文的 Git 提交为准。

## 本次提交内容

- 将字符串验证目录推广为数值、Int64、GUID、日期共享的属性值目录。首次核对每个键与源字段、FID 唯一性、有效范围及非 NULL 总数，不能只复核已命中的候选。
- 验证成功后按叶页原值范围选择页面；数值有序页内二分，无序页仍逐键比较，最后复核真实字段。错误键、重复/缺失 FID、超预算或坏索引回退完整字段扫描。
- 新增 `QueryEngine::query_attribute_int64()`，CLI 绑定 Int64 字段时使用该入口；整数上下限与 2^53 相邻值不经过 double。普通 double 入口保持有限输入要求。
- 浮点排序保留无穷及正负零语义，拒绝 NaN；GUID 统一规范值。原始 OLE 日数和 typed 日期毫秒分别缓存，避免不同语义共用目录。TimestampOffset 的原始数值入口比较存储日数组件；UTC 语义继续使用 typed WHERE 和既有回退。
- 修复坏源行被当成 NULL 或部分字段发布的问题。几何扫描区分物理读取失败与合法 NULL；属性、顺序、WHERE、空间及联合查询无法完整读取时返回 Unsupported 并清空部分结果。可完整回退的失效空间候选先扫描全部活动行。主动 limit 和取消不误判为损坏。
- 更新 CLI、Reader 流程、测试索引及总计划；原始用户 GDB 保持只读。

## 可复现的问题与测试范围

[修复前 CLI](2026-11-08-attribute-integrity/before-fix.log) 使用 GDAL 创建真实 Int32/Float64 源值 5、10、15，再将索引中 10 的键改成有限值 5；旧入口成功返回零条匹配，漏掉真实的 10。[修复后](2026-11-08-attribute-integrity/after-fix-cli.log) 回退扫描并返回完整结果。日志中的 `Matched FIDs: 0` 是匹配数量为零。

[坏行修复前](2026-11-08-attribute-integrity/unreadable-row-before-fix.log) 证明坏物理行曾导致成功但不完整的空间/联合结果；新增 GDAL fixture 和无 GDAL 损坏输入测试验证错误传播、合法 NULL、调用方限量及一次性取消信号。

新增测试包括：v1/v2 页、无序叶页、精确 Int64/IEEE 编码、比较域隔离；有限错键、重复 FID、缺失项、越界 FID；GUID 损坏回退；真实 GDAL Int64 字段上下限和六运算符对照；数值冷热规模测试。GUID 损坏 fixture 的 ATX 明确为合成，不能当作 ArcGIS 原生索引验收。Int64 字段也不能替代高位 ObjectID64 映射验收。

## 资源与性能

目录为 Catalog 同代快照共享，所有类型合计最多 8 项、16 MiB 计费载荷；验证 FID 位图在最多 16M 逻辑槽位下不超过 2 MiB。目录计费不包括输出 FID、mmap、表偏移、管理对象和已淘汰但仍被在途查询持有的目录。每页临时键数组由页容量限制；验证失败会缓存负结果。

冷查询为 O(N) 验证，包含按键读源字段和完整非 NULL 计数，不能视为低成本首次查询。Linux ARM64、GDAL 3.10.3，63 个不同等值探针，完整源字段读取作为独立真值：

| 行数 | 冷查询 ms | 验证字段访问 | 目录字节 | 暖查询 median/p95/p99 ms | 原始分页基线 median/p95/p99 ms |
|---|---:|---:|---:|---|---|
| 1M | 113.938 | 1,989,690 | 367,850 | .026501 / .030419 / .043419 | .013209 / .016417 / .728125 |
| 10M | 1111.58 | 19,896,907 | 3,676,190 | .025918 / .028418 / .045669 | .014334 / .016209 / .018376 |

暖查询合计各读取 63 叶页、1136/1138 次键比较访问（包含二分重复探测，不是唯一键数）。基线为原始 B-tree 查询加源字段复核，缺少完整性验证和公开入口全部身份检查，不是旧版本公开 API 的整体耗时。暖查询中位数更慢，不能宣称全面提速。原始记录：[1M](2026-11-08-attribute-integrity/linux-1m-numeric.log)、[10M](2026-11-08-attribute-integrity/linux-10m-numeric.log)。计时早于最后坏行失败处理补丁；该补丁不改变正常索引的选页算法。

目录在 Catalog.scan 后重置。外部 Writer 关闭后应 Dataset.refresh，或重建 Catalog、重新解析图层并打开引擎；只在旧 Catalog 上重开引擎不能代替完整刷新。大小/时间等元数据完全相同的原地改写不能保证自动发现。未保证与活动 Writer 并发 mmap 读取安全。

## 最终验证

- macOS、GDAL 3.13.0 Release：606 项，588 PASS / 18 SKIP / 0 FAIL，75.68 秒。[完整日志](2026-11-08-attribute-integrity/macos-release.log)
- Linux ARM64、GDAL 3.10.3 Release：647 项，628 PASS / 19 SKIP / 0 FAIL，33.63 秒。[完整日志](2026-11-08-attribute-integrity/linux-release.log)
- Linux GDAL-free ASan/UBSan/LSan：439 项，434 PASS / 5 SKIP / 0 FAIL，17.65 秒。[完整日志](2026-11-08-attribute-integrity/linux-asan.log)
- 最终构建重新安装后，macOS linear/hybrid/unified 外部 Consumer 3/3、Linux linear/hybrid/adaptive/unified 4/4 通过，包含精确 Int64 新符号链接检查。[macOS](2026-11-08-attribute-integrity/macos-install-consumers.log)、[Linux](2026-11-08-attribute-integrity/linux-install-consumers.log)

macOS/Linux 均配置用户曲线样本、固定元数据样本和历史参考库。跳过项保留原因，包括真实附件、云服务、未显式启用的基准和特定后端合同；没有将 SKIP 记为验收。Linux 虚拟机约 1.9 GiB、无 swap，同时编译两个配置曾 OOM；改为串行单线程 sanitizer 构建后继续，见[资源记录](2026-11-08-attribute-integrity/build-resource-note.log)。

## 已提交工作之外的剩余项

- S2.5/S2.6：ArcGIS 真实附件 payload 和高级对象。
- S3.3/S3.4：非空真实 MultiPatch；v4 高位稀疏 ObjectID、删除及索引映射独立真值。
- S3.5：v9 gdbindexes、SDC/CDF 和配置矩阵；真实 ArcGIS DateOnly/TimeOnly/TimestampOffset ATX。
- S4.2：真实 ArcGIS 大型索引、百万长字符串生成与验收；非选择性条件、宽键及冷 I/O 继续测。百万/千万普通数值不能替代这些门禁。
- S5.1：Windows 中文路径、文件替换、真实语料和发布检查。TSan 的容器 runtime 限制仍未解决，不能记通过。
- 后续代码自查：独立原始 ATX 显式类型/宽度约束、真实 B-tree 最后子页下探、超长 varuint 拒绝、空间索引候选完整性与几何语义错误传播、重复索引名绑定歧义。尚未形成修复或验收结论。

写入能力不在本次只读实现范围；继续按总计划在完整只读门禁之后单独设计。
