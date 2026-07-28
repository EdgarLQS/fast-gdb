# ADR-008：同进程 GDAL Writer 协调与 Adaptive Reader

- **状态**：Accepted
- **日期**：2026-07-23
- **依赖**：[ADR-007：Reader-only 与 GDAL 编辑边界](ADR-007-reader-only-gdal-edit-boundary.md)
- **实现计划**：[22：同进程 GDAL 写入与 Adaptive Reader 切换计划](../planning/22_AdaptiveReader写入检测与GDAL回退计划.md)
- **当前验证基线**：macOS + GDAL 3.13.0；其它 GDAL 版本需单独验证，NULL 位图未使用填充位不属于字段 NULL 合同。

## 背景

ADR-007 已确定 fast-gdb 的正式产品边界是 Reader only。FileGDB 创建、更新、删除、
Schema 修改、索引维护和 REPACK 继续由调用方直接使用官方 GDAL/OpenFileGDB 完成。

原有 ADR-008 草案试图同时处理进程内 Writer、外部 Writer 检测、文件指纹和 GDAL
回退。该范围无法为 mmap Reader 与未知外部 Writer 的重叠访问提供确定性安全保证：
Writer 可能在检测信号出现前修改或截断文件，读取后的指纹校验也无法撤回已经发生的
无效映射访问。

因此 v1 收敛为 **同一进程、统一入口、显式协调的官方 GDAL Writer**。它不检测绕过
协调入口的外部进程，也不将同目录并发读写包装成事务或快照。

## 决策摘要

新增可选产品 target `fast_gdb::adaptive`。它只负责：

- 协调 fast Reader 租约；
- 在 Writer 打开 update Dataset 前发布 `WriterPending`；
- 等待旧 fast Reader 排空；
- 路由稳定读取与显式并发 GDAL 读取；
- 在 Writer 关闭后递增 generation 并强制重建旧 Reader 对象图。

它不打开、不暴露、不 Flush、不关闭调用方的 update Dataset，也不导出任何 GDAL
Writer 类型。

```text
Stable
  → 默认 fast-gdb
  → FastGdb + Verified

WriterPending / WriterActive
  → 默认 SourceBusy
  → 显式 GdalUnverified 策略才允许 fresh GDAL 只读
  → GdalOpenFileGDB + UnverifiedConcurrentRead

调用方确认 update Dataset 已关闭
  → generation + 1（每个 Writer 生命周期最多一次）
  → 旧 fast Reader、cursor、Schema 绑定全部过期
  → 重建后恢复 FastGdb + Verified
```

`UnverifiedConcurrentRead` 只说明读取由官方 GDAL 执行并且本次读取独占自己的只读
Dataset。它不保证结果属于完整旧版本或完整新版本，不可作为事务、快照或一致性门禁。

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
- `fast_gdb::hybrid` 的曲线回退不承担 Writer 协调；
- Adaptive target 不新增 Writer API、Writer ABI 或自研 FileGDB 写入代码；
- Adaptive 公共协调头不暴露 GDAL 类型。

ADR-007 保持不变。未采用 Adaptive 协调入口的调用方仍必须遵守：

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
  → 返回 ExternalUpdateToken

调用方 GDALOpenEx(... GDAL_OF_UPDATE ...)
  → 成功：notify_update_opened()，进入 WriterActive
  → 失败：cancel_before_update()，撤销 Pending
```

等待必须使用调用方显式超时。超时后协调器撤销本次 Pending、禁止打开 update Dataset，
并返回 `ReadersActive` 和剩余 Reader 数量。不得无限等待或后台轮询。

## 进程级协调状态

所有默认构造的 `InProcessGdbCoordinator` 共享一个进程级注册表。不同组件不能通过创建
新 coordinator 绕过同一路径的 Writer 状态。

注册表按规范化 GDB 路径维护：

```cpp
struct CoordinatedSourceState {
    bool writer_pending = false;
    bool writer_active = false;
    bool source_verified = true;
    uint64_t generation = 0;
    size_t fast_reader_count = 0;
};
```

内部还保存当前 coordination id 和“本 Writer 是否已经使 generation 失效”的标志。
路径使用 absolute、lexical normalize、可用时 weakly canonical；Windows 额外遵守平台
大小写规则。

状态约束：

- 同一路径最多一个 Pending 或 Active Writer；
- 不同 GDB 路径可以独立并行；
- Pending 发布后新的 fast Reader 获取立即失败；
- Writer 只有在 `fast_reader_count == 0` 时才能进入 Active；
- 所有 coordinator 对象观察到同一状态；
- 每个 Writer 生命周期最多递增一次 generation；
- Writer 未确认关闭时，Active 不得清除，新 Writer 不得进入；
- 只有确认关闭后才恢复 `source_verified=true`。

## Reader 租约

### fully-materialized query

稳定状态下，Adaptive Reader 获取查询级 fast 租约，使用全新 fast-gdb Reader 对象图
执行查询，完整物化结果后释放租约。Pending 可以在查询执行期间发布，但 Writer 必须
等待该租约退出，因此本次查询可以完成并作为其获取时 generation 的 `Verified` 结果
返回。

### streaming cursor

fast cursor 持有长期租约：

- 每次 `next()` 入口是安全点；
- Pending 在 cursor 空闲时发布，下一次 `next()` 关闭底层 cursor 并返回
  `ReaderExpired`；
- Pending 在当前 `next()` 内发布，当前调用可以完成，后续安全点必须过期；
- 过期时释放 mmap、文件句柄和 Reader 计数；
- cursor 不再推进也不销毁时，Writer 按超时失败，不得强行进入 Active。

Writer 关闭通知递增 generation 后，任何旧 Reader、catalog、table、cursor、tablx、
索引状态和 Adaptive Schema 绑定都必须重建，禁止局部 refresh。

## 外部更新令牌与关闭失败

协调令牌不包含 GDAL 类型。推荐调用序列：

```cpp
auto prepared = coordinator.prepare_external_update(gdb_path, timeout);
if (prepared.status != CoordinationStatus::Ok) {
    return prepared.status;
}

auto token = std::move(prepared.token);
const uint64_t coordination_id = token.coordination_id();
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

令牌语义：

- Pending 状态析构可自动撤销 Pending；
- Active 状态析构不得猜测 Dataset 已关闭，源保持 fail-closed；
- `notify_update_closed(false)` 表示 Dataset 尚未确认关闭：保持 WriterActive、保留 id、
  禁止 fast Reader 和新 Writer，并使 generation 失效一次；
- 同一令牌或保存的 coordination id 可在实际关闭后再次报告 `true`；
- 错误 id 不得恢复源；
- 重复报告 `false` 不得重复递增 generation；
- 最终报告 `true` 清除 Active，但不为同一 Writer 再次递增 generation。

## 正式读取后端

### fast-gdb backend

- 每个 query 创建新的 catalog、resolver 和 `QueryEngine`；
- 每个 cursor 独占自己的 fast Reader 对象图；
- 只在获得 fast 租约后打开源；
- 继续使用现有 fast-gdb 七类 `QueryKind` 实现。

### GDAL/OpenFileGDB backend

- 每个 query fresh `GDALOpenEx(... READONLY, OpenFileGDB)`，物化后关闭；
- 每个 cursor 独占自己的只读 `GDALDataset`，cursor 关闭时释放；
- 不复用曲线桥接层的 thread-local Dataset；
- 支持 FID、顺序扫描、BBOX、数值索引、字符串索引、WHERE 和 SpatialWhere；
- FID、字段顺序、NULL 位图和 ISO WKB 按 fast-gdb 合同物化；
- 打开失败与过滤/读取失败分别报告；
- 成功结果无条件标记 `UnverifiedConcurrentRead`。

GDAL backend 使用在 Stable 状态加载的 `AdaptiveLayerBinding`，其中包含字段合同、索引名
到字段名映射和 generation。若 generation 不匹配，GDAL 并发路径 fail closed，调用方
必须重新加载绑定并重建 session，禁止用旧 Schema 解释新代次数据。

## 并发读取策略

```cpp
enum class ConcurrentReadPolicy {
    SourceBusy,
    GdalUnverified
};
```

### 默认策略：`SourceBusy`

Pending、Active 或 fail-closed 时：

- 不调用 fast backend；
- 不调用 GDAL backend；
- 立即返回 `SourceBusy`；
- 不隐藏等待、不自动重试。

### 显式策略：`GdalUnverified`

Pending、Active 或 fail-closed 时：

- 使用 fresh 官方 GDAL 只读 Dataset；
- query 和 cursor 都不共享 Dataset；
- 成功结果始终是 `GdalOpenFileGDB + UnverifiedConcurrentRead`；
- Writer 在读取期间结束也不得升级为 `Verified`；
- stale Schema binding、GDAL 打开或读取失败必须保留 backend、consistency、generation
  和诊断。

稳定状态默认仍使用 fast-gdb。稳定数据源下可运行独立 parity 测试比较 fast 与 fresh
GDAL 的 FID、字段、NULL、WKB 和错误分类，但该测试不改变默认路由。

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

首批正式合同测试为计划文档中的 17 项：

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

附加门禁：

- 独立 coordinator 对象共享进程级状态；
- Active 令牌丢失只能凭正确 coordination id 恢复；
- 关闭失败保持 Active、阻断第二 Writer、generation 只递增一次；
- 真实 OpenFileGDB update 和 REPACK 只在 fast 租约排空后运行；
- 正式 fast/GDAL backend 的七类 QueryKind、字段、NULL 和 ISO WKB parity；
- stale Schema binding 在新 generation 中 fail closed；
- Adaptive ON 且 GDAL OFF 时配置失败；
- `fast_gdb::linear` 构建与安装依赖中不存在 GDAL；
- 三平台 required 测试无 required SKIP、崩溃、死锁或资源泄漏。

## 后果

### 正面

- 先行 Pending 屏障消除受协调 Writer 与 fast mmap 的重叠；
- 关闭失败不会误放行第二 Writer；
- 不改变 Reader-only 产品定位；
- 默认 fail-closed，显式暴露 GDAL 并发读取的不确定性；
- `linear` 继续保持纯 C++、无 GDAL 依赖；
- Writer 生命周期仍完全由调用方和官方 GDAL 控制。

### 代价

- 所有同进程 Writer 必须经过统一协调入口；
- 长 cursor 可能导致显式超时；
- 调用方必须保存 coordination id 并可靠报告 Dataset 关闭；
- `GdalUnverified` 结果不能用于强一致业务判断；
- 绕过协调入口的外部 Writer 仍属于 ADR-007 的不支持并发场景。
