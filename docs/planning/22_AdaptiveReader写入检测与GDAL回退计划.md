# Adaptive Reader 写入检测与 GDAL 只读回退计划

- **状态**：Active / Design first
- **日期**：2026-07-22
- **架构依据**：[ADR-008：Adaptive Reader 写入检测与 fresh GDAL 只读回退](../adr/ADR-008-adaptive-reader-write-detection-gdal-fallback.md)
- **产品边界**：Reader only；不实现任何 FileGDB 写入能力

## 1. 目标

实现一个可选的 Reader 编排层，在不改变现有 `fast_gdb::linear` 低层读取职责的前提下提供：

```text
正常稳定数据源
  → fast-gdb 高性能读取

fast-gdb 不支持或确定性读取失败
  → fresh GDAL/OpenFileGDB 只读回退

检测到活动 Writer 或读取期间数据源变化
  → 丢弃结果
  → 返回 SourceBusy / SourceChangedDuringRead

写入完成且数据源恢复稳定
  → fresh GDAL 只读恢复
  → 后续重建 fast-gdb Reader
```

目标不是实现边写边读，而是把不安全重叠转换为可诊断的拒绝和恢复。

## 2. 成功标准

### 协调模式

- Writer 活动期间不进入 fast-gdb 或 GDAL 读取；
- generation 读取前后不变才允许发布结果；
- generation 变化使旧 Reader 立即过期；
- 写入结束后 fresh GDAL 能读取完整新状态；
- 压力测试只允许完整旧版本、完整新版本或 `SourceBusy`。

### 无协调模式

- 对依赖文件的身份、大小、mtime、增加、删除和替换进行前后检测；
- 发现变化时绝不返回 fast-gdb 结果；
- fresh GDAL 读取期间再次变化时绝不返回 GDAL 结果；
- API、日志和文档明确标记为 best-effort，不宣称可发现所有外部 Writer。

### 产品边界

- 不新增 Writer target、Writer API、Writer ABI；
- 不创建或修改外部 writer marker；
- 不包装 GDAL update；
- 不提供事务、回滚、发布或版本回收。

## 3. 设计原则

1. **Fail closed**：无法判断源是否稳定时返回 Busy，不返回猜测结果；
2. **Fresh fallback**：恢复路径每次新开并关闭 GDALDataset；
3. **Materialize before publish**：源校验完成前不得暴露借用视图；
4. **Full invalidation**：源变化后旧 catalog/table/cursor/index/mmap 全部失效；
5. **No hidden wait**：默认立即返回 Busy；等待和重试必须由显式策略控制；
6. **Dependency-aware**：只监测本次查询依赖的文件，同时允许目录级兜底；
7. **Observable**：后端、变化文件、generation 和重试次数必须可诊断；
8. **Zero Writer coupling**：协调接口只读取调用方状态，不接管写入生命周期。

## 4. 目标组件

### 4.1 `FileStamp`

跨平台文件身份和元数据：

```cpp
struct FileStamp {
    uint64_t device_or_volume = 0;
    uint64_t inode_or_file_id = 0;
    uint64_t size = 0;
    int64_t mtime_ticks = 0;
    bool exists = false;
};
```

实施要求：

- POSIX 使用 `stat` 的 device/inode/size/高精度 mtime；
- Windows 使用 VolumeSerialNumber/FileIndex/FileSize/FILETIME；
- 从现有 `TablxCacheKey` 提取共用实现；
- 文件不存在、被替换或读取元数据失败均可区分。

### 4.2 `GdbSourceSnapshot`

记录一次查询所依赖的数据源状态：

```cpp
struct GdbSourceSnapshot {
    uint64_t generation = 0;
    std::vector<StampedPath> dependencies;
    std::vector<std::string> lock_signals;
};
```

依赖集合至少覆盖：

- 目标 `.gdbtable`；
- 目标 `.gdbtablx`；
- 查询使用的 `.spx`；
- 查询使用的 `.gdbindexes/.atx`；
- 解析图层和 Schema 所需的系统表；
- 可选目录清单摘要。

### 4.3 `WriterActivityProbe`

只读协调接口：

```cpp
class WriterActivityProbe {
public:
    virtual ~WriterActivityProbe() = default;
    virtual WriterActivityState observe(const std::string& gdb_path) = 0;
};

struct WriterActivityState {
    bool writer_active = false;
    uint64_t generation = 0;
    bool coordinated = false;
};
```

首批实现：

- `NullWriterActivityProbe`：无协调模式；
- `CallbackWriterActivityProbe`：调用方回调；
- 可选 `SidecarWriterActivityProbe`：只读取约定的 activity/generation 文件。

fast-gdb 不负责创建或删除 sidecar 文件。

### 4.4 `GdbChangeDetector`

职责：

- 捕获依赖快照；
- 比较 before/after；
- 输出变化文件和变化类型；
- 对 stat 失败采取 fail-closed；
- 可选稳定窗口只作为无协调模式启发式，不作为写入结束证明。

### 4.5 `AdaptiveReadSession`

统一编排 fast 和 GDAL 后端：

```cpp
AdaptiveReadResult read(const AdaptiveReadRequest& request);
void invalidate();
bool expired() const;
```

建议不改变低层 `GdbTableParser` 的基础接口，先在更高层实现编排和失效管理，降低对现有性能路径的侵入。

### 4.6 `FreshGdalReadSession`

职责：

- 每次请求新建 `GDALDataset`；
- 完整属性、几何和查询结果物化；
- 释放 Feature/SQL result set；
- `GDALClose()` 后才允许返回；
- 不访问 `GdalCurveBackendBridge` 的 thread-local Dataset 缓存。

## 5. 状态与错误

### 状态机

```text
Ready
  → FastReading
  → FastValidated
  → ReturnFast

Ready
  → WriterActive
  → SourceBusy

FastReading
  → SourceChanged
  → Expired
  → QuiescenceCheck
  → FreshGdalReading
  → GdalValidated
  → ReturnGdal
```

### 结果状态

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

诊断字段：

- `backend`；
- `attempt_count`；
- `coordinated`；
- `writer_active_seen`；
- `generation_before/after`；
- `changed_files`；
- `fast_error`；
- `gdal_error`。

## 6. 分阶段实施

## Phase 0：合同和测试骨架

交付：

- ADR-008；
- 本计划；
- 新测试 target 和空实现接口；
- 明确现有 ADR-007 在新能力验收前仍是唯一正式合同。

门禁：

- 文档不暗示功能已经实现；
- 安装面仍为 Reader-only；
- 现有 boundary tests 不改变语义。

## Phase 1：跨平台文件快照基础

任务：

- 提取 `FileStamp`；
- 将 `TablxCacheKey` 改为复用 `FileStamp`；
- 为 `GdbCatalog` 增加可选高精度快照；
- 实现依赖文件枚举和差异诊断。

测试：

- 同长度原地修改；
- 文件增长/缩小；
- 删除后重建；
- rename/replace；
- mtime 纳秒或 FILETIME；
- stat 失败；
- UTF-8 Windows 路径。

验收：

- 三平台相同语义；
- 不影响现有 tablx cache 正确性；
- 快照操作有独立性能基线。

## Phase 2：协调探针与 Reader 失效

任务：

- `WriterActivityProbe`；
- callback 和 null 实现；
- generation 前后检查；
- `ReaderExpired` 状态；
- 统一 invalidation hook，清理 catalog/table/index/cursor 和相关缓存。

测试：

- writer active 时 fast/GDAL 后端调用计数为 0；
- generation 变化使 Reader 过期；
- 旧 Reader 不得自动恢复；
- 新 Reader 可在新 generation 上打开。

## Phase 3：fast 路径前后校验

任务：

- 读取前捕获 activity + snapshot；
- fast-gdb 完整物化结果；
- 读取后再次捕获；
- 变化时丢弃结果并记录差异；
- 禁止向调用方泄露 `FieldRef`、mmap 指针和 cursor 借用状态。

测试：

- `.gdbtable` 修改；
- `.gdbtablx` 修改；
- `.spx` 修改；
- `.atx/.gdbindexes` 修改；
- Schema/system table 修改；
- 结果物化完成前发生变化；
- 结果物化完成后、发布前发生变化。

## Phase 4：fresh GDAL 完整读取回退

任务：

- 新建 `FreshGdalReadSession`；
- 支持与首批 fast API 对等的 FID 读取和基础查询；
- 每次请求 fresh open/close；
- 读取前后快照校验；
- 明确区分 fast unsupported、fast read failed、source changed。

测试：

- fast 人工不支持、源稳定 → GDAL 成功；
- GDAL open/read 失败；
- GDAL 读取期间源变化 → 丢弃；
- 写后 fresh GDAL 读到新值；
- 旧 thread-local GDAL cache 不被复用。

## Phase 5：稳定判断与策略

任务：

- 默认 `ImmediateBusy`；
- 可选有界重试 `BoundedRetry`；
- 协调模式以 `writer_active=false + generation stable` 为准；
- 无协调模式以快照稳定 + fresh GDAL 前后验证为准；
- 超时返回 `SourceNeverStabilized`。

禁止：

- 无限等待；
- 后台轮询线程；
- 仅凭连续两次 mtime 相同宣称写入完成；
- `.lock` 不存在即判定安全。

## Phase 6：压力、故障和平台验收

场景：

- `SetFeature`；
- `CreateFeature`；
- `DeleteFeature`；
- 变长字符串和几何更新；
- Schema 修改；
- 属性/空间索引重建；
- `REPACK`；
- Writer 中途失败；
- marker 残留；
- 高频短写和长事务；
- 本地盘、网络盘 profile；
- Windows/Linux/macOS；
- 多个 GDAL 稳定版本。

硬门禁：

- 不崩溃；
- 不发布 mixed 结果；
- 协调模式只返回完整旧、完整新或 Busy；
- 无协调模式不得把 best-effort 观测写成强保证；
- 所有失败包含可操作诊断。

## Phase 7：API、安装和文档收口

任务：

- 决定是否作为 `fast_gdb::adaptive` 可选 Reader target；
- 保持 `fast_gdb::linear` 无 GDAL 依赖；
- Adaptive target 显式依赖 GDAL；
- package consumer 增加 adaptive 只读用例；
- 更新 README、usage、architecture、test index 和 changelog；
- ADR-008 通过验收后从 Proposed 改为 Accepted。

安装门禁：

- 无 Writer 头和 Writer target；
- 无 GDAL update 符号或包装 API；
- `usegdal` 仍为 reference only；
- adaptive API 名称和文档只描述读取、拒绝和回退。

## 7. 测试目标规划

建议新增：

```text
fast_gdb_adaptive_reader_test_runner
adaptive-reader.unit.*
adaptive-reader.coordinated.*
adaptive-reader.uncoordinated.*
adaptive-reader.gdal-fallback.*
adaptive-reader.stress.*
```

首批正式测试名称建议：

- `WriterActiveReturnsBusyWithoutCallingEitherBackend`；
- `GenerationChangeExpiresExistingReader`；
- `StableFastReadReturnsMaterializedFastResult`；
- `ChangedSourceDiscardsFastResult`；
- `StableUnsupportedFastReadUsesFreshGdal`；
- `ChangedSourceDuringGdalReadDiscardsFallbackResult`；
- `CompletedWriteFreshGdalReadsNewGeneration`；
- `FreshGdalFallbackDoesNotReuseCachedDataset`。

## 8. 性能预算

Adaptive 层不能让稳定数据源下的快路径失去 fast-gdb 的主要价值。

初始预算：

- 协调模式仅两次轻量 activity/generation 读取；
- 文件快照按查询依赖收集，避免默认哈希整个 GDB；
- 不在每次读取中做全文件内容哈希；
- 允许调用方按请求粒度选择 `None/Coordinated/BestEffort` 检测策略；
- 建立 FID lookup、顺序扫描、空间查询的额外延迟和吞吐回归门禁；
- 性能优化不能削弱 fail-closed 语义。

## 9. 风险

| 风险 | 控制措施 |
|---|---|
| 外部 Writer 无协调导致漏检 | 明确 best-effort；前后快照；fresh GDAL 再验证 |
| mmap 文件截断导致进程异常 | 正式强保证依赖协调模式；压力测试 REPACK/replace；必要时为 adaptive 模式提供非 mmap 安全读取策略 |
| 快照开销抵消性能收益 | 依赖感知；协调模式优先；基准和预算 |
| GDAL fallback 复用旧缓存 | 独立 fresh session；禁止使用 thread-local cache |
| 状态过多导致 API 混乱 | 明确状态枚举、诊断和重试分类 |
| 功能被误解为 Writer | target/API/文档全程使用 Reader/fallback/busy 术语 |
| marker 残留导致永久 Busy | 显式超时和诊断；不自动删除 marker |

## 10. 完成定义

只有满足以下条件，计划才能标记 Completed：

- ADR-008 Accepted；
- 三平台自动化通过；
- 协调模式压力测试无 mixed、无崩溃；
- fresh GDAL 回退前后验证通过；
- 所有旧 Reader 和缓存失效测试通过；
- 性能预算有可复现 artifact；
- package install 仍严格 Reader-only；
- README、架构、使用、测试和规划文档口径一致；
- CI 日志和 artifact 可用，不能仅以代码存在宣称完成。