> 状态：Active / Partial evidence
> 适用范围：S3.5 历史版本与压缩格式
> 更新日期：2026-11-07
> 权威执行入口：[完整只读能力与 GDAL 对照升级计划](22_完整只读能力与GDAL对照升级计划.md)

# FileGDB 历史版本与压缩格式验收矩阵

## 1. 目的与判定规则

本矩阵把 ArcGIS 产品版本、FileGDB 物理表版本、索引版本和数据压缩分别记录。它们不是同一层编号；读到一种 `.gdbtable` 版本不能证明该 ArcGIS 产品代际的全部 Schema、索引和压缩配置都兼容。

状态含义：

- **真实样本通过**：有 ArcGIS 写出的完整样本、独立真值和可复现结果。
- **合成/参考通过**：代码路径和边界有合成夹具或 GDAL 参考夹具验证；不证明 ArcGIS 写入行为。
- **部分实现**：已覆盖部分布局，但仍有未知分支或语义未验证。
- **待样本**：没有满足验收条件的样本；不得算作通过。
- **不支持**：当前 Reader 明确不能解码，必须 fail closed 或报告 Unsupported。

GDAL 是重要的独立实现对照，但当 GDAL 文档明确记录格式限制时，不以 OpenFileGDB 的输出作为该格式的正确性真值。ArcGIS 官方文档描述产品版本和语义；字节布局仍需源码交叉检查和完整数据样本。

## 2. 当前覆盖矩阵

| 维度 | 格式/样本类别 | fast-gdb 当前状态 | GDAL/外部对照 | 证据与未完成条件 |
|---|---|---|---|---|
| ArcGIS 产品代际 | ArcGIS 9.x FileGDB | **官方历史参考库字段对照通过，ArcGIS 独立真值待补** | GDAL v3.10.3 的 92/93 命名夹具各 22 层、453 行、2,056 字段值完整对照通过 | 两份完整归档及每文件 SHA-256 已记录；创建产品原始日志及 ArcGIS 导出真值未取得，几何和索引合同未验收。仍需 ArcGIS 9.3.1 原始样本及升级前后对照，见 [历史参考库记录](evidence/2026-11-07-S3.5-gdal-historical-attributes.md)。 |
| ArcGIS 产品代际 | ArcGIS 10+ | **部分验证** | OpenFileGDB 支持 10+ 创建的 GDB | 仓库中的 ArcGIS 真实样本观察到 v3 表；缺少按 ArcGIS/Pro 版本分层的完整语料。GDAL 文档指出 FileGDB 格式版本自 10.1 后未变化，但这不证明所有新字段和新对象语义一致。 |
| `.gdbtable` / `.gdbtablx` | v3 表及行偏移索引 | **真实样本通过（当前语料范围）** | GDAL v3 读取路径可作为第二实现对照 | 本地历史扫描记录有 301 个 `.gdbtable` 和 304 个 `.gdbtablx`，均为 v3；真实 ArcGIS 主样本、常规读取和 v3 稀疏压缩块映射测试通过。尚不能据此覆盖全部 ArcGIS 10+ 写入配置。 |
| `.gdbtable` / `.gdbtablx` | v4 密集、无位图布局 | **合成/参考通过** | GDAL 3.13.0 与 3.13.3 均读取参考夹具 | `synthetic_v4_dense_gdal_reference.gdb` 为 GDAL 生成后转换目标表头/索引 trailer 的四行夹具，不是 ArcGIS 写出的 v4 样本。真实 v4 仍待补。 |
| `.gdbtable` / `.gdbtablx` | v4 已识别位图布局 | **部分实现** | 与 GDAL 已识别布局比对 | 合成测试验证位图标记、逻辑块到紧凑物理块映射；未知位图标记/大小和不支持的删除记录布局 fail closed。需要 ArcGIS 高位稀疏 ObjectID、删除记录和完整查询样本。 |
| `.gdbtable` / `.gdbtablx` | 未知表版本或未知 v4 位图 | **不支持，fail closed** | 不猜测偏移和 FID 映射 | 代码只接受表版本 3/4 和已识别索引布局。新版本或不匹配表/索引版本应报告解析失败；需要新增版本证据后再扩展。 |
| ObjectID | 32 位常规映射 | **真实样本通过（当前语料范围）** | 对比 GDAL FID 与字段值 | 完整高覆盖数据集仍需逐层全 FID digest；现有状态不声明跨所有版本绝对一致。 |
| ObjectID | v4 64 位、密集低位 ID | **合成/参考通过** | GDAL dense v4 参考读回 | 四行密集 v4 夹具和 API 读取通过。 |
| ObjectID | 稀疏/高位/删除记录 v4 | **部分实现，验收待样本** | OpenFileGDB 官方明确称稀疏 64 位 OBJECTID 支持只读且不完整，可能输出不忠实 ID | 当前不得用 GDAL 输出单独确定行槽到 ObjectID 的映射。需 ArcGIS 原始 ID、删除/重排状态及直接读取、索引查询、全表扫描的逐项真值。 |
| SPX / ATX | v1，4 KiB 页和 32 位 ID | **实现；以 v3 真实语料覆盖为主** | GDAL v1 parser 对照 | 合成树形索引、损坏回退与现有真实索引覆盖已通过；旧版 ArcGIS 独立样本仍未分层。 |
| SPX / ATX | v2，64 KiB 页和 64 位 ID | **合成通过** | GDAL v2 页/trailer 定义对照 | 合成测试覆盖 v2 页解析、高位索引键和分支页查询；没有 ArcGIS-authored v2 索引样本，不证明 ArcGIS writer parity。 |
| 压缩数据 | SDC（Smart Data Compression） | **不支持** | GDAL OpenFileGDB 和 FileGDB SDK 驱动文档均记录限制 | 当前 fast-gdb 没有 SDC 解压器，也没有完整 SDC 样本。应显式 Unsupported，不按普通 v3/v4 表解析。若将来实现，需独立格式依据、ArcGIS 原始样本和解压后字段/几何真值。 |
| 压缩数据 | CDF（Compressed Data Format） | **不支持** | OpenFileGDB 不支持；GDAL FileGDB SDK 驱动文档要求 SDK 1.4 或更高 | 当前测试构建使用 OpenFileGDB，未配置或验收 proprietary FileGDB SDK fallback。即使外部 SDK 可读，也不能把它当作 fast-gdb 原生支持。 |
| 记录块映射 | v3/v4 tablx 稀疏块位图 | **部分实现，按布局分别验收** | GDAL 作为实现对照，不作为未知位图真值 | v3 真实/合成映射已有证据；v4 仅识别已知布局，未知位图、删除状态和稀疏 ObjectID 仍 fail closed 或待样本。此块映射位图不等同于 SDC/CDF 数据压缩。 |
| 存储配置 | 几何/BLOB inline 与 out-of-row 等配置关键字差异 | **待样本** | Esri 公开配置关键字会影响存储行为；GDAL 仅能证明实际读回结果 | 当前样本没有覆盖所有关键字与大 payload 组合。需由 ArcGIS 创建配置矩阵，并记录逻辑值、物理载荷位置、预算行为及 GDAL 对照。 |

## 3. 术语边界

- **ArcGIS 产品版本**（如 9.3.1、10.x、ArcGIS Pro）用于描述创建/升级客户端与产品能力。
- **`.gdbtable` 版本**描述表文件物理头；本实现当前识别 v3/v4。它不是 ArcGIS Pro 的版本号。
- **`.gdbtablx`、SPX、ATX 版本**各有自身页/偏移布局，不能从表版本推断。
- **SDC/CDF**是压缩数据路径；**tablx 稀疏块位图**是行槽到紧凑偏移块的映射，两者不能混称为同一种压缩。
- ArcGIS 文件地理数据库的产品版本兼容说明与内部字节格式规范不同。Esri 文档说明旧于 ArcGIS 10 的 FileGDB 可能需要升级，且版本标识自 10.1 后未改变；这不构成全部历史布局的公开二进制规范。

## 4. 闭环所需数据

最低可验收语料：

1. ArcGIS 9.3.1 原始 FileGDB，升级前先复制并生成文件 manifest；
2. ArcGIS 10.0、10.1 和至少一个当前 ArcGIS Pro 写出的普通 v3 GDB；
3. ArcGIS 原生 v4、稀疏且包含高位 64 位 ObjectID 的完整 GDB，另含删除记录；
4. ArcGIS 创建的 SDC 与 CDF 数据，各一份；如拿不到，保留为明确不支持，不伪造夹具；
5. 至少一份使用不同几何/BLOB 配置关键字并含 out-of-row 载荷的 GDB；
6. 每份数据对应 ArcGIS 导出 schema、全量 FID/ObjectID、规范化字段 digest、几何摘要和 GDAL 版本/驱动日志。

执行时对每份样本分别跑 fast-gdb FastOnly、项目实际可用的 GDAL driver 和 ArcGIS 原生导出。记录完整打开、目录/schema、逐行读取、索引查询、空间过滤、刷新重开、文件 manifest 和错误诊断；每个跳过项记录原因，不计为通过。未知/损坏布局必须 fail closed，不能返回看似成功的空表或重编号 ObjectID。

## 5. 一手来源与项目证据

- [GDAL OpenFileGDB 矢量驱动](https://gdal.org/en/stable/drivers/vector/openfilegdb.html)：9.x/10+ 读取范围、SDC/CDF 限制、稀疏 64 位 ObjectID 限制。
- [GDAL FileGDB SDK 驱动](https://gdal.org/en/stable/drivers/vector/filegdb.html)：CDF 所需 SDK 版本与 SDC 限制。
- [Esri：升级 FileGDB](https://pro.arcgis.com/en/pro-app/3.5/help/data/geodatabases/manage-file-gdb/upgrade-a-file-geodatabase.htm)：旧于 ArcGIS 10 的库可能需要升级。
- [Esri：客户端与地理数据库兼容性](https://pro.arcgis.com/en/pro-app/3.2/help/data/geodatabases/overview/client-geodatabase-compatibility.htm)：FileGDB 产品版本及新字段兼容边界。
- [Esri：FileGDB 配置关键字](https://pro.arcgis.com/en/pro-app/latest/help/data/geodatabases/overview/configuration-keywords-for-file-geodatabases.htm)：配置关键字与存储行为。
- 本仓库 [S3.4 边界记录](evidence/2026-11-06-S3.4-objectid-boundary.md) 与 [dense v4 参考证据](evidence/2026-11-06-S3.4-dense-v4-baseline.md)：当前真实/合成版本和 ObjectID 限制。
- 本轮 [S3.5 初始证据记录](evidence/2026-11-06-S3.5-historical-format-matrix.md)：解析器分支检查与 68 项表/索引回归结果。

## 当前结论

S3.5 的矩阵与验收条件已建立，但历史 ArcGIS 版本、ArcGIS-authored v4、稀疏高位 ObjectID、SDC/CDF 和配置关键字语料仍不齐。故 S3.5 仍为部分完成；当前只读产品合同应对 SDC/CDF 和未知布局返回明确不支持，不宣称“完全解析所有历史 FileGDB”。
