# 2026-11-07–08 字符串索引验证目录与范围查询

## 实现与正确性依据

字符串物理键可能被填充、截断或折叠，不能直接据其顺序裁剪原值范围。本次保留独立 ATX 的物理键接口，为带表元数据的 QueryEngine 增加以下路径：

1. 每个目录快照首次使用字符串索引时，遍历链表叶页，核对索引键与实际非 NULL 字符串。验证 ObjectID 范围、唯一性、读取成功及非 NULL 总数，防止重复、遗漏和键值不符造成漏结果。接受已验证的 UTF-16 或低 16 位折叠编码；不靠推测排序启用快路径。
2. 保存每个叶页真实 UTF-8 原值的上下界前缀（最多 128 字节），按下界排序，建立前缀最大上界目录。截断上界按整个前缀区间处理，不能按完整值比较。六种比较均可保守选择叶页；最终仍读取字段原值复核。
3. 目录绑定 ATX、gdbtable、gdbtablx 的 device/inode/size/mtime/ctime（含可用精度）及字段位置。验证前后重新核对身份；变化时不发布缓存或部分候选。同一 Catalog 快照的独立 Engine 共享不可变目录；重新 scan 会建立新缓存。
4. LRU 最多 8 项、目录计费载荷最多 16 MiB。首次验证最多为 16M 逻辑行槽分配 2 MiB bitmap。每个快照一次只允许一个构建者，热查询不持构建锁。超预算或验证失败缓存负结果，并回退字段扫描。活跃查询持有的共享目录、输出候选、缓存键/容器管理开销不包含在载荷上限内。
5. 新增缓存命中、目录字节和验证行数指标。普通字符串查询和空间 WHERE 在读取前后检查表与 tablx 身份；检测到变化返回 SourceChanged，要求刷新或完整重开。

目录内存计费在冻结时计算一次，热查询指标和 LRU 计费不再遍历全部叶页。

首次验证需要索引条目的字段读取，再扫描表统计非 NULL 值，约两遍字段访问；它不是每次冷进程都能避免的成本。选择性差、长公共前缀和宽范围谓词仍可访问大量页面。

## 专项验证

`AttributeStringCacheTest` 覆盖 v1/v2 布局、随机区间/长前缀的六操作符不漏页、不同探针的缓存复用、重复/缺失/错键、空树证明、源身份变化、验证途中变更不发布、并发单次构建、槽位预算和 LRU。`SpatialWhereUnicodeTest` 使用真正 GDAL 生成的索引，逐条比较 GDAL 全量字段读取得到的独立原值期望，覆盖普通/联合查询、Unicode/空格/截断，以及外部字段更新后的旧对象拒绝和新对象读取。

补充回归明确区分两种合同：

- 文件身份确实变化时，原缓存不能命中旧证明。
- Linux 本轮观察到同一时间戳量子内的等长改写可以保留全部 stat 身份。此时不能保证旧对象自动检测；新 Catalog/refresh 必须重新验证。本轮通过。首次全套中的失败是测试错误假定该改写必定改变身份，原始失败保留在 [日志](2026-11-07-string-directory/linux-release-before-test-contract-fix.log)，修正测试前提后同时验证刷新路径与身份失效路径，未放松结果正确性断言。

这遵循总计划 S1 的外部写入关闭后 refresh/完整重开合同，不提供未知 Writer 并发写入安全保证。

## 性能与同条件对照

Linux ARM64、GDAL 3.10.3、Release。63 个不同等值探针；每个结果与 GDAL 全量字段读取的期望核对，另与原保守算法的“扫描全部索引叶页 → 读取候选字段原值复核”比较。对照使用保留的无 context 解析器路径和同一表字段扫描，不冒充旧提交的整套 public API 二进制对比，也不比较 GDAL 索引谓词速度。OS 缓存未清，数值为该机器运行记录。

| 样本 | 首次验证 ms / 页 | 缓存字节 | 63 次热查询页 / 条目 | 热查询 median / p95 / p99 ms | 保守对照 median / p95 / p99 ms |
| --- | --- | --- | --- | --- | --- |
| 100K，含长值，160 B 物理键 | 26.7996 / 4170 | 552206 | 104 / 2488 | 0.041211 / 0.057879 / 0.18051 | 29.7657 / 31.537 / 32.3026 |
| 1M，短值，26 B 物理键 | 212.377 / 7355 | 931958 | 63 / 8568 | 0.056587 / 0.073672 / 0.07738 | 69.2776 / 71.7635 / 72.5786 |

[100K 原始记录](2026-11-07-string-directory/linux-100k.log)、[1M 短值记录](2026-11-07-string-directory/linux-1m-short.log)、[macOS 10K 记录](2026-11-07-string-directory/macos-focused.log)。

百万条长值样本在 **GDAL 生成索引阶段失败**，本项目查询没有运行。声明字段宽度 256 或 32 均不能改变该结果；两个失败分别保留为 [width256](2026-11-07-string-directory/linux-1m-width256-writer-limit.log)、[width32](2026-11-07-string-directory/linux-1m-width32.log)。核对 [GDAL v3.10.3 源码](https://github.com/OSGeo/gdal/blob/v3.10.3/ogr/ogrsf_frmts/openfilegdb/filegdbindex_write.cpp#L397-L443)：写入器的页面容量取决于物理键宽度，并限制四层树的最大条目数；字符串宽度由实际值确定（同文件 L1359–1422）。本例 160 B 键每页 24 项，对应上限 346200，不能用该写入器生成百万条长键索引。这项边界未算通过，短值百万条也不替代它。

复现规模测试（使用已经配置的 Linux 构建）：

```bash
FAST_GDB_STRING_BENCHMARK_ROWS=100000 \
  /tmp/fast-gdb-linux-s52/bin/gdb_tutorial_test_runner \
  --gtest_filter=SpatialWhereUnicodeTest.ValidatedDirectoryPrunesMultiPageQueriesAndRefreshesSnapshots

FAST_GDB_STRING_BENCHMARK_ROWS=1000000 FAST_GDB_STRING_BENCHMARK_SHORT_VALUES=1 \
  /tmp/fast-gdb-linux-s52/bin/gdb_tutorial_test_runner \
  --gtest_filter=SpatialWhereUnicodeTest.ValidatedDirectoryPrunesMultiPageQueriesAndRefreshesSnapshots
```

## 本次提交范围

包含字符串验证目录及快照缓存、普通/联合查询接入、源变化检查、指标与计费优化、专项和规模对照测试、原始成功/失败证据、总计划与 CLI/测试索引文档。没有新增写入能力。

## 验证状态与剩余工作

- Linux Release：631 项，612 PASS、19 SKIP、0 FAIL，22.64 s；[完整日志](2026-11-07-string-directory/linux-release.log)。
- GDAL-free ASan/UBSan/LSan：432 项，427 PASS、5 SKIP、0 FAIL，17.72 s；[完整日志](2026-11-07-string-directory/linux-asan.log)。
- macOS Release：590 项，562 PASS、28 SKIP、0 FAIL，71.92 s；[完整日志](2026-11-07-string-directory/macos-release.log)。最新专项 28/28 通过。
- S4.2 继续保留未完成：本次补齐字符串验证缓存和已测规模的安全范围路径，仍需核对非法数值键、真实 ArcGIS 大索引及长键规模边界；不将受预算保护的字段回退计为索引性能验收。
- 真实 ArcGIS 附件/高级对象/MultiPatch/v4 稀疏高位 ID/旧格式和 Windows 验收等保持总计划原状态。未修改原始 testcurve.gdb。
