# fast-gdb 统一访问与 GDAL/S3 路由计划

**状态**：Local implementation in progress / release gates pending

**修订**：2026-07-31 二轮架构、接口、故障、安全、构建与发布自检版

**目标版本**：v0.2.0

**范围**：统一只读访问入口、FileGDB 后端路由、完整 Group 模型、
`FastFileGDB` GDAL/OGR 兼容入口和对象存储边界。

本文同时记录冻结设计和 v0.2.0 实施状态。统一入口、共享 runtime、本地
fast/GDAL 路由和 `FastFileGDB` 驱动已有可运行实现；完整 Schema parity、GDAL
Group/plugin 合同、版本/平台矩阵、故障注入和真实 AWS characterization 尚未完成。

## 1. 最终设计结论

fast-gdb 不直接把 S3 当作本地 FileGDB 目录读取，也不在核心 Reader 中加入对象存储
适配。新增一个 source-neutral 的统一访问 seam，并让 C++ facade 与 GDAL 插件共享
同一份进程级 runtime：

```text
C++ application
    → fast_gdb::unified
        → fast_gdb_runtime (shared)
            → BackendRouter
                ├─ FastGdbAdapter
                └─ GdalOpenFileGdbAdapter

GDAL/OGR application
    → gdal_FastFileGDB plugin
        → 同一个 fast_gdb_runtime
```

路由原则：

```text
本地稳定 .gdb + fast 能力满足
    → fast-gdb

s3://... 或 /vsis3/...
    → 官方 GDAL/OpenFileGDB

本地只读能力缺口
    → 仅在允许的 fallback 白名单内使用官方 GDAL/OpenFileGDB
```

统一接口保持 Reader-only。Writer、事务、远程更新、对象锁和对象存储发布不进入
v0.2.0。

## 2. 模块和构建边界

### 2.1 新增模块

- `fast_gdb::unified`：稳定 C++ 统一接口；
- `fast_gdb_runtime`：共享动态库，承载 Router、GDAL Adapter、coordinator、
  generation 和进程级缓存；
- `gdal_FastFileGDB`：只读 GDAL vector plugin；
- `FastGdbAdapter`：把现有 Reader/QueryEngine 转为统一模型；
- `GdalOpenFileGdbAdapter`：只允许调用官方 `OpenFileGDB`。

### 2.2 保持兼容

现有 target 不替换、不改名：

- `fast_gdb::linear`；
- 可选 `fast_gdb::hybrid`；
- 可选 `fast_gdb::adaptive`。

现有 target 继续保持当前静态库和 Reader-only 合同。统一 facade 和插件必须通过
同一份 `fast_gdb_runtime` 访问进程级协调状态，禁止各自静态复制 coordinator；
否则同一进程会出现互相不可见的 Reader/Writer 状态。

### 2.3 CMake

新增：

```cmake
FAST_GDB_BUILD_UNIFIED=ON
FAST_GDB_BUILD_GDAL_DRIVER=<FAST_GDB_WITH_GDAL>
```

- `FAST_GDB_WITH_GDAL=OFF`：仍构建 `fast_gdb::unified` 和 fast-only runtime；
- GDAL OFF 下，S3 和 `GdalOnly` 返回 `BackendUnavailable`；
- GDAL driver 只能在 `FAST_GDB_WITH_GDAL=ON` 时构建；
- runtime 与 plugin 携带相同版本和 build ID，不匹配时拒绝注册；
- Linux 配置相对 RPATH，macOS 配置 install name，Windows 同包放置 runtime DLL；
- 不自动修改宿主的 `GDAL_DRIVER_PATH`。

## 3. 公开类型与接口

### 3.1 基础策略

```cpp
namespace fast_gdb {

using Fid = std::int64_t;

enum class BackendPreference {
    Auto,
    FastOnly,
    GdalOnly,
};

enum class ResultOrder {
    Native,
    FidAscending,
};

struct OpenOptions {
    BackendPreference backend = BackendPreference::Auto;
    ConcurrentReadPolicy concurrent_read =
        ConcurrentReadPolicy::SourceBusy;
    RemoteSourcePolicy remote_source =
        RemoteSourcePolicy::ImmutablePrefixRequired;
    bool include_system_tables = false;
};

struct Query {
    std::vector<std::string> projected_fields;
    std::string attribute_filter;
    std::optional<Envelope> spatial_filter;
    std::uint64_t offset = 0;
    std::uint64_t limit = 0;
    ResultOrder order = ResultOrder::Native;
    CancellationToken cancellation;
    std::optional<Deadline> deadline;
};

struct ReadAllOptions {
    std::uint64_t max_features = 1'000'000;
    std::uint64_t max_materialized_bytes = 512_MiB;
    std::uint64_t max_feature_bytes = 64_MiB;
    bool unlimited = false;
};

}  // namespace fast_gdb
```

`Fid` 使用 OGR/FileGDB 兼容的正数 64 位语义，`-1` 保留为无效值。fast Adapter
只在 `FID - 1` 可安全映射为当前零基 `uint32_t` row slot 时执行；超出范围时
`Auto` 路由 GDAL，`FastOnly` 返回 `Unsupported`。

### 3.2 Dataset、Group 和 Layer

```cpp
class Dataset {
public:
    static Result<Dataset> open(std::string uri,
                                OpenOptions options = {});

    Result<Group> root_group() const;
    Result<std::vector<std::string>> layer_names() const;
    Result<Layer> open_layer(std::string_view name) const;
    Result<Layer> open_layer_by_path(std::string_view path) const;
};

class Group {
public:
    Result<std::vector<GroupInfo>> groups() const;
    Result<std::vector<LayerInfo>> layers() const;
    Result<Group> open_group(std::string_view name) const;
};

class Layer {
public:
    Result<FeatureRead> read_by_fid(Fid fid) const;
    Result<ReadBatch> read_all(
        const Query& query = {},
        ReadAllOptions options = {}) const;
    Result<FeatureCursor> open_cursor(
        const Query& query = {}) const;
    Result<CapabilityReport> capabilities() const;
    Result<QueryPlan> explain(const Query& query) const;
    Result<FastLayerExtensions> fast_extensions() const;
};

class FeatureCursor {
public:
    Result<std::optional<Feature>> next();
    BackendReport backend_report() const;
    QueryReport query_report() const;
    void close() noexcept;
};
```

- `Dataset`、`Group` 和 `Layer` 是共享不可变状态的轻量句柄；
- `FeatureCursor` 可移动、不可复制、只能由一个线程使用；
- 每个 Cursor 创建独立 fast Reader 或 GDALDataset；
- `Result<T>` 使用项目内 C++17 expected-like 类型，不增加第三方依赖；
- 操作错误不通过异常跨越公开接口；
- v0.2.0 承诺源码接口和同一工具链内的 runtime/plugin ABI，不承诺跨编译器稳定
  C++ ABI。

## 4. 标准 Feature 和 fast 专有扩展

### 4.1 标准 Feature

标准接口严格采用 OGR 兼容的拥有型语义：

- Integer、Integer64、Real、String、Binary、Date、Time、DateTime；
- UUID 在标准接口中映射为 String；
- 每个字段明确区分 `Unset`、`Null` 和 `Value`；
- 字段投影不能用 NULL 代替“未物化”；
- Geometry 明确区分无几何、NULL 几何、空几何和有效几何；
- Geometry 返回拥有型 WKB、geometry type 和 SRS；
- Z/M、曲线和 MultiPatch 不得静默降维；
- `Feature` 不暴露 GDAL、OGR、mmap、`FieldRef` 或解析器借用指针。

### 4.2 fast 专有接口

```cpp
Result<FastLayerExtensions> Layer::fast_extensions() const;

Result<FastNativeFeature>
FastLayerExtensions::read_native_by_fid(
    Fid fid,
    NativeReadLimits limits = {});
```

获取 `FastLayerExtensions` 即进入 `FastOnly` 合同：

- 能返回 FileGDB 原始字段类型；
- OLE 时间原值；
- UUID_1/UUID_2；
- descriptor 与原始字段字节；
- 内部零基 row slot；
- `.spx/.atx` 和执行路径诊断。

GDAL 后端不得伪造这些信息。fast 无法提供时返回 `ExtensionUnavailable` 或
`Unsupported`。所有原始数据仍为拥有型，并受单项和总字节上限保护。

## 5. Group、Schema 和名称合同

### 5.1 完整 Group 优先

第一版提供完整 Root Group 和 Feature Dataset 层级：

- `Dataset::root_group()` 是正式入口；
- 支持嵌套 Group 和 Group 下的 Layer 枚举；
- `layer_names()` 仅作为传统扁平兼容视图；
- 系统表默认隐藏；
- `include_system_tables=true` 时才枚举内部表。

扁平视图出现重名时：

- `open_layer(name)` 返回 `AmbiguousLayer`；
- 调用方必须使用 `open_layer_by_path()`；
- canonical path 使用 UTF-8 `/FeatureDataset/Layer` 形式。

名称查找优先精确 UTF-8 匹配，再进行唯一的 ASCII 大小写不敏感匹配。嵌入 NUL、
歧义名称和非法 path segment 直接返回结构化错误。

### 5.2 Schema 冻结

标准 Layer Schema 在 Layer 打开时冻结：

- fast 已知字段映射不完整时，标准 Layer 整体使用 GDAL；
- 查询阶段需要 GDAL fallback 时，必须在 Cursor 返回前验证 GDAL Schema；
- GDAL Schema 与冻结 Schema 不一致时返回 `SchemaMismatch`；
- 后端切换不得改变字段顺序、类型、nullable、默认值、domain、geometry type 或 SRS；
- Schema parity 不通过时不得输出首条 Feature。

## 6. 查询和结果顺序

### 6.1 第一版 Query

支持：

- 字段名投影；
- OGR SQL WHERE 属性过滤表达式；
- bbox；
- 属性与 bbox 联合过滤；
- offset、limit；
- cancellation、deadline；
- Native 或 FidAscending 顺序；
- profiling 和 explain。

不纳入 v0.2.0 正式合同：

- JOIN；
- 聚合；
- ORDER BY 任意字段；
- Dataset ExecuteSQL；
- 写 SQL；
- Raster；
- Arrow Stream。

属性表达式默认最大 64 KiB，URI 默认最大 8 KiB。

### 6.2 顺序

- `Native`：保持真正流式和最高性能，不承诺跨后端顺序一致；
- `FidAscending`：先收集和排序 FID，再逐条物化；
- 使用 offset/limit 且要求跨后端结果确定时，必须选择 `FidAscending`；
- FidAscending 的候选 FID 集合单独受内存上限保护；
- 已删除或空洞 row slot 不进入结果，输出只排序实际存在的公开 FID。

## 7. Router 和 fallback 白名单

| 来源/策略 | 行为 |
|---|---|
| 本地 `.gdb` + `Auto` | Schema 与 Query 能力满足时使用 fast，否则按白名单使用 OpenFileGDB |
| 本地 `.gdb` + `FastOnly` | 只使用 fast，能力缺口返回 `Unsupported` |
| 本地 `.gdb` + `GdalOnly` | 只使用官方 OpenFileGDB |
| S3 + `Auto/GdalOnly` | 使用官方 OpenFileGDB，并报告远程一致性 |
| S3 + `FastOnly` | 返回 `UnsupportedSource` |
| 其它 VSI、嵌套压缩或带凭据 URI | v0.2.0 返回 `InvalidUri` |
| FastFileGDB 更新打开 | 明确失败，绝不借 OpenFileGDB 完成写入 |

允许自动 fallback：

- S3/`/vsis3/`；
- 明确的 fast 字段、Query 或 geometry 能力缺口；
- fast 无法表示的 64 位或稀疏 FID；
- fast 不支持的 OGR WHERE；
- 非矩形 OGR 空间过滤；
- 调用方明确允许的本地并发未验证读取。

禁止自动 fallback：

- Invalid request；
- Source not found；
- Permission/authentication/network configuration error；
- Corrupt data；
- Schema mismatch；
- SourceChanged、ReaderExpired；
- Cancelled、DeadlineExceeded；
- ResultLimitExceeded；
- 一般性 fast open/read failure。

GDAL Adapter 的 `GDALOpenEx()` allowed-driver 列表固定为 `OpenFileGDB`，防止
重新进入 `FastFileGDB`。

## 8. Cursor、read_all 和资源门禁

### 8.1 Cursor

`open_cursor()` 返回前完成：

1. URI 和 Query 验证；
2. Schema 与 capability 预检；
3. 后端路由；
4. 后端打开；
5. Schema parity；
6. 首条 Feature 预取或确认 EOF。

Cursor 返回后：

- 后端永久固定；
- 中途失败不再 fallback；
- 已消费 Feature 不撤回；
- 错误通过 `next()` 返回；
- EOF、显式 `close()` 或析构释放 Dataset/Reader；
- cancellation/deadline 只能保证在相邻 GDAL 调用间检查，阻塞中的网络请求是否能
  及时终止取决于部署环境的 GDAL 网络超时。

### 8.2 read_all

`read_all()` 完整物化到临时结果，验证成功后一次发布：

- 默认最多 1,000,000 个 Feature；
- 默认最多 512 MiB 已拥有数据；
- 单个 Feature 默认最多 64 MiB；
- 任一上限触发返回 `ResultLimitExceeded`；
- 不发布部分结果；
- `unlimited=true` 是唯一关闭保护的方式；
- 白名单能力错误可以丢弃临时结果后改用 GDAL；
- 损坏、权限、SourceChanged 等禁止 retry/fallback。

字节统计必须包含：

- 字符串 capacity；
- Binary；
- WKB；
- 字段容器；
- Feature 和索引辅助内存；
- fast 专有 raw bytes。

## 9. 本地与 S3 一致性

### 9.1 本地

```text
稳定本地源 → fast-gdb
外部 GDAL 编辑前销毁全部 Reader/Cursor
GDALClose 后完整重开
```

默认 Writer 活动期间返回 `SourceBusy`。显式允许的 `GdalUnverified` 只能报告
`UnverifiedConcurrentRead`，不得作为一致性证明。

### 9.2 S3

```text
s3://bucket/version-id/data.gdb
    → /vsis3/bucket/version-id/data.gdb
    → 官方 OpenFileGDB read-only
```

正式合同仅适用于读取期间不可覆盖的版本化 prefix：

- 库报告 `ImmutablePrefixAssumed`，不能证明对象目录事务快照；
- 可变 prefix 只有调用方显式放宽策略时才允许；
- 可变 prefix 永远报告 `RemoteUnverified`；
- S3 bucket versioning 本身不等于一个 FileGDB 多对象快照；
- 同一 logical dataset 的新版本必须发布到新 prefix，并使用新 URI 重开；
- 不调用全局 VSI cache clear 破坏其它 Dataset。

MinIO、OSS、COS 和其它 S3-compatible 服务必须单独验证，不能继承 AWS S3 结论。

## 10. GDAL runtime 与 FastFileGDB

### 10.1 GDAL 生命周期

- 进程级 `std::call_once` 执行 GDAL 初始化；
- plugin 注册期间禁止递归调用 `GDALAllRegister()`；
- runtime/plugin 不调用 `GDALDestroyDriverManager()`、`OSRCleanup()`；
- GDAL 全局清理由宿主程序负责；
- 每个 Cursor 拥有独立 GDALDataset；
- 不跨 Cursor 或线程共享 OGRLayer/GDALDataset；
- 每次 GDAL 调用前重置错误状态，调用后立即复制错误；
- 不安装永久全局 error handler。

### 10.2 FastFileGDB driver

插件名与注册函数：

```text
gdal_FastFileGDB
GDALRegister_FastFileGDB()
```

驱动行为：

- 只声明 vector/read-only；
- 普通探测不替换官方 OpenFileGDB；
- 调用方通过 `GDALOpenEx()` allowed-driver 或 `-if FastFileGDB` 显式选择；
- 插件内部复用统一 `Auto` Router；
- S3 或 Query 能力缺口可以使用官方 OpenFileGDB；
- update/create/delete/transaction/raster 明确不支持；
- `GetFeatureCount(force=false)` 和 `GetExtent(force=false)` 不为 S3 强制全扫描；
- `SetAttributeFilter`、`SetSpatialFilter`、`ResetReading` 在下一次 Cursor 创建时重新路由；
- Dataset/Layer metadata 暴露实际 backend、route reason 和 consistency；
- internal OpenFileGDB fallback 最多进入一次，禁止递归。

## 11. GDAL 版本与凭据安全

### 11.1 版本

- 源码兼容下限：GDAL 3.9.3；
- Linux CI 覆盖 3.9、3.10、3.11、3.12、3.13 各自选定的最新补丁；
- macOS/Windows 至少覆盖当前基线和发布目标版本；
- 预编译 plugin 文件名和 manifest 标明精确 GDAL major.minor；
- 同时记录平台、架构、编译器 ABI 和 fast-gdb build ID；
- 不宣称一个 plugin 二进制兼容任意 GDAL；
- 老 GDAL 通过源码测试不等于仍受上游安全维护。

### 11.2 凭据

统一接口不提供：

- Access Key；
- Secret Key；
- Session Token；
- 任意 GDAL key/value 配置表。

凭据由部署环境在 Dataset 打开前提供：

- IAM role；
- 环境变量；
- GDAL 配置文件；
- 宿主预设的 path-specific options。

安全要求：

- 拒绝带 userinfo/query credential 的 URI；
- 默认日志不记录完整远程 URI；
- Error、BackendReport 和 metrics 对 bucket/prefix 脱敏；
- 不把完整 URI、bucket、layer name 作为高基数指标 label；
- 第一版只接受本地路径、`s3://` 和 `/vsis3/`；
- 不向不可信输入开放任意 VSI；
- 文档明确警告不可信 `GDAL_DRIVER_PATH` 和恶意数据风险；
- CPU、内存、FD 和网络隔离仍由宿主进程或沙箱负责。

## 12. Error、BackendReport 和 ConsistencyReport

三类信息严格分离：

```text
Error
    → 操作为什么失败

BackendReport
    → 请求了什么后端、实际选择什么后端、为什么 fallback

ConsistencyReport
    → 结果具有何种一致性证明
```

稳定错误至少包括：

- `InvalidUri`；
- `BackendUnavailable`；
- `Unsupported`；
- `AmbiguousLayer`；
- `SchemaMismatch`；
- `SourceBusy`；
- `SourceChanged`；
- `ReaderExpired`；
- `OpenFailed`；
- `ReadFailed`；
- `Cancelled`；
- `DeadlineExceeded`；
- `ResultLimitExceeded`；
- `RuntimeVersionMismatch`。

每次操作独立携带报告，不提供线程不安全的 `last_report()`。

报告可以包含：

- requested/selected backend；
- route/fallback reason；
- source kind；
- consistency；
- generation；
- Feature 数量；
- 已拥有字节；
- 阶段和总耗时；
- 稳定错误码。

## 13. 实施顺序

### 阶段 0：文档决策同步（完成）

- 用本文同步 ADR-009；
- 同步 README、规划索引、教程、技术边界、测试矩阵、构建矩阵和测试索引；
- 本地实现状态同步为 v0.2.0；
- S3 保持 Experimental / Unverified。

### 阶段 1：统一类型与 fast Adapter（基础实现完成，完整验收待补）

- 实现 Result/Error/report；
- 实现 OGR-compatible Feature；
- 实现 Dataset/Group/Layer/Cursor；
- 实现完整 Group；
- 实现 fast Adapter、Schema 冻结和 FastLayerExtensions；
- 在 GDAL OFF 下完成安装 consumer。

### 阶段 2：共享 runtime 与 GDAL Adapter（基础实现完成，故障矩阵待补）

- 新增动态 `fast_gdb_runtime`；
- 迁移 coordinator/runtime state；
- 实现 GDAL initialization 和 OpenFileGDB Adapter；
- 实现 Auto/FastOnly/GdalOnly；
- 实现 Router/fallback 白名单；
- 实现 Cursor/read_all 双模式和资源门禁。

### 阶段 3：FastFileGDB plugin（基础实现完成，完整 GDAL 合同待补）

- 实现 GDAL Dataset/Group/Layer wrapper；
- 实现显式 driver 注册和 metadata；
- 复用统一 Router；
- 验证无递归、无重复 coordinator、无写入口；
- 完成 runtime/plugin packaging。

### 阶段 4：S3 characterization（待真实 AWS 环境）

- 路由替身测试；
- public/no-sign AWS fixture；
- IAM 保护的真实 AWS fixture；
- immutable prefix；
- 目录枚举、Range Read、403、404、timeout 和中途断网；
- 性能、请求量和字节量；
- 缺少环境时标记 `SKIPPED`，不升级产品状态。

### 阶段 5：发布收口（进行中）

- 完成平台/GDAL 版本矩阵；
- 完成 package consumer；
- 完成 ASan/UBSan/TSan；
- 更新支持矩阵；
- 生成 v0.2.0 release notes 和 rollback 指南。

当前本地证据：macOS/AppleClang、GDAL 3.13 的 478 项全量普通测试、GDAL 3.9.3
的 28 项统一入口定向测试、GDAL OFF 的 117 项测试、安装
consumer、ASan/UBSan、TSan、build-ID 负向装载、fast/GDAL Group、default/domain
parity 输入和插件 filter/reset 合同已通过。GDAL 3.10–3.12、Linux/Windows 和真实 AWS
已有可执行 CI/手动工作流，但尚无本分支远端运行结果；远程 403/404/timeout/disconnect
与性能证据仍待补，不能视为发布门禁通过。

## 14. 测试与验收门禁

### 14.1 接口与语义

- OGR 字段类型、Unset/Null/Value；
- UUID、Binary、日期时间与时区；
- 无几何、NULL、empty、Z/M、曲线、MultiPatch；
- 64 位 FID、零基映射、删除空洞和超范围 fallback；
- Group 嵌套、重名 Layer、系统表开关和 Unicode；
- Schema 冻结与 mismatch；
- Native/FidAscending；
- offset/limit 确定性；
- FastLayerExtensions 强制 FastOnly。

### 14.2 生命周期和故障

- Cursor 首条预取失败；
- 首条发布后失败；
- EOF、cancel、deadline、close、析构；
- `read_all()` 三重上限与 unlimited；
- 单个超大 Binary/Geometry；
- fallback 白名单和禁止列表；
- SourceBusy/SourceChanged/ReaderExpired；
- GDAL 写后完整重开；
- 多 Dataset/Layer/Cursor 并发；
- 同一 Cursor 跨线程负向测试；
- FD、mmap、Dataset 和 plugin 泄漏；
- runtime/plugin build ID mismatch；
- plugin 注册期间无递归和死锁。

### 14.3 GDAL plugin

- 注册、自动加载、显式 allowed-driver；
- 不覆盖官方 OpenFileGDB 普通探测；
- internal fallback 无递归；
- Group、LayerDefn、FID、filter、GetFeature、GetNextFeature、ResetReading；
- update/create/raster/transaction 不支持；
- GDAL 3.9.3 至 3.13.x 源码矩阵；
- 每个预编译 plugin 的 ABI 匹配。

### 14.4 S3 与安全

- URI 规范化；
- illegal bucket、`..`、重复 slash、超长 URI、NUL；
- credential URI 拒绝；
- 日志和 Error 脱敏；
- public 与 IAM fixture；
- immutable prefix；
- directory listing 和 random Range Read；
- 403、404、timeout、disconnect；
- mutable prefix 永远 RemoteUnverified；
- 本地替身不能代替真实 AWS 证据。

### 14.5 性能和资源

分别记录：

- facade Router 开销；
- plugin 与直接统一接口差异；
- local fast；
- local OpenFileGDB；
- S3 首条 Feature 延迟；
- S3 总字节、请求量和吞吐；
- Native/FidAscending 代价；
- read_all 峰值内存；
- 多 Cursor FD 和连接数。

本地和远程结果不得合并成一个“fast-gdb 比 GDAL 快”的结论。

## 15. 发布、状态与回滚

- v0.2.0 同版交付统一接口和 `FastFileGDB`；
- 本地统一接口、Group、plugin、parity、资源门禁、安装 consumer 和 GDAL 版本矩阵
  必须通过；
- 若真实 AWS S3 未验收，v0.2.0 仍可发布本地能力；
- 此时 S3 在接口、CapabilityReport 和文档中保持 `Experimental/Unverified`；
- 真实 AWS 验收通过后，用补丁版本单独升级 S3 支持状态；
- ADR-009 在实现与本地门禁完成后可改为 Accepted；
- 产品矩阵仍单独记录 S3 的 Experimental/Supported 状态；
- 回滚无需数据迁移：禁用 plugin 或使用 `GdalOnly`；
- 现有 linear/hybrid/adaptive 用户不受影响。

## 16. 明确非目标

- fast-gdb 原生 S3 mmap/pread backend；
- S3 Writer；
- 远程事务、对象锁和原子目录发布；
- 完整 GDAL ABI 替代；
- 任意 OGR SQL；
- Raster；
- MinIO、OSS、COS 自动继承支持；
- 跨编译器 C++ ABI；
- 在无真实证据时声明远程正式支持。

## 相关文档

- [ADR-009：统一 FileGDB 访问与 GDAL/S3 路由](../adr/ADR-009-unified-filegdb-routing.md)
- [GDAL 功能对比矩阵](02_GDAL功能对比矩阵.md)
- [组件库设计与使用](../tutorial/01_组件库设计与使用.md)
- [GDAL 写入与 fast-gdb Reader 边界](../technical/07_gdal-write-reader-boundary.md)
- [构建与平台矩阵](../testing/04_构建与平台矩阵.md)
