# ADR-008：Adaptive Reader 写入检测与 fresh GDAL 只读回退

- **状态**：Proposed
- **日期**：2026-07-22
- **依赖**：[ADR-007：Reader-only 与 GDAL 编辑边界](ADR-007-reader-only-gdal-edit-boundary.md)

## 背景

ADR-007 已确定 fast-gdb 只提供 FileGDB Reader，所有创建和编辑由 GDAL/OpenFileGDB 或 ArcGIS 完成。当前受支持流程要求写前关闭全部 Reader、写后完整重开。

实际集成中可能存在以下情况：

- fast-gdb 正在读取时，另一个线程或进程开始修改同一个 `.gdb`；
- fast-gdb 因格式、几何或索引能力不足而读取失败；
- 外部写入已经完成，但调用方希望先通过 GDAL 只读路径恢复服务，再重建 fast-gdb Reader；
- 调用方只需要统一的读取接口，不希望 fast-gdb 承担任何写入、事务或发布职责。

仅以“fast-gdb 是否报错”判断是否回退不安全。外部写入期间 fast-gdb 可能返回旧值、新值、跨文件混合值或解析错误，也可能在 mmap 文件被截断或替换时遇到进程级异常。GDAL 只读打开也不能被视为正在写入时的并发安全快照。

## 决策目标

在不引入 FileGDB Writer 的前提下，规划一个 Reader-only 的 `Adaptive Reader`：

```text
稳定数据源
  → 优先 fast-gdb 读取
  → fast-gdb 不支持或读取失败时使用 fresh GDAL 只读回退

检测到写入活动或读取期间源发生变化
  → 丢弃本次结果
  → 返回 SourceBusy / SourceChangedDuringRead

确认写入结束且数据源稳定
  → 使用全新 GDALDataset 只读读取
  → 完整物化结果并关闭 GDALDataset
  → 后续重新创建 fast-gdb Reader
```

该能力只决定“何时可以安全读取、使用哪个读取后端、何时拒绝返回结果”，不负责执行写入。

## 两层检测合同

### 1. 协调模式：正式支持目标

写入方通过调用方提供的最小协调信号暴露：

- `writer_active`：当前是否处于写入生命周期；
- `generation`：完成一次写入后递增的稳定版本号。

fast-gdb 只消费该信号，不创建、删除或修改写入标记，也不包装 GDAL update API。协调信号可以来自：

- 进程内状态对象；
- 跨进程共享状态；
- sidecar activity/generation 文件；
- 调用方实现的回调或接口。

正式语义：

```text
writer_active == true
  → 不启动 fast-gdb 读取
  → 不启动 GDAL fallback
  → 返回 SourceBusy

writer_active == false
且 generation 在读取前后不变
  → 允许返回完整物化结果

读取前后 generation 不同
  → 丢弃结果
  → 使旧 fast-gdb Reader 过期
```

协调模式是能够提供确定性行为的主要合同。

### 2. 无协调模式：best-effort 兼容目标

对于 ArcGIS、QGIS、第三方 GDAL 程序或其他不遵守协调协议的外部 Writer，只能通过文件系统变化进行启发式检测：

- 文件身份：device/inode 或 volume/file ID；
- 文件大小；
- 高精度最后修改时间；
- 文件新增、删除和替换；
- `.gdbtable/.gdbtablx/.spx/.atx/.gdbindexes` 及相关系统表变化；
- 可选 `.lock` 文件信号。

无协调模式必须明确标记为 `BestEffortExternalWriterDetection`。它可以发现大量重叠写入，但不能承诺捕获任意外部 Writer 的完整开始和结束时刻。

连续若干次文件指纹相同也不能单独证明 Writer 已结束，因为 Writer 可能暂停。只有在 fresh GDAL 完整读取成功且读取前后源快照一致时，才允许返回 GDAL 结果；若仍发生变化，则返回 `SourceBusy` 或 `SourceNeverStabilized`。

## 读取状态机

```text
FastReady
  ├─ writer active ───────────────────────→ SourceBusy
  └─ capture snapshot A
          ↓
      FastReading
          ↓ materialize result
      capture snapshot B
          ├─ A == B and fast success ─────→ ReturnFast
          └─ changed or fast unsupported ─→ InvalidateFast
                                                ↓
                                          Wait/CheckQuiescent
                                                ├─ writer active/unstable → SourceBusy
                                                └─ stable → FreshGdalReading
                                                               ↓ materialize
                                                          close GDALDataset
                                                               ↓
                                                          verify snapshot
                                                               ├─ stable → ReturnGdal
                                                               └─ changed → SourceBusy
```

## 结果生命周期

任何需要读取后校验的路径都必须先完整物化结果，再比较源快照。不得在校验完成前向调用方暴露：

- mmap 指针；
- `FieldRef`；
- 指向行缓冲区的视图；
- 指向 GDAL `OGRFeature` 或 `OGRGeometry` 的非拥有指针；
- 依赖旧 Reader 生命周期的 cursor。

若源在读取期间变化，本次已物化结果也必须丢弃。

## GDAL 回退规则

Adaptive Reader 的 GDAL 回退必须使用 `FreshGdalReadSession` 语义：

1. 在确认没有活动 Writer 后捕获源快照；
2. 以 `GDAL_OF_VECTOR | GDAL_OF_READONLY` 新开 Dataset；
3. 完整物化本次查询结果；
4. 释放 Feature 和 SQL result set；
5. `GDALClose()` Dataset；
6. 再次捕获源快照；
7. 快照一致时才返回结果。

不得复用写入前、变化前或其他请求遗留的 GDALDataset。现有曲线回退中的 thread-local GDAL Dataset 缓存不能直接承担该恢复职责。

## fast-gdb Reader 失效规则

出现以下任一情况后，当前 fast-gdb Reader 对象图必须标记为 expired，并在后续 fast 路径前完整重建：

- 协调 generation 变化；
- 依赖文件身份、大小或修改时间变化；
- 文件新增、删除或替换；
- fast-gdb 读取期间检测到源变化；
- GDAL fallback 被触发且源可能已经更新；
- mmap、文件句柄、Schema、tablx 或索引状态异常。

不能通过局部重载 `.gdbtablx`、单独清空索引缓存或继续复用旧 `GdbTableParser` 恢复有效状态。

## 缓存要求

实施时必须覆盖：

- 将现有 `TablxCacheKey` 的文件身份能力抽取为通用 `FileStamp`；
- `GdbCatalog` 快照包含依赖文件身份、大小和高精度 mtime；
- generation 变化时使 tablx、catalog、index 和 GDAL fallback 缓存失效；
- Adaptive recovery 路径禁止复用 `GdalCurveBackendBridge` 的 thread-local Dataset；
- 旧 Reader 的 mmap、fd/HANDLE 和 cursor 必须关闭。

## 错误模型

规划以下可区分状态：

```cpp
enum class AdaptiveReadStatus {
    Ok,
    SourceBusy,
    SourceChangedDuringRead,
    ReaderExpired,
    FastBackendUnsupported,
    FastBackendReadFailed,
    GdalOpenFailed,
    GdalReadFailed,
    SourceNeverStabilized
};
```

诊断信息至少包含：

- 实际后端；
- 尝试次数；
- 是否看到 Writer 活动；
- generation 前后值；
- 发生变化的文件；
- fast-gdb 和 GDAL 的失败原因。

## 非目标

本 ADR 不引入：

- FileGDB Writer；
- GDAL update 包装 API；
- 事务、回滚或恢复；
- fast-gdb 创建/删除 writer marker；
- 对任意外部 Writer 的绝对检测保证；
- 正在写入时通过 GDAL 强行返回结果；
- 在线副本发布、路由切换和旧版本回收；
- 写后局部 refresh；
- 无限等待或后台监控线程。

## 验收条件

在本 ADR 从 Proposed 转为 Accepted 前，至少需要：

1. 协调模式 Writer 活动期间确定性返回 `SourceBusy`；
2. generation 变化时旧 Reader 确定性返回 `ReaderExpired`；
3. 无写入时 fast-gdb 快速路径保持正确；
4. fast-gdb 不支持且源稳定时 fresh GDAL 回退正确；
5. fast-gdb 读取期间源变化时结果被丢弃；
6. GDAL 读取期间源变化时结果被丢弃；
7. 写入结束后 fresh GDAL 读取到完整新状态；
8. 不复用旧 GDALDataset、旧 mmap、旧 tablx 或旧索引缓存；
9. 协调模式压力测试只能产生完整旧版本、完整新版本或 `SourceBusy`，不得出现 mixed；
10. Windows、Linux、macOS 均有可复现测试和日志；
11. 无协调模式文档和 API 名称明确包含 best-effort 限定；
12. 安装面仍不包含 Writer target、Writer API 或 Writer ABI。

## 决策关系

ADR-007 在本 ADR 实现和验收前仍是当前唯一受支持合同：

```text
close Readers → GDAL edit → GDALClose → reopen Readers
```

ADR-008 不放宽“Reader 与 Writer 重叠不支持”的边界，而是规划将重叠访问转换为确定性的拒绝、失效和读取后端恢复。