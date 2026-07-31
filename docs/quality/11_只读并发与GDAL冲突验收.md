> 状态：Current
> 适用版本：v0.2.0+
> 维护入口：docs/quality/

# 只读并发与 GDAL 冲突验收

本文是 Reader-only 并发和 GDAL 边界的验收入口。它区分“多个独立 Reader 并发”与
“Reader 和未协调 Writer 重叠”两种完全不同的语义。

## 1. 支持合同

支持多个独立的 `Reader → Layer → QueryEngine → Cursor` 对象图并发读取。每个线程
必须独立打开完整对象图；同一个 `Reader`、`Layer`、`QueryEngine` 或 `Cursor` 不得
跨线程共享。一个 `QueryEngine` 同时只允许一个活动 Cursor。

同一进程的 Writer 通过 `InProcessGdbCoordinator` 协调：

```text
Stable
  → WriterPending（停止新 fast lease）
  → drain（等待已有 lease 完成）
/

  → WriterActive（fast/GDAL 默认均不调用）
  → GDAL 写入并关闭
  → generation + 1
/

  → fresh Reader / Verified
```

已经获得 lease 的完整物化 query 可以在 `WriterPending` 期间完成；结果必须记录
`writer_pending_seen`，且仍属于旧 generation。读取期间如果 generation 或 source
verification 发生变化，结果必须丢弃并返回过期诊断。

## 2. 独立 Reader 并发门禁

测试 `ReaderConcurrencyTest.IndependentReadersReturnIdenticalFeatureDigests` 使用
2、4、8 个线程，每个线程独立创建 Reader，覆盖：

- ReadByFid、顺序扫描、属性、BBOX、WHERE、SpatialWhere；
- 有限 Cursor；
- FID 顺序、字段值、NULL、几何 WKB 和结构化查询状态。

所有线程对同一图层的 digest 必须相同；不同图层按图层分别比较。该测试包含
Tablx 全局缓存竞争、冷打开、不同图层并行和反复打开关闭，但不把共享对象安全性
推导出来。

## 3. Adaptive 结果分类

| 结果 | 是否正确性证据 | 解释 |
|---|---:|---|
/

| `FastGdb + Verified` | 是 | source/generation 前后一致的稳定 fast 结果 |
/

| `SourceBusy` | 是 | 默认策略在 WriterActive/Pending 无后端调用 |
| `ReaderExpired` | 是 | 旧对象图必须丢弃并重建 |
/

| `GdalOpenFileGDB + UnverifiedConcurrentRead` | 否 | 显式 fresh GDAL 并发读取，仅作路由/诊断 |
| `old/new/mixed/error` | 否 | 未协调外部 Writer 的 characterization 分类 |
| `SKIPPED` | 否 | 平台、GDAL 版本或 sanitizer 未执行，不得写成 PASS |

`UnverifiedConcurrentRead` 永远不能升级为 `Verified`，即使 Writer 在读取期间结束，
也不能把该结果用于数据正确性对等测试。

## 4. GDAL 写后重开矩阵

本地 Adaptive GDAL fixture 已覆盖：

- `SetFeature`；
- `CreateFeature` 和新 FID；
- `DeleteFeature` 和稀疏 FID；
- `CreateField` Schema 变化；
- `DeleteField` Schema 变化；
/

- 属性索引删除/重建（驱动不支持时明确记为 `SKIPPED`）；
- 属性索引创建、空间候选和 extent 更新；
- `REPACK`。

每项均遵守：

```text
/

关闭 Reader/Cursor
  → GDALOpenEx(UPDATE)
/

  → 执行一个写操作并 Sync/Flush
/

  → 释放 Feature/SQL result set
  → GDALClose
  → 完整 Reader 重开
  → 比较 FID、字段、NULL、WKB 或查询结果
```

未协调同目录重叠读写只输出 old/new/mixed/error，不对任一观察结果断言安全、一致
/

或必然失败。跨驱动的 `DeleteField`、显式空间索引删除/重建需分别记录驱动能力。

## 5. 损坏输入

`CorruptInputConformance.*` 验证截断 `.gdbtable`、`.gdbtablx`、`.spx`、
`.gdbindexes` 以及损坏系统目录均 fail closed。`.atx` 还通过独立的 trailer 计数、
/

循环页链、零 FID 和截断测试；几何/WKB 测试验证无效编码、拓扑和截断输入。

失败路径不得发布部分结果、把损坏索引当作合法零命中或崩溃。

## 6. 本地命令

/

GDAL/Adaptive 本地门禁：

```bash
cmake -S . -B build \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_BUILD_ADAPTIVE_READER=ON \
  -DFAST_GDB_BUILD_FULL_TESTS=ON \
  -DBUILD_TESTING=ON
cmake --build build --target gdb_tutorial_test_runner \
  fast_gdb_adaptive_reader_test_runner --parallel 4
ctest --test-dir build --output-on-failure
```

纯 Reader 安装面必须另外使用 `FAST_GDB_WITH_GDAL=OFF` 构建，并运行 `linear`
package consumer。ASan/UBSan/TSan 命令见
[构建与平台矩阵](04_构建与平台矩阵.md)；工具链不支持时记录 `SKIPPED`。

## 7. 禁止误读

- 8 个独立 Reader 通过，不等于共享 QueryEngine 线程安全；
/

- GDAL 同目录重叠观测到 old/new，不等于获得一致性保证；
- Adaptive fresh GDAL 返回成功，不等于并发结果已验证；
/

- macOS + 单一 GDAL 版本通过，不等于 Linux/Windows 或其它 GDAL 版本通过；
/

- 普通测试通过，不等于 sanitizer、10M 内存/p99 或跨平台发布证据完成。
