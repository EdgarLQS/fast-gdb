# fast-gdb 统一访问与 GDAL/S3 路由计划

**状态**：Proposed / design-only

**范围**：统一只读访问入口、FileGDB 后端路由、GDAL/OGR 兼容入口和对象存储边界。

本文只记录设计和实施顺序，不表示当前代码已经提供统一入口、S3 读取或
`FastFileGDB` 驱动。

## 1. 最终设计结论

fast-gdb 不直接把 S3 当作本地 FileGDB 目录读取，也不在核心 Reader 中加入对象存储
适配。统一入口负责根据 URI 和能力选择后端：

```text
fast_gdb::Dataset::open(uri, Auto)
        │
        ├─ 本地 .gdb
        │     └─ FastGdbAdapter
        │
        ├─ s3://... 或 /vsis3/...
        │     └─ GdalOpenFileGdbAdapter
        │
        └─ fast-gdb 不支持的只读能力
              └─ fresh GdalOpenFileGdbAdapter
```

调用方始终使用统一的 `Dataset / Layer / Feature / Cursor` 模型；实际后端、路由原因、
一致性和 fallback 信息通过报告接口暴露。

### 1.1 三种后端策略

```cpp
enum class BackendPreference {
    Auto,
    FastOnly,
    GdalOnly,
};
```

- `Auto`：本地稳定 `.gdb` 优先 fast-gdb；S3 或能力缺口走 GDAL；
- `FastOnly`：不允许 GDAL fallback，S3 或不支持能力直接返回结构化错误；
- `GdalOnly`：所有读取均使用 GDAL/OpenFileGDB。

S3 路由依赖部署的 GDAL `/vsis3/`/VSI 能力、认证、目录枚举和随机读取配置。
MinIO、OSS、COS 或其它对象存储不因本计划自动获得支持，必须另行验证和设计。

## 2. 对外接口方向

新增稳定的 `fast_gdb` 统一接口，避免把当前 `explorgdb` 物理解析器头文件直接冻结为
长期 SDK：

```cpp
namespace fast_gdb {

struct OpenOptions {
    BackendPreference backend = BackendPreference::Auto;
    bool allow_degraded = false;
};

class Dataset {
public:
    static Result<Dataset> open(std::string uri,
                                OpenOptions options = {});

    std::vector<std::string> layer_names() const;
    Result<Layer> open_layer(std::string_view name) const;
    BackendReport backend_report() const;
};

class Layer {
public:
    Result<Feature> read_by_fid(Fid fid) const;
    Result<FeatureCursor> open_cursor(const Query& query = {}) const;
    CapabilityReport capabilities() const;
};

}  // namespace fast_gdb
```

第一版接口必须满足：

- `Feature` 和 `Geometry` 返回拥有型数据，不暴露 mmap、`FieldRef` 或 GDAL 借用指针；
- `FeatureCursor` 的后端在开始输出前确定；
- 已输出部分结果后不得静默切换后端；
- `BackendReport` 区分 `FastGdb`、`GdalOpenFileGDB`、`SourceBusy`、`ReaderExpired`、
  `UnverifiedConcurrentRead` 和失败原因；
- 统一接口保持 Reader-only，不增加 Writer、事务或在线发布能力。

## 3. GDAL/OGR 兼容入口

为已有 GDAL/OGR 程序提供可选的 `FastFileGDB` 只读驱动：

```text
GDAL/OGR application
        ↓
FastFileGDB driver
        ↓
FastGdbAdapter
        ↓
fast-gdb Reader / QueryEngine / FeatureCursor
```

驱动使用独立名称，不替换官方 `OpenFileGDB` 的注册和探测优先级。驱动只承诺：

- 只读打开；
- Layer/schema；
- FID 读取；
- 顺序 Cursor；
- bbox 和明确支持的属性过滤；
- 拥有型 OGR Feature/Geometry 转换。

更新、创建、Schema 编辑、事务、复杂 SQL、Raster 和未验收的复杂几何仍返回明确的
不支持结果或由上层统一 Router 选择 GDAL。

## 4. fast-gdb 扩展接口

标准读取接口保持后端中立；fast-gdb 特有能力通过可选扩展报告提供：

- `explain(query)` 和实际执行路径；
- `.spx/.atx` 是否命中、候选数和复核数；
- 字段投影、批量 FID、WKB-first；
- 查询阶段耗时；
- 曲线/MultiPatch 降级与 fallback 原因；
- 当前后端、generation 和一致性状态。

扩展不得暴露 `GDALDataset*`、`OGRLayer*`、内部 mmap 地址或旧代 Cursor。

## 5. 读写与对象存储边界

### 5.1 本地 FileGDB

```text
稳定本地源 → fast-gdb
外部 GDAL 编辑前关闭所有 Reader
GDALClose 后完整重开 Reader
```

### 5.2 S3 对象存储

```text
s3://bucket/path.gdb
    → 转换为部署环境可用的 GDAL VSI 路径
    → GDAL OpenFileGDB read-only
    → 完整物化统一 Feature
    → 关闭 GDAL Dataset
```

当前不把 S3 写入、在线事务、对象目录原子发布或远程锁纳入范围。需要更新时，业务层
负责本地 staging、完整验证和版本发布；fast-gdb 不对远程写入建立正确性合同。

## 6. 实施阶段

### 阶段 0：文档与决策冻结

- 保留当前 `linear/hybrid/adaptive` 产品和 Reader-only 合同；
- 新增本计划与 Proposed ADR；
- 全部现行入口明确 S3 是未来路由目标，不是当前支持能力；
- 历史 Writer/usegdal 文档继续归档，不恢复为产品入口。

### 阶段 1：source-neutral facade

- 定义 `Dataset / Layer / Feature / Cursor / Query / Error`；
- 先接入现有本地 fast-gdb Reader；
- 建立字段、NULL、时间、Binary、FID、WKB 和生命周期合同；
- 不修改底层 FileGDB 解析器和当前安装 target。

### 阶段 2：GDAL read-only Adapter

- 将 GDAL/OpenFileGDB 结果转换为统一拥有型 Feature；
- 建立 fresh open/materialize/close 的资源合同；
- 增加 `Auto/FastOnly/GdalOnly` 和 `BackendReport`；
- fallback 只允许发生在 Cursor 输出前。

### 阶段 3：URI 路由与 S3 characterization

- 本地路径、`s3://`、`/vsis3/` 分类；
- 使用测试替身验证路由决策，不先宣称 S3 端到端支持；
- 在具备凭据和真实对象布局的 GDAL 环境中验证目录枚举、随机读取、错误和性能；
- 缺少环境时标记 `SKIPPED`，不使用本地测试替代远程证据。

### 阶段 4：FastFileGDB GDAL 驱动与扩展

- 实现最小只读驱动；
- 验证 FID、字段/NULL、几何所有权、过滤器和 `GDALClose`；
- 通过稳定的扩展入口提供 fast-gdb 诊断；
- 不把驱动适配层反向依赖到 Reader 核心。

## 7. 验收门禁

必须分别验证：

1. 本地 `.gdb` 在 `Auto` 下使用 fast-gdb；
2. S3 URI 在配置有效时使用 GDAL OpenFileGDB；
3. `FastOnly` 不会静默调用 GDAL；
4. `GdalOnly` 不会创建 fast-gdb Reader；
5. fast 和 GDAL 的字段、NULL、时间、Binary、FID、WKB、空结果和错误分类一致；
6. 源变化、`SourceBusy`、`ReaderExpired` 和 fallback 不发布部分结果；
7. 独立 Reader 并发、GDAL 写后重开和 Adaptive 现有门禁不回退；
8. linear 安装仍可在 GDAL OFF 下构建和消费。

性能结论必须区分本地 fast-gdb、GDAL 本地读取和 GDAL S3 远程读取，不能混合为一个
“fast-gdb 比 GDAL 快”的结论。

## 8. 非目标

- 不实现 fast-gdb 原生 S3 mmap/pread 后端；
- 不把 fast-gdb 伪装成完整 GDAL ABI 替代品；
- 不复制完整 OGRLayer/OGRFeature 生态；
- 不恢复 Writer、事务、远程在线更新或对象存储锁；
- 不在无验证数据时宣称支持 MinIO、OSS、COS 或任意 S3 兼容服务。

## 相关文档

- [ADR-009：统一 FileGDB 访问与 GDAL/S3 路由](../adr/ADR-009-unified-filegdb-routing.md)
- [GDAL 功能对比矩阵](02_GDAL功能对比矩阵.md)
- [组件库设计与使用](../tutorial/01_组件库设计与使用.md)
- [GDAL 写入与 fast-gdb Reader 边界](../technical/07_gdal-write-reader-boundary.md)
