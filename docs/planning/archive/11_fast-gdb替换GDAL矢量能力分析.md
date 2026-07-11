# 11 — fast-gdb 替换 GDAL 矢量能力分析

**建立日期**：2026-07-10  
**文档状态**：架构决策参考  
**适用范围**：`explorgdb reader` 替代 GDAL OpenFileGDB 矢量只读路径

## 1. 结论

`fast_gdb` 适合替代 GDAL 的一部分 FileGDB 高频只读路径，但不适合替代 GDAL 整套矢量生态。

推荐定位：

> fast-gdb 负责性能敏感、格式固定、能力边界可控的 FileGDB 矢量读取；GDAL 保留多格式、重投影、复杂 SQL、完整几何语义和兼容性职责。

不建议以“完全替代 GDAL”为当前目标。

## 2. 替换 GDAL 的优势

### 2.1 性能路径更短

fast-gdb 直接解析 `.gdbtable`、`.gdbtablx`、`.spx` 和 `.atx`，避免构造完整的 OGRFeature、OGRGeometry 和字段定义对象。

适合优化：

- 全表顺序扫描；
- 按 FID 高频读取；
- bbox 空间查询；
- 属性索引查询；
- 批量字段导出；
- 延迟几何解码。

### 2.2 内存和对象开销较低

纯二进制 reader 可以按需读取字段和几何，减少堆分配、临时对象和通用抽象层开销，更适合百万级和千万级只读数据处理。

### 2.3 查询执行路径透明

`QueryEngine` 可以显式暴露：

- 是否使用 `.spx`；
- 是否使用 `.atx`；
- 是否降级为顺序扫描；
- 曲线记录是否被跳过；
- fallback reason。

这种可观测性适合生产排障和性能分析。

### 2.4 能力边界可控

项目采用以下原则：

> 不能可靠表达的数据必须明确 degraded 或 unsupported，不生成看似合法但语义错误的结果。

例如：

- 曲线记录返回 `UNSUPPORTED_CURVE_GEOMETRY`；
- 曲线空间过滤 fail closed；
- MultiPatch 明确标记 degraded；
- Raster 只检测字段，不伪装成像素读取支持。

### 2.5 可减少生产运行时依赖

explorgdb reader 核心为纯 C++，可避免在只读热路径中依赖完整 GDAL/PROJ/GEOS 运行时。GDAL 仍可保留为测试 oracle、格式转换工具或显式兼容路径。

### 2.6 更容易进行业务定制

可针对业务提供更小的接口：

```cpp
scan(...)
read_by_fid(...)
query_bbox(...)
query_attribute(...)
read_selected_fields(...)
```

不必复制完整 OGRLayer 生命周期和状态模型。

## 3. 替换 GDAL 的劣势

### 3.1 只覆盖 FileGDB

GDAL 提供统一的多格式矢量访问能力。fast-gdb 当前只处理 FileGDB，因此全面替换会导致上层同时维护 fast-gdb 和 GDAL 两套路径。

### 3.2 几何语义尚不完整

当前边界包括：

- CircularArc、Bezier、EllipticArc 不支持标准输出；
- MultiPatch 不保留完整 part type 和表面拓扑；
- GeneralPoint / GeneralMultiPoint 完整 decode 进入 v3；
- Point/MultiPoint 的部分 bbox 和空间过滤一致性仍需 v3 修正。

因此复杂 ESRI 几何仍应优先使用 GDAL 或明确拒绝。

### 3.3 不提供完整空间参考能力

fast-gdb 可以读取 SRS 元数据，但不提供完整重投影、datum transformation、vertical CRS 和轴顺序处理。涉及坐标转换时仍需要 GDAL/PROJ。

### 3.4 SQL 能力有限

当前只支持明确的 WHERE 子集，不支持完整 SQL、JOIN、聚合、函数和子查询。

### 3.5 缺少成熟 API 生态

GDAL 已有稳定的 C/C++、Python、Java 和 C# 接口。自研 reader 需要自行承担 API 稳定性、线程安全、错误模型、绑定、版本兼容和维护成本。

### 3.6 真实数据覆盖仍不足

当前本地功能 runner 为：

```text
401 passed / 11 skipped / 0 failed
```

但普通真实 FileGDB 和真实曲线 FileGDB 尚未完成 PASSED 验收。合成测试全绿不能等价为所有生产 FileGDB 均兼容。

### 3.7 长期维护成本高

FileGDB 是复杂专有格式。后续需要持续处理：

- ArcGIS 版本差异；
- 新字段和新几何标志；
- 老 schema；
- 异常或损坏文件；
- Annotation、Dimension、Raster；
- 关系、拓扑、网络等高级数据集。

### 3.8 容错积累不如 GDAL

GDAL 已积累大量真实世界兼容处理。fast-gdb 必须持续加强长度校验、越界检查、varint 上限、数量合理性和错误上下文，避免脏数据造成崩溃或静默错误。

## 4. 适合替换的场景

| 场景 | 建议 |
|------|------|
| 大型 FileGDB 全表扫描 | 优先 fast-gdb |
| 按 FID 高频读取 | 优先 fast-gdb |
| bbox / `.spx` 查询 | 优先 fast-gdb |
| `.atx` 属性索引查询 | 优先 fast-gdb |
| 批量字段导出和只读 ETL | 适合 fast-gdb |
| 服务端只读 API | capability 检查后使用 fast-gdb |
| 数据来源和几何类型可控 | 适合 fast-gdb |
| 不需要重投影的统计和质检 | 适合 fast-gdb |

## 5. 不适合直接替换的场景

| 场景 | 原因 |
|------|------|
| 多格式统一访问 | 仍需要 GDAL 驱动生态 |
| 坐标重投影 | 需要 GDAL/PROJ |
| 原生曲线必须保留 | fast-gdb 当前 unsupported |
| MultiPatch 必须完整还原 | 当前 degraded |
| Annotation / Dimension | 当前未支持 |
| Raster 像素读取 | 当前未支持 |
| 复杂 SQL / JOIN / 聚合 | 当前未支持 |
| 数据来源完全不可控 | 兼容和容错风险较高 |
| 写入后要求 ArcGIS/GDAL 完整兼容 | writer 系统表仍未完成 |

## 6. 推荐架构

不建议隐式 fallback，也不建议删除 GDAL。推荐显式双路径：

```text
业务请求
  │
  ├─ FileGDB + 只读 + capability 可接受
  │      └─ fast-gdb 高性能路径
  │
  ├─ 曲线 / MultiPatch 完整语义 / 特殊数据集
  │      └─ 明确拒绝，或显式选择 GDAL 兼容路径
  │
  └─ 其他格式 / 重投影 / 完整 SQL / 格式转换
         └─ GDAL
```

建议接口：

```cpp
auto report = inspect_layer(path, layer);

if (report.supports_fast_path()) {
    return fast_gdb_read(...);
}

if (request.allow_gdal_compatibility_path) {
    return gdal_read(...);
}

return error(report.unsupported_reasons);
```

GDAL 兼容路径必须由调用方显式允许，避免静默切换掩盖能力缺口。

## 7. 分阶段替代建议

### 阶段 1：内部批处理

优先替换：

- scan；
- FID；
- bbox；
- 属性索引；
- schema；
- SRS 元数据。

这是风险最低、最容易做 GDAL 对照验证的阶段。

### 阶段 2：服务端只读主路径

在数据集白名单和 capability 检查基础上，将 fast-gdb 用作默认热路径。

必须具备：

- GDAL oracle 对照；
- 真实数据样本矩阵；
- 错误和 fallback 指标；
- 无静默兼容路径；
- 可快速切回 GDAL 的配置开关。

### 阶段 3：扩大几何覆盖

完成 v3 后，再逐步扩大：

- GeneralPoint；
- GeneralMultiPoint；
- Point/MultiPoint bbox 精确一致性；
- 更多真实 General 几何；
- MultiPatch 真实样本。

### 暂不建议

- 删除 GDAL 依赖；
- 宣称完整替代 OGR；
- 所有 FileGDB 无条件进入 fast-gdb；
- 复制完整 OGRLayer API；
- 同时扩曲线、Raster、Annotation、writer 和完整 SQL。

## 8. 决策标准

只有同时满足以下条件，某个图层才建议进入 fast-gdb 默认路径：

1. 数据格式为 FileGDB。
2. 操作为只读。
3. capability 没有 Unsupported。
4. Degraded 项符合业务容忍范围。
5. 不需要重投影和完整 SQL。
6. 真实样本已与 GDAL 对照验证。
7. 失败时有明确错误和可观测信息。

## 9. 最终定位

fast-gdb 的长期价值是：

> 成为 FileGDB 高频只读矢量路径的专用高性能引擎，而不是重新实现一套完整 GDAL。

GDAL 继续承担多格式、投影、复杂语义、兼容性和工具生态；fast-gdb 则聚焦可验证、可观测、性能敏感的 FileGDB reader。
