# ADR-008：同进程 GDAL Writer 协调与 Adaptive Reader

- **状态**：Accepted
- **日期**：2026-07-23
- **依赖**：[ADR-007：Reader-only 与 GDAL 编辑边界](ADR-007-reader-only-gdal-edit-boundary.md)
- **实现计划**：[22：同进程 GDAL 写入与 Adaptive Reader 切换计划](../planning/22_AdaptiveReader写入检测与GDAL回退计划.md)

## 背景

ADR-007 已确定 fast-gdb 的正式产品边界是 Reader only。FileGDB 创建、更新、删除、
Schema 修改、索引维护和 REPACK 继续由调用方直接使用官方 GDAL/OpenFileGDB 完成。

原有 ADR-008 草案试图同时解决进程内 Writer 协调、外部 Writer 检测、文件快照校验和
fresh GDAL 回退。该范围无法为 mmap Reader 与未知外部 Writer 的重叠访问提供确定性
安全保证：Writer 可能在检测信号出现前修改或截断文件，读取后的指纹校验也无法撤回
已经发生的无效映射访问。

本 ADR 将第一版收敛为 **同一进程、统一入口、显式协调的 GDAL Writer**。它不检测
绕过协调入口的外部进程，也不把同目录并发读写提升为强一致合同。

## 决策

新增可选 Reader 编排 target `fast_gdb::adaptive`。它只协调读取租约和调用方的外部
GDAL update 生命周期，不打开、不暴露、不 Flush、不关闭 update Dataset，也不导出
任何 GDAL Writer 类型。

```text
Stable
  → 默认 fast-gdb
  → FastGdb + Verified

WriterPending / WriterActive
  → 默认 SourceBusy
  → 显式 GdalUnverified 策略才允许 fresh GDAL 只读
  → GdalOpenFileGDB + UnverifiedConcurrentRead

调用方确认 update Dataset 已关闭
  → generation + 1
  → 旧 fast Reader 对象图过期
  → 新 Reader 重建后恢复 FastGdb + Verified
```

`UnverifiedConcurrentRead` 只说明读取由官方 GDAL 执行并且资源由本次读取独占。它不
保证结果属于完整旧版本或完整新版本，不允许作为事务、快照或一致性门禁。

## 产品与构建边界

新增构建开关：

```cmake
option(FAST_GDB_BUILD_ADAPTIVE_READER
       "Build the coordinated Adaptive Reader" OFF)
```

约束：

- `FAST_GDB_BUILD_ADAPTIVE_READER=ON` 必须同时启用 `FAST_GDB_WITH_GDAL=ON`；
- 安装导出名为 `fast_gdb::adaptive`；
- `fast_gdb::linear`、其公共头和传递依赖保持无 GDAL 依赖；
- `fast_gdb::hybrid` 的既有曲线回退行为不承担 Writer 协调；
- Adaptive target 不新增 Writer API、Writer ABI 或自研 FileGDB 写入代码。

ADR-007 保持不变。未显式采用 Adaptive 协调入口的调用方仍必须遵守：

```text
close all Readers → official GDAL edit → GDALClose → reopen Readers
```

## 为什么必须有 `WriterPending`

Writer 不能在已有 fast Reader 的 mmap、fd/HANDLE、tablx、Schema 或索引状态仍存活时
打开 update Dataset。仅发布 `WriterActive` 已经太晚，因此写入准备分为两个阶段：

```text
prepare_external_update()
  → 原子发布 WriterPending
  → 阻断新的 fast Reader 租约
  → 已有 fully-materialized query 完成并释放租约
  → 已有 cursor 在下一次安全点返回 ReaderExpired
  → 等待 fast Reader 计数归零
  → 向调用方返回 ExternalUpdateToken

调用方 GDALOpenEx(... GDAL_OF_UPDATE ...)
  → 成功：notify_update_opened()，进入 WriterActive
  → 失败：cancel_before_update()，撤销 Pending
```

等待必须使用调用方显式提供的超时。超时后协调器必须撤销本次 Pending、不得允许调用方
打开 update Dataset，并返回 `ReadersActive` 及剩余 Reader 数量。

## 协调器状态

协调器按规范化 GDB 路径维护：

```cpp
struct CoordinatedSourceState {
    bool writer_pending = false;
    bool writer_active = false;
    bool source_verified = true;
    uint64_t generation = 0;
    size_t fast_reader_count = 0;
};
```

状态转换要求：

- 同一路径最多一个 Pending 或 Active Writer；
- 不同 GDB 路径可以独立并行；
- Pending 发布后新的 fast Reader 获取必须失败；
- Writer 只有在 `fast_reader_count == 0` 时才能进入 Active；
- 调用方确认 Dataset 已关闭后清除 Active 并递增 generation；
- 即使调用方报告关闭失败，也必须递增 generation，使全部旧 Reader 失效；
- 关闭失败时源保持 fail-closed，不能把后续 fast 结果标记为 Verified；
- Windows 路径规范化遵守平台大小写规则。

## Reader 租约

### fully-materialized query

稳定状态下，Adaptive Reader 获取查询级 fast 租约，执行现有 `QueryEngine`/Reader 路径，
完整物化结果后释放租约。Pending 可以在查询执行期间发布，但 Writer 必须等待该租约
退出，因此本次查询可以完成并作为其获取时 generation 的 `Verified` 结果返回。

### streaming cursor

fast cursor 持有长期租约：

- 每次 `next()` 入口是安全点；
- Pending 在 cursor 空闲时发布，下一次 `next()` 关闭底层 cursor 并返回 `ReaderExpired`；
- Pending 在当前 `next()` 内发布，当前调用可以完成，后续安全点必须过期；
- 过期时必须释放 mmap、文件句柄和 Reader 计数；
- 调用方不再推进也不销毁 cursor 时，Writer 等待按超时失败，不得强行进入 Active。

Writer 关闭通知递增 generation 后，任何持有旧 generation 的 Reader、catalog、table、
cursor、tablx 和索引状态都必须重建，禁止局部 refresh。

## 外部更新令牌

协调令牌不包含 GDAL 类型。推荐调用序列：

```cpp
auto prepared = coordinator.prepare_external_update(gdb_path, timeout);
if (prepared.status != CoordinationStatus::Ok) {
    return prepared.status;
}

auto token = std::move(prepared.token);
GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
    gdb_path.c_str(), GDAL_OF_VECTOR | GDAL_OF_UPDATE,
    open_file_gdb_only, nullptr, nullptr));

if (dataset == nullptr) {
    token.cancel_before_update();
    return OpenFailed;
}

token.notify_update_opened();
// 调用方直接使用官方 GDAL API 修改、Flush。
GDALClose(dataset);
token.notify_update_closed(true);
```

令牌在 Pending 状态析构可以自动撤销 Pending。令牌进入 Active 后，析构不得猜测外部
Dataset 已关闭；协调状态必须保持 Active/fail-closed，直到调用方通过有效令牌或保存的
协调标识显式报告关闭。

## 并发读取策略

```cpp
enum class ConcurrentReadPolicy {
    SourceBusy,
    GdalUnverified
};
```

### 默认策略：`SourceBusy`

Pending 或 Active 时：

- 不调用 fast backend；
- 不调用 GDAL backend；
- 立即返回 `SourceBusy`；
- 不隐藏等待、不自动重试。

### 显式策略：`GdalUnverified`

Pending 或 Active 时：

- 每个 query 新建并关闭自己的只读 GDALDataset；
- 每个 cursor 独占自己的只读 GDALDataset；
- 不复用 `GdalCurveBackendBridge` 的 thread-local Dataset；
- 成功结果无条件标记 `GdalOpenFileGDB + UnverifiedConcurrentRead`；
- Writer 在读取期间结束也不得升级为 `Verified`；
- GDAL 打开和读取失败必须保留后端、consistency、generation 和错误诊断。

稳定状态默认仍使用 fast-gdb。稳定数据源下可以运行独立 parity 测试比较 fast 与 fresh
GDAL 的 FID、字段、NULL、WKB 和错误分类；该测试不改变默认路由。

## 结果与错误模型

```cpp
enum class AdaptiveReadStatus {
    Ok,
    SourceBusy,
    ReaderExpired,
    FastBackendReadFailed,
    GdalOpenFailed,
    GdalReadFailed
};

enum class CoordinationStatus {
    Ok,
    ReadersActive,
    WriterAlreadyPending,
    WriterAlreadyActive,
    InvalidCoordinationToken,
    ExternalUpdateNotClosed
};
```

每个 Adaptive 结果至少记录：

```text
status
backend
consistency
generation_before
generation_after
writer_pending_seen
writer_active_seen
fast_error
gdal_error
```

非成功结果不得通过默认值伪装为 Verified。

## v1 非目标

第一版明确不包含：

- 外部进程、QGIS、ArcGIS 或绕过统一入口的 Writer 检测；
- 文件指纹、`.lock`、sidecar 或稳定窗口的强保证；
- 扫描 `GDALGetOpenDatasets()` 作为正式 Writer 检测；
- GDAL 驱动插件；
- fast-gdb 自研 FileGDB Writer；
- Writer target、Writer API、Writer ABI；
- 自研事务、回滚、恢复、索引写入或跨 GDB 事务；
- 同一 GDB 多 Writer；
- 将 `UnverifiedConcurrentRead` 作为业务正确性保证；
- 无限等待、后台监控线程或无界轮询。

外部 Writer 检测和 GDAL 插件必须在 v1 验收后另立 ADR。

## 测试与验收

首批正式测试为计划文档中的 17 项：

1. `StableSourceUsesFastVerified`；
2. `WriterPendingStopsNewFastReads`；
3. `FastCursorExpiresAtNextSafePoint`；
4. `PendingTimeoutClearsPendingAndRecovers`；
5. `UpdatePermitRequiresFastReadersDrained`；
6. `UpdateOpenFailureCancelsPending`；
7. `AbandonedActiveTokenRemainsFailClosed`；
8. `DefaultPolicyReturnsBusyWithoutCallingBackends`；
9. `ExplicitPolicyUsesFreshGdalUnverified`；
10. `UnverifiedResultIsNeverReportedVerified`；
11. `GdalFailureKeepsDiagnosticAndConsistency`；
12. `ClosedUpdateNotificationIncrementsGeneration`；
13. `OldReaderExpiresAfterClosedUpdateNotification`；
14. `PostWriteRebuildReturnsFastVerified`；
15. `AllQueryKindsMatchOnStableSource`；
16. `RepackNeverOverlapsFastMmap`；
17. `MultipleReadersSingleWriterStress`。

发布门禁还必须验证：

- `FAST_GDB_BUILD_ADAPTIVE_READER=ON` 且 GDAL OFF 时配置失败；
- `fast_gdb::linear` 的安装包和链接依赖中不存在 GDAL；
- Adaptive target 的 Pending/Active 路由、租约排空和 generation 失效通过；
- 所有并发 GDAL 成功结果均为 `UnverifiedConcurrentRead`；
- REPACK/update Dataset 从未与 fast mmap 租约重叠；
- 三平台 required 测试无 required SKIP、无崩溃、死锁或资源泄漏。

## 后果

### 正面

- 以先行 Pending 屏障消除受协调 Writer 与 fast mmap 的重叠；
- 不改变 Reader-only 产品定位；
- 默认 fail-closed，显式暴露 GDAL 并发读取的不确定性；
- 低层 `linear` Reader 保持纯 C++、无 GDAL 依赖；
- Writer 生命周期仍完全由调用方和官方 GDAL 控制。

### 代价

- 所有同进程 Writer 必须经过统一协调入口；
- 长 cursor 可能导致显式超时；
- `GdalUnverified` 结果不能用于强一致业务判断；
- 绕过协调入口的外部 Writer 仍属于 ADR-007 的不支持并发场景。
