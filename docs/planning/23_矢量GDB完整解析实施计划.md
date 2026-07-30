# 矢量 FileGDB 完整解析实施计划

## 1. 目标与边界

本文定义 Raster 之外的矢量 FileGDB 完整只读解析路线。目标不是“可以打开文件”，而是：

- 物理记录、字段值、NULL、Binary、XML、UUID、日期时间均可读取；
- Domain、Subtype、Relationship Class、Feature Dataset 和空间参考可形成完整结构化结果；
- 普通几何、曲线和 MultiPatch 具有明确的完整、降级或不支持语义；
- 无法解析的输入 fail closed，不返回伪造的部分成功结果；
- 结果可以通过 ArcGIS 原生数据、原始 payload 和逐字段 digest 验证。

本计划明确不包含：

- Raster 像素读取；
- fast-gdb Writer、事务、Recovery 和在线副本切换；
- 共享同一 Reader/Layer/QueryEngine 的线程安全；
- 完整 SQL、JOIN、聚合和子查询引擎；
- 未协调外部 Writer 的强一致性保证。

当前 Windows 验收提交 `71a053f52f068c71717d009cc8e2998b17e1ca41` 作为基线。它证明了当前 Reader-only 矢量核心和基础元数据能力，但不等同于所有 FileGDB 版本和对象均已完整解析。

## 2. 结果合同

所有读取结果统一归入以下类别：

| 状态 | 含义 |
|---|---|
| `LosslessPhysicalRead` | 原始系统表、业务记录、字段和 XML/Binary payload 均可取出 |
| `StructuredMetadataRead` | Domain、Subtype、Relationship、Dataset 等结构化解析成功 |
| `SemanticGeometryRead` | 几何语义和 WKB 输出满足当前合同 |
| `DegradedWithRawPayload` | 高级语义暂不完整，但原始数据和诊断仍可取得 |
| `UnsupportedAndDiagnosed` | 明确不支持、返回原因，未伪装成成功 |

系统表缺失、系统表为空、XML 损坏、未知结构和解析异常必须使用不同诊断，不能统一返回空集合。

## 3. 本机可以完成的工作

以下工作不依赖 Windows 专有环境，可在当前 macOS 工作区完成代码、单元测试和现有 GDB 回归。

### P0：公共元数据模型

- 扩展 `MetadataReadResult`，增加 Workspace Domains、完整 Relationship Class Definition、Feature Dataset Definition、成员关系、Subtype 和结构化诊断。
- 增加高级元数据审计入口，提供系统表名、FID、字段定义、原始 XML/Binary 和未识别字段。
- 保留现有 `metadata_snapshot()`、`RelationshipSummary` 和 `MetadataReader` 兼容入口。
- `read_metadata()` 不再丢失完整关系定义，也不再把缺失系统表误报为空结果。

当前公开快照的字段范围见 `src/edgar/explorgdb/reader/reader.h`；当前 `read_metadata()` 的组装逻辑见 `src/edgar/explorgdb/reader/reader.cpp`。

### P0：无损字段和系统表读取

- 读取并保存字段默认值。目前字段描述解析只跳过默认值字节，见 `src/edgar/explorgdb/reader/gdb_table.cpp`。
- XML 保留原文，Binary 保留原始字节，并同时提供规范化摘要。
- 对 `GDB_Items`、`GDB_ItemRelationships`、`GDB_ItemRelationshipTypes`、`GDB_ItemTypes`、`GDB_Datasets` 和 `GDB_DatasetRelationships` 建立逐表 digest。
- 增加缺失、截断、损坏 XML、损坏 Binary 和未知列的 fail-closed 测试。

### P0：FID 和边界行为

- 本机完成 FID 类型升级设计、索引边界检查、稀疏 FID、删除槽、最大值和非法 FID 测试。
- 若采用完整 64-bit FID，端到端修改 `FeatureRecord`、QueryRequest、Cursor、`.gdbtablx`、`.spx`、`.atx` 和 Hybrid 映射。
- 在未获得真实 64-bit 数据前，超范围数据继续 fail closed，不能静默转换为 `uint32_t`。

### P1：本机测试和文档

- 建立元数据 conformance runner、逐记录 digest 和逐字段 digest。
- 对现有 `acceptance_metadata.gdb` 回归 Domain、Relationship、Feature Dataset、空间参考、字段类型、NULL、Binary 和稀疏 FID。
- 补齐普通 XML、空 XML、缺失系统表和损坏系统表的测试 fixture。
- 同步 Reader README、测试索引、发布说明和未完成任务清单。
- 保持 `linear` 安装 consumer 在 GDAL OFF 下通过。

## 4. 必须补充 Windows/ArcGIS 数据的工作

以下工作不能仅凭本机合成测试形成完整产品证据，需要 Windows 上的 ArcGIS Pro/ArcPy 数据或 Windows 编译运行结果。

### W1：丰富元数据

需要生成 Workspace、Feature Dataset 和图层具有非空标题、摘要、标签、限制条件、Documentation/ItemInfo XML 的 GDB，并逐项对照 XML 原文和结构化字段。

当前 `metadata-expected.json` 中标题、摘要和标签为空，因此现有数据只能证明系统表结构，不足以证明丰富元数据完整解析。

### W2：Subtype 和完整关系类

需要 Windows ArcGIS 原生数据覆盖：

- Subtype 字段、多个 code/name；
- 每个 Subtype 的 Domain 和默认值；
- Simple、Composite、1:1、1:M Relationship Class；
- GUID/GlobalID 关系键；
- Notification、Containment、Primary/Foreign Key；
- 带属性 Relationship Class，若当前 ArcGIS 版本不支持，必须换版本或明确记录为未验收能力。

### W3：曲线和 MultiPatch

需要 ArcGIS 原生数据覆盖：

- CircularArc、Bezier、EllipticArc；
- 曲线 Polyline/Polygon、洞、岛中岛和 multipart；
- MultiPatch part type、Z/M、TIN、Ring 和表面拓扑。

曲线仍以 WKB-first 为正式输出，但必须记录线性化容差、源曲线类型并保留原始 geometry payload。MultiPatch 在完整 part type 和拓扑未完成前，只能标记为 `DegradedWithRawPayload`。

### W4：版本和 Windows 运行证据

需要在 Windows 上独立验证：

- MSVC Release/Debug 构建；
- Windows mmap、文件句柄、路径和 Unicode；
- ArcGIS Pro 多版本生成的 GDB；
- v3/v4 `.gdbtable/.gdbtablx`；
- 多个 GDAL 版本的创建、修改、关闭和重开；
- 2/4/8 个独立 Reader 并发；
- ASan/UBSan，TSan 若环境可用则单独记录。

macOS 结果只能作为开发证据，不能代替 Windows 或多版本验收。

## 5. 实施顺序

1. 本机扩展元数据公开模型和错误分类。
2. 本机修复默认值、XML/Binary 原始保留和系统表审计。
3. 本机建立 conformance runner、digest 和损坏输入测试。
4. 使用现有 `acceptance_metadata.gdb` 完成基础回归。
5. 在 Windows 生成丰富 XML、Subtype、完整关系类和曲线/MultiPatch 数据。
6. 本机先解析新增数据，Windows 再运行同一测试集并归档日志。
7. 补多版本 GDB、64-bit 边界和跨 GDAL 版本证据。
8. 更新发布矩阵和“完整解析”声明。

## 6. 验收门槛

### 本机阶段

- 公共元数据模型、错误模型和审计入口完成；
- 默认值、XML、Binary 不丢失；
- 现有 ArcGIS 数据的字段、NULL、Domain、Relationship、Dataset 和空间参考不回归；
- 损坏输入全部 fail closed；
- `linear` 安装面完全无 GDAL。

### Windows 数据阶段

- Rich Metadata、Subtype、Relationship Class 和 Dataset 层级逐项对照通过；
- 曲线和 MultiPatch 有完整结果，或有 raw payload 加明确 degraded 诊断；
- v3/v4 和目标 ArcGIS 版本通过；
- Windows 构建、安装包、GDAL 写后重开和独立 Reader 并发通过；
- 目标测试不再存在未解释的 `SKIPPED`。

只有本机实现和 Windows 数据验收均通过，才能声明：

> fast-gdb 已完成 Raster 之外目标矢量 FileGDB 的完整只读解析。
