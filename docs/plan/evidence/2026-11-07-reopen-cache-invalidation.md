# 直接重开时的 tablx 缓存失效

基线：`fa5ac83`。环境：Debian 13.4 ARM64 / GCC 14.2 / GDAL 3.10.3 / Release，Colima 容器。

## 问题与实现

文件系统可在同一时间戳量子内改写 `.gdbtablx`，同时保持缓存键中的文件身份、大小、mtime 和 ctime 不变。仅新建 Parser 会命中此前的偏移数组，导致 GDAL 已创建第六条记录而新 Reader 仍返回五条。

每次 `GdbCatalog::scan()` 建立新目录快照后，逐个失效当前目录 `.gdbtablx` 的当前身份键。Reader、Unified 和 Adaptive 的目录重开路径共用此入口。缓存淘汰不修改已被 Parser 持有的不可变数组；外部写入前仍须关闭全部旧 Reader/Parser，未协调重叠读写仍不支持。直接使用低层 Parser、未重新扫描目录时，仍应由调用方管理缓存失效。

代价：完整新目录扫描不再复用上一快照的 tablx 解析缓存；同一目录快照内的重复 Parser 仍可共享缓存。

## 验证

- 公开 `Reader::open()` 回归在 GDAL 新增记录后，将旧偏移数组绑定到当前文件身份键，确定性模拟 metadata 未变化的缓存命中。修复前 5/6 条断言失败，修复后通过；预期新记录与 FID 由独立 OpenFileGDB 重开读取确认。
- 写入前所有旧 Reader/Parser 关闭，仅保留无文件句柄的不可变缓存值。
- 创建、删除、索引/extent 更新合同移除 20 ms 等待，各重复 30 次通过。
- 当前源码完整构建通过；配置 acceptance_metadata 与只读用户 testcurve 后，完整 CTest 590 项：573 PASS、17 SKIP、0 FAIL，29.18 秒。附件、AWS、opt-in benchmark 与驱动不支持项仍跳过。
- 人工审查确认：失效只针对当前目录可见 tablx 身份键；缓存访问由原锁保护；未改变旧数组内容或 Reader 写入协调合同。

## 性能观测

启用 `FAST_GDB_READER_CONTENTION_BENCHMARK=1`，真实 testcurve 三个 CRS 图层，1/2/4/8 线程、每线程 25 次新 Reader 打开和首活动 FID 读取；所有 digest 一致。完整 CSV 见 [Linux 数据](2026-11-07-reopen-cache-reader-contention-linux.csv)。8 线程单热点/三图层集合吞吐分别 31.820/32.297 次每秒，p99 295.360/266.970 ms。

本次 Linux 结果没有同环境优化前基线，不能与此前 macOS 数值直接计算回归比例；未清 OS 缓存，也不代表物理冷启动。需要复用同一 Reader 快照以避免反复新开成本；后续性能优化必须保持完整重开可见外部更新的合同。
