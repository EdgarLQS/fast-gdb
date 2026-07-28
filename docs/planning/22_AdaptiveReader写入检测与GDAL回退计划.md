# 同进程 GDAL 写入与 Adaptive Reader 切换计划

- **状态**：Active / Evidence pending
- **日期**：2026-07-23
- **当前正式合同**：[ADR-007：Reader-only 与 GDAL 编辑边界](../adr/ADR-007-reader-only-gdal-edit-boundary.md)
- **决策依据**：[ADR-008：Adaptive Reader 写入检测与 fresh GDAL 只读回退](../adr/ADR-008-adaptive-reader-write-detection-gdal-fallback.md)
- **产品边界**：fast-gdb 仍是 Reader only；所有 FileGDB 修改继续由原生 GDAL/OpenFileGDB 执行

## 1. 计划结论

第一版只处理 **同一进程、统一入口管理的 GDAL Writer**。稳定数据源使用
fast-gdb；Writer 请求开始后，新的读取停止进入 fast 路径，并按显式策略返回
`SourceBusy` 或退化为原生 GDAL 读取。

```text
稳定状态
  → fast-gdb
  → Verified

WriterPending / WriterActive
  → 默认 SourceBusy
  → 显式 GdalUnverified 策略下使用原生 GDAL
  → UnverifiedConcurrentRead

Writer 关闭
  → generation + 1
  → 旧 fast Reader 全部过期
  → 重建后恢复 fast-gdb
```

`UnverifiedConcurrentRead` 只表示“读取行为交给原生 GDAL 执行”。它不保证结果属于
完整旧版本或完整新版本，也不把同目录重叠读写提升为 fast-gdb 的支持合同。

未采用 Adaptive 协调入口时，唯一具有一致性保证的流程仍然是：

```text
关闭全部 Reader → GDAL 写入 → GDALClose → 重开 Reader
```

## 2. 为什么需要 `WriterPending`

如果 Writer 在 fast Reader 的 mmap 读取中途直接修改、重写或截断文件，Reader
可能读取跨文件混合状态，也可能因失效映射发生进程级异常。事后把结果标记为
`Unverified` 无法撤回事先暴露的数据或避免 mmap 异常。

因此 Writer 分为两个阶段：

```text
Writer 请求开始
  → 发布 WriterPending
  → 新 Reader 不再进入 fast
  → 已有 fast query 完成当前原子读取
  → 已有 fast cursor 在下一安全点返回 ReaderExpired
  → 等待 fast Reader 计数归零
  → 打开 OpenFileGDB update Dataset
  → WriterActive
```

这不是停止整个读取服务。Pending 和 Active 期间，新请求仍可在调用方显式选择
`GdalUnverified` 时通过 GDAL 读取；真正禁止重叠的只有旧 fast mmap Reader 与
Writer 修改源文件。

如果调用方不再推进或销毁旧 cursor，Writer 等待可能超时。超时必须：

- 撤销本次 Pending；
- 不打开 GDAL update Dataset；
- 返回 `ReadersActive`；
- 保留可操作诊断，包括未退出 Reader 数量和等待时间。

## 3. 范围

### 3.1 第一版包含

- 同一进程内的统一 Reader/Writer 协调；
- 单 GDB 单 Writer；
- `WriterPending`、`WriterActive` 和 generation；
- fast query 与流式 cursor 的安全租约；
- 显式 `SourceBusy` / `GdalUnverified` 策略；
- 原生 GDAL 的 FID、顺序、空间、属性、WHERE 和 SpatialWhere 读取；
- 后端、一致性、generation 和错误诊断；
- Writer 关闭后的完整 Reader 失效与重建。

### 3.2 第一版不包含

- 外部进程、QGIS 或 ArcGIS Writer 检测；
- 扫描 `GDALGetOpenDatasets()` 作为正式 Writer 检测；
- 文件指纹、`.lock` 或 sidecar 的强保证；
- fast-gdb 自研 FileGDB Writer；
- 自研事务、回滚、恢复或索引写入；
- GDAL 驱动插件；
- 跨 GDB 事务或同一 GDB 多 Writer；
- 将 `UnverifiedConcurrentRead` 结果作为正确性门禁。

外部 Writer 检测和 GDAL 插件只能在本计划完成后另立提案。

## 4. 目标接口

### 4.1 构建开关

```cmake
option(FAST_GDB_BUILD_ADAPTIVE_READER
       "Build the coordinated Adaptive Reader" OFF)
```

约束：

- `FAST_GDB_BUILD_ADAPTIVE_READER=ON` 要求 `FAST_GDB_WITH_GDAL=ON`；
- 新增可选安装 target `fast_gdb::adaptive`；
- `fast_gdb::linear` 保持无 GDAL 依赖；
- Adaptive 编译宏只属于新 target，不进入低层 Reader 公共头；
- Adaptive runtime target 已实现；本计划剩余工作集中于三平台、压力、性能、多 GDAL 版本和安装包证据，不扩大 Reader/Writer 边界。

### 4.2 `InProcessGdbCoordinator`

按规范化 GDB 路径维护：

```cpp
struct CoordinatedSourceState {
    bool writer_pending = false;
    bool writer_active = false;
    uint64_t generation = 0;
    size_t fast_reader_count = 0;
};
```

要求：

- 使用进程内互斥保护状态转换；
- 同一路径只允许一个 Pending/Active Writer；
- 不同 GDB 可独立并行；
- Windows 路径采用平台一致的规范化和大小写规则；
- 调用方确认 update Dataset 已关闭后递增 generation；
- 调用方报告 `GDALClose` 失败时也要使旧 Reader 失效，不能继续把源视为 `Verified`。

### 4.3 外部更新协调令牌

fast-gdb 不打开、不暴露、不 Flush、不关闭 GDAL update Dataset，也不导出任何 GDAL
Writer 类型。调用方继续直接使用官方 GDAL，只通过无 GDAL 类型的协调令牌报告状态。

准备更新时必须显式提供等待超时，不允许隐藏的无限等待：

```cpp
PrepareExternalUpdateResult prepare_external_update(
    const std::string& gdb_path,
    std::chrono::milliseconds drain_timeout);
```

行为：

1. 原子发布 `WriterPending`；
2. 阻止新的 fast Reader 租约；
3. 等待已有 fast Reader 在超时内退出；
4. 归零后向调用方返回 `ExternalUpdateToken`；
5. 调用方自己调用 `GDALOpenEx(...GDAL_OF_UPDATE...)`；
6. 打开失败时调用 `cancel_before_update()`，撤销 Pending；
7. 打开成功后调用 `notify_update_opened()`，转换为 `WriterActive`；
8. 调用方自己执行官方 GDAL 写入、Flush 和 `GDALClose()`；
9. Dataset 关闭后调用 `notify_update_closed(close_succeeded)`，递增 generation 并清除 Active。

令牌在 Pending 状态析构可以安全撤销 Pending。令牌进入 Active 后，如果调用方没有先
关闭 GDAL Dataset 并发送关闭通知，协调器必须保持 fail-closed，不能靠令牌析构猜测
Writer 已结束；恢复需要调用方确认 Dataset 已关闭后显式通知。应用层可自行用 RAII
封装“GDALClose → notify”，但该封装不属于 fast-gdb 安装面。

失败状态至少包括：

```text
ReadersActive
WriterAlreadyPending
WriterAlreadyActive
InvalidCoordinationToken
ExternalUpdateNotClosed
```

### 4.4 `AdaptiveReadSession`

```cpp
enum class ConcurrentReadPolicy {
    SourceBusy,
    GdalUnverified
};

enum class AdaptiveReadBackend {
    FastGdb,
    GdalOpenFileGDB
};

enum class AdaptiveReadConsistency {
    Verified,
    UnverifiedConcurrentRead
};
```

每个结果至少包含：

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

稳定状态：

- 获得 fast Reader 租约；
- 执行 fast-gdb；
- 返回 `FastGdb + Verified`；
- 低层 `GdbTableParser` 和 `QueryEngine` 接口保持不变。

Pending/Active 状态：

- `SourceBusy` 策略不调用任何读取后端；
- `GdalUnverified` 策略每次创建新的只读 GDALDataset；
- 查询结果完整物化并关闭 Dataset 后返回；
- 结果无条件标记为 `GdalOpenFileGDB + UnverifiedConcurrentRead`；
- Writer 在 GDAL 读取期间结束，也不得把本次结果升级为 `Verified`；
- GDAL 打开或读取失败分别返回 `GdalOpenFailed` / `GdalReadFailed`。

### 4.5 `AdaptiveFeatureCursor`

fast cursor：

- 注册为活动 fast Reader；
- WriterPending 发布后，正在执行的单次 `next()` 可以完成；
- 下一次安全点关闭底层 cursor、释放 mmap/句柄并返回 `ReaderExpired`；
- 调用方重新打开后按 Pending/Active 策略进入 GDAL 或 Busy。

GDAL cursor：

- 自己拥有 fresh GDALDataset；
- Pending/Active 期间不计入 fast Reader 数量；
- cursor 整个生命周期保持 `UnverifiedConcurrentRead`；
- 销毁时释放 Feature、SQL result set、Layer 引用并关闭 Dataset。

## 5. 状态机

```text
Stable
  ├─ Reader → FastReading → Return Verified
  └─ Writer request
        ↓
     WriterPending
        ├─ new Reader + SourceBusy → SourceBusy
        ├─ new Reader + GdalUnverified → Return Unverified
        ├─ old fast Reader → safe-point ReaderExpired
        └─ readers drained
              ↓
          WriterActive
              ├─ Reader + SourceBusy → SourceBusy
              ├─ Reader + GdalUnverified → Return Unverified
              └─ GDALClose
                    ↓
               generation + 1
                    ↓
               Stable / rebuild fast Reader
```

禁止：

- Writer 在 fast Reader 计数非零时打开 update Dataset；
- Pending 后继续创建新的 fast cursor；
- 将 GDAL 并发读取结果标记为 `Verified`；
- 复用旧 fast mmap、catalog、tablx 或索引缓存；
- 复用曲线回退的 thread-local GDALDataset；
- 无限等待或后台无界轮询。

## 6. 分阶段实施

### Phase 0：决策同步

- 修订 ADR-008，使其包含 `WriterPending` 和显式 `GdalUnverified`；
- 保留 ADR-007 当前正式合同；
- 同步 README、架构、使用说明和测试索引；
- 建立 Adaptive 测试 target，但不以空测试宣称实现完成。

### Phase 1：Coordinator 与外部更新协调

- 实现路径规范化、单 Writer、generation 和 Reader 计数；
- 实现 Pending、超时撤销和 Active 状态转换；
- 实现不依赖 GDAL Writer 类型的 `ExternalUpdateToken`；
- 确认调用方只在 fast Reader 归零后打开 update Dataset；
- Active 令牌异常丢失时保持 fail-closed，不猜测外部 Dataset 已关闭。

### Phase 2：fast Reader 租约和安全点过期

- fully-materialized query 持有查询级租约；
- cursor 注册长期 Reader，并在 `next()` 边界处理 Pending；
- generation 变化后完整失效 Reader 对象图；
- 不在逐 Feature 热路径增加全局文件快照。

### Phase 3：GDAL 未验证回退

- 实现 FID、顺序、空间、属性、WHERE 和 SpatialWhere；
- 每个 query fresh open/close；
- cursor 独占自己的 GDALDataset；
- 所有成功结果强制携带 `UnverifiedConcurrentRead`；
- 保持 fast 与 GDAL 的 FID、NULL、字段和几何输出语义可对照。

### Phase 4：写后恢复

- 调用方确认 update Dataset 已关闭后，关闭通知使 generation 递增；
- 旧 Reader 返回 `ReaderExpired`；
- 新 Reader 完整重建并恢复 `FastGdb + Verified`；
- 覆盖 Feature、Schema、索引、REPACK 和 extent 修改。

### Phase 5：三平台、性能和安装收口

- Windows、Linux、macOS 自动化；
- OpenFileGDB create/update 能力缺失时 required job 失败，不能 SKIP；
- 增加 adaptive package consumer；
- 验证 linear 包仍不依赖 GDAL；
- 记录 fast 稳定路径和 GDAL 并发路径的独立性能数据。

## 7. 测试矩阵

### 7.1 状态与路由

| 场景 | 策略 | 预期后端 | 一致性/状态 | 门禁 |
|---|---|---|---|---|
| 无 Writer | 任意 | fast-gdb | `Verified` | 正式 |
| WriterPending | `SourceBusy` | 不调用 | `SourceBusy` | 正式 |
| WriterActive | `SourceBusy` | 不调用 | `SourceBusy` | 正式 |
| WriterPending | `GdalUnverified` | fresh GDAL | `UnverifiedConcurrentRead` | 正式路由门禁 |
| WriterActive | `GdalUnverified` | fresh GDAL | `UnverifiedConcurrentRead` | 正式路由门禁 |
| GDAL 并发读取成功 | `GdalUnverified` | GDAL | 值不作一致性断言 | Characterization |
| GDAL 并发读取失败 | `GdalUnverified` | GDAL | 明确 GDAL 错误 | 正式错误门禁 |
| 调用方关闭 Dataset 并通知 | 任意 | 重建 fast | 新 generation `Verified` | 正式 |

### 7.2 Pending 与租约

| 场景 | 预期 |
|---|---|
| fast query 执行中发布 Pending | query 完成后释放租约，Writer 才可打开 |
| fast cursor 空闲时发布 Pending | 下一次 `next()` 返回 `ReaderExpired` 并关闭底层 Reader |
| fast cursor 当前 `next()` 中发布 Pending | 当前调用完成，后续安全点过期 |
| cursor 不再推进也不销毁 | 超时返回 `ReadersActive`、撤销 Pending，后续 Reader/Writer 可恢复进入 |
| Pending 等待期间的新 Reader | 不得增加 fast Reader 计数 |
| 同一路径第二个 Writer | 返回 AlreadyPending/AlreadyActive |
| 不同路径 Writer | 可并行 |
| 调用方 GDAL update 打开失败 | `cancel_before_update()` 撤销 Pending，不增加 generation |
| Active 令牌未收到关闭通知 | 保持 fail-closed，不允许新 fast Reader |
| 调用方报告 Dataset 已关闭 | 清除 Active，generation 增加，旧 Reader 失效 |

### 7.3 查询 parity

| QueryKind | fast 稳定路径 | GDAL Pending/Active 路径 |
|---|---|---|
| `ReadByFid` | 值和 FID 正确 | 结果可读，标记 Unverified |
| `SequentialScan` | 顺序、删除槽正确 | 同一 GDAL 调用内完成，标记 Unverified |
| `SpatialBbox` | spx/精确过滤正确 | OGR spatial filter，标记 Unverified |
| `AttributeDouble/String` | atx/回退正确 | OGR attribute filter，标记 Unverified |
| `WhereClause` | fast WHERE 正确 | `SetAttributeFilter`，标记 Unverified |
| `SpatialWhere` | 联合查询正确 | GDAL 联合过滤，标记 Unverified |

稳定数据源下，fast 与 GDAL 必须比较 FID 集合、字段值、NULL、WKB 和错误分类。
Writer 活动期间只验证路由、所有权、状态标记和进程稳定性，不比较业务结果一致性。

### 7.4 GDAL 编辑覆盖

| GDAL 操作 | 写前 | 写中 | 写后 |
|---|---|---|---|
| `SetFeature` | fast Verified | GDAL Unverified/Busy | 新 fast 值 Verified |
| `CreateFeature` | 旧计数 | 不断言计数 | 新 FID/计数 Verified |
| `DeleteFeature` | FID 可读 | 不断言删除状态 | 删除槽/扫描 Verified |
| 几何更新 | 旧 WKB | 不断言 WKB | 新 WKB/空间查询 Verified |
| Create/Delete Field | 旧 Schema | 不断言 Schema | 新 Schema Verified |
| Create/Delete Index | 旧索引状态 | 不断言索引 | 索引或正确回退 Verified |
| `REPACK` | 旧物理布局 | 只验证无 fast mmap 重叠 | tablx/FID Verified |
| Recompute extent | 旧范围 | 不断言范围 | 新范围 Verified |

### 7.5 计划测试名称

```text
fast_gdb_adaptive_reader_test_runner
adaptive-reader.unit.*
adaptive-reader.coordinated.*
adaptive-reader.gdal-unverified.*
adaptive-reader.parity.*
adaptive-reader.lifecycle.*
adaptive-reader.stress.*
```

首批测试：

- `StableSourceUsesFastVerified`;
- `WriterPendingStopsNewFastReads`;
- `FastCursorExpiresAtNextSafePoint`;
- `PendingTimeoutClearsPendingAndRecovers`;
- `UpdatePermitRequiresFastReadersDrained`;
- `UpdateOpenFailureCancelsPending`;
- `AbandonedActiveTokenRemainsFailClosed`;
- `DefaultPolicyReturnsBusyWithoutCallingBackends`;
- `ExplicitPolicyUsesFreshGdalUnverified`;
- `UnverifiedResultIsNeverReportedVerified`;
- `GdalFailureKeepsDiagnosticAndConsistency`;
- `ClosedUpdateNotificationIncrementsGeneration`;
- `OldReaderExpiresAfterClosedUpdateNotification`;
- `PostWriteRebuildReturnsFastVerified`;
- `AllQueryKindsMatchOnStableSource`;
- `RepackNeverOverlapsFastMmap`;
- `MultipleReadersSingleWriterStress`.

## 8. 构建与 CI 矩阵

| 构建 | GDAL | Adaptive | 目的 |
|---|---:|---:|---|
| Linear | OFF | OFF | 纯 Reader、无 GDAL 依赖 |
| Hybrid | ON | OFF | 当前几何回退 |
| Adaptive configure error | OFF | ON | 必须配置失败 |
| Adaptive | ON | ON | Coordinator、外部更新协调和读取切换 |
| Sanitizer | ON | ON | 生命周期、竞态和资源释放 |
| Package consumer | ON | ON | 安装 target 与公共接口 |

CI 要求：

- Linux、macOS、Windows；
- 至少一个最低支持 GDAL 和一个当前稳定 GDAL；
- required 测试不得因 OpenFileGDB update 缺失而 SKIP；
- 上传测试日志、GDAL 版本、构建选项和压力测试摘要；
- `UnverifiedConcurrentRead` 的 old/new/mixed/error 观测只作为 artifact，不作为 PASS 值。

## 9. 性能与可观测性

稳定 fast 路径：

- 每次 query 或 cursor 打开只做一次协调状态读取；
- 不在逐 Feature 热路径读取全局状态；
- 不扫描进程全部 GDAL handle；
- 不默认捕获整个 GDB 文件快照。

GDAL 未验证路径单独记录：

- Pending/Active；
- GDAL open/read/close 时间；
- 查询类型和返回数量；
- Writer 操作类型；
- `UnverifiedConcurrentRead`；
- GDAL 错误。

不得把 GDAL 并发路径性能与稳定 fast 路径混为同一性能结论。

## 10. 风险

| 风险 | 控制措施 |
|---|---|
| 长 cursor 阻塞 Writer | Pending 安全点过期；显式等待超时 |
| 调用方绕过外部更新协调入口 | 文档明确不在检测合同内；测试只覆盖统一入口 |
| GDAL 并发结果被误用 | 策略显式开启；结果强制携带 Unverified |
| mmap 与 REPACK 重叠 | Writer 打开前 fast Reader 计数必须为零 |
| GDAL Dataset 被缓存复用 | query fresh session；cursor 独占 session |
| 调用方报告 close 失败 | 增加 generation、保留诊断并使旧 Reader 过期 |
| Active 关闭通知丢失 | 保持 fail-closed，待调用方确认 Dataset 已关闭后恢复 |
| 计划与 ADR 冲突 | ADR-008 已 Accepted；新增行为仍不得修改 ADR-007 的 Reader/Writer 当前合同 |

## 11. 完成定义

只有满足以下条件，计划才能标记 Completed：

- ADR-008 已按本计划修订并 Accepted；
- 三平台 required 自动化通过且无 required SKIP；
- 调用方从未在 fast Reader 计数非零时打开 update Dataset；
- 所有 GDAL 并发成功结果都标记 `UnverifiedConcurrentRead`；
- 稳定源全部 QueryKind 完成 fast/GDAL parity；
- Feature、Schema、索引、REPACK 和 extent 写后重建测试通过；
- 压力测试无崩溃、死锁或资源泄漏；
- linear、hybrid、adaptive 安装面验证通过；
- 性能和 characterization artifact 可审计；
- README、ADR、架构、使用、测试和规划文档口径一致。
