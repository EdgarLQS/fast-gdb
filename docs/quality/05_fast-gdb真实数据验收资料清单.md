> 状态：Current
> 适用版本：v0.2.0+
> 维护入口：docs/quality/

# 05 — fast-gdb 真实数据验收资料清单

**文档状态**：当前真实数据验收规范
**更新日期**：2026-11-06
/

**适用范围**：`fast_gdb_linear`、`fast_gdb_hybrid` Reader、统一几何模型、WKB/WKT 输出、空间查询和 GDAL 回退链路
**验收目标**：后续新增能力和新增真实数据按本清单准备，避免只验证“能打开文件”，却没有足够证据判断几何语义、FID 映射和查询结果是否正确。

## 1. 使用原则

1. 原始数据必须保留为完整 FileGDB 目录，不能只提供 Shapefile、WKT 或已经被 GDAL 线性化的导出结果。
2. 合成测试只能证明代码路径存在，不能替代 ArcGIS Pro 原生数据的等价验收。
3. 每类数据都要同时验证读取、几何输出、空间查询和必要时的 Hybrid 回退。
4. 真实验收分为“数据覆盖完成”和“逐要素对照完成”；当前版本结论以最终验收报告为准。

## 2. 必需数据清单

下表中的“必需”是后续新增能力或扩大支持范围时应具备的数据类别；已有样本的状态按当前验收报告解释。

| 编号 | 数据类别 | 至少包含的内容 | 当前状态 | 验收重点 |
|---|---|---|---|---|
| D01 | 普通二维基线 | Point、MultiPoint、Polyline、Polygon、MultiPolygon | 已有普通 FileGDB | 图层/字段/要素数、WKB、bbox、属性查询 |
| D02 | Z/M/ZM | Point、Polyline、Polygon 的 Z、M、ZM | 完整外部 `testcurve.gdb` 含 Point/Polyline/Polygon Z、M、ZM 图层，`Curve_Polyline_M_FC` 有 2 条；两条曲线与 GDAL 的类型、bbox、长度合同通过。各非曲线维度坐标和逐类型精度仍待验 | ISO WKB 维度、坐标插值、bbox、长度/面积 |
| D03 | CircularArc | 单弧、多弧、完整圆、跨象限圆弧 | 参数化和完整样本均可打开并含曲线图层；testcurve M 曲线 2/2 通过。跨象限、完整圆及逐类型要素对照仍待验 | 曲线识别、折线化、极值 bbox、长度、空间过滤 |
| D04 | Bezier | 参数化 Bezier 曲线，保留原生控制点和来源信息 | 参数化样本可打开；GDAL 3.13.3 当前将该层输出为线性化 LineString，控制点语义和逐要素 ArcGIS 对照未完成 | 控制点语义、线性化误差和长度 |
| D05 | Ellipse | 完整椭圆、椭圆弧和旋转椭圆 | 参数化样本可打开；GDAL 3.13.3 当前输出为线性化 LineString，长短轴/旋转/闭合的逐要素真值待补 | 长短轴、旋转、起止角、闭合和面积 |
/

| D06 | 混合曲线 | 直线与 CircularArc 混合、多种曲线段组合 | 参数化样本含 GDAL 可识别的直线/CircularArc 复合曲线；Bezier/Ellipse 混合与逐要素语义待验 | part 顺序、连接点、WKB、长度和查询 |
| D07 | 曲线 Polygon | 曲线外环、洞、多面及曲线边界 | 参数化和完整样本均含曲线 Polygon 图层；完整样本另有多洞、岛中岛图层。逐要素环结构、面积和空间查询对照未完成 | 环组织、洞语义、面积、点包含、bbox、空间查询 |
/

| D08 | 多 part | MultiPolyline、MultiPolygon、曲线 MultiPolyline/Polygon | 参数化样本覆盖部分多部件/多面；part 顺序、方向和逐要素输出仍待验 | part 数量、顺序、方向和要素级输出 |
/

| D09 | FID/ObjectID 间断 | ObjectID 不连续、删除后重建、非 1 起始 ID | `acceptance_metadata.gdb/parcels` 已覆盖稀疏 FID 1–8、20–29；外部 `testcurve.gdb/Point_FIDGap` 的 1、2、6、7 已逐要素对照字段、点几何和逻辑行槽。64 位 ObjectID 未覆盖 | fast FID、GDAL FID、ObjectID 三者映射和 Hybrid 回退 |
/

| D10 | 坏拓扑 | 自交、退化环、重复点、重复环、零面积、相切/重叠 | 完整样本含真实坏拓扑层；自交样本由 FastGDB 报 `TouchingRings` 并不输出 WKB，GDAL 则归一为双部件 MultiPolygon；`DegenerateRing_Polygon` 实际被两边读作空/NULL 几何。重复点、重复环和零面积层尚未逐要素验收 | fail-closed、诊断信息、GDAL 回退，不得静默修正 |
| D11 | 空几何 | 空 Point、空线、空 Polygon、空 MultiPatch | 部分合成测试已有 | 读取、WKB、计数和空间查询 |
/

| D12 | 非空 MultiPatch | 多种 part type、Ring/TIN 和 Z/M | 完整样本含 `Multipatch_FC` 图层，但 GDAL 报告 0 条记录；仍只有合成 raw-payload 用例，非空 ArcGIS MultiPatch 未验收 | 完整 part type 和表面拓扑仍明确为能力边界 |
| D13 | 坐标系 | 已知 WKID/WKT 的 FileGDB 图层 | 完整样本由 GDAL 识别 EPSG:4326、4490（CGCS2000）和 4610（Xian 1980）；FastGDB 的几何字段 WKT 可解析，名称、椭球参数和角度单位与 GDAL 相符。完整 datum/axis-order 等价未证。样本 `GDB_SpatialRefs` 为 v3 `ID`/`SRTEXT` 布局，而 `MetadataReader::read_spatial_reference(wkid)` 当前只识别 `wkid` 列 | SRS 读取、层级继承和输出一致性 |
| D14 | 空间索引 | 有效 `.spx`、缺失 `.spx`、损坏 `.spx` | 有效索引有真实样本；缺失/损坏路径以合成安全用例覆盖；真实大型 `.spx` 尚未验收 | 候选查询、精确过滤和回退原因 |
| D15 | 大规模性能 | 至少 10 万要素，推荐 100 万要素；点、线、面各一份 | 当前 100K 基准和 1K/10K/1M/10M 空间矩阵来自性能夹具；未形成 1～20GB 真实生产数据基线 | bbox、属性查询、Hybrid 和内存使用扩展基线 |

## 3. 每份数据必须附带的资料

每个 `.gdb` 数据包至少附带一个 `manifest.md` 或 `manifest.json`，包含：

| 信息 | 要求 |
|---|---|
/

| 数据来源 | ArcGIS Pro/其他工具名称和版本、创建步骤 |
/

| 原始性 | 是否为 ArcGIS 原生曲线；是否经过 GDAL/其他工具导出或线性化 |
| 目录信息 | FileGDB 路径、文件总数、`.gdbtable`、`.gdbtablx`、`.spx`、索引数量 |
| 图层清单 | 图层名、几何类型、字段数、要素数、是否有几何字段 |
| 坐标参考 | WKID、WKT 或明确记录 unknown |
| FID 信息 | GDAL FID、ObjectID、业务主键的对应关系和是否存在间断 |
/

| 曲线信息 | 曲线类型、控制点/端点、起止角、旋转角、part 组成 |
| 拓扑信息 | 外环、洞、岛中岛、坏拓扑类型和预期处理方式 |
| 空间基准 | 图层 extent、测试 bbox、点包含位置及预期命中结果 |
| 数值基准 | 长度、面积、关键坐标、允许误差 |
| 预期行为 | 纯 C++ 成功、显式不支持、Hybrid 回退或 fail-closed |

对于 Bezier、Ellipse 和曲线 Polygon，必须额外说明“原生曲线”还是“已线性化结果”。当前
/

`参数化数据_liqs.gdb` 已确认来自 ArcGIS Pro 3.5，可作为 ArcGIS/GDAL 等价性的原生数据证据。

## 4. 每类数据的验收项目

### 4.1 读取与结构

- GDAL OpenFileGDB 能打开；
- fast-gdb 能扫描目录、系统表、业务表和索引；
- 图层数、字段数、要素数与 GDAL 一致；
/

- FID/ObjectID 分布被完整记录；
- 无截断、错位、`UNKNOWN` 或 `<geom decode error>`。

### 4.2 几何输出

- GeometryModel 类型与来源状态正确；
- ISO WKB 可被 GDAL 重新解析；
- WKB、WKT、空间判断使用同一几何语义；
- Z/M/ZM 维度和坐标值保持正确；
- 曲线无法安全表达时必须明确返回不支持或触发 Hybrid 回退。

### 4.3 空间查询

- `.spx` 只用于候选，不作为最终判断；
- bbox 全范围、局部范围、边界相交和完全不相交均有样例；
- Polygon 洞、岛中岛和曲线边界使用精确过滤；
- 记录命中 FID、执行路径和 fallback 原因。

### 4.4 Hybrid 与映射

/

- fast-gdb 失败/不支持时能触发预期回退；
/

- GDAL Dataset/Layer 缓存路径可用；
- FID 映射不依赖连续 ObjectID 的假设；
- 未命中、无效数据源和映射失败都有明确诊断。

## 5. 非数据发布要求

以下内容不是通过补充 `.gdb` 就能完成，必须单独验收：

1. Windows、Linux、macOS 纯 C++ 构建和测试；
2. Linux GDAL Hybrid、GDAL 曲线后端和产品构建；
3. ASan/UBSan/LSan 几何和解析安全测试；
4. 串行完整 CTest 通过；性能夹具不能因并发共享目录产生误报；
/

5. 10 万/100 万要素性能基线和资源使用记录；
6. 文档、能力矩阵、迁移指南与实际证据一致；
7. 代码、数据快照、测试命令和结果可复现；
/

8. 在所有门禁完成前，不得宣称“ArcGIS/GDAL 全量等价验收完成”。

## 6. 建议的数据交付目录

```text
/

acceptance-data/
/

  <dataset-name>.gdb/
  manifest.md
  layer-inventory.csv
  fid-objectid-mapping.csv
  spatial-cases.csv
  expected-values.csv
  source-notes.md
```

其中：

- `layer-inventory.csv`：图层、几何类型、字段数、要素数、CRS；
- `fid-objectid-mapping.csv`：GDAL FID、fast-gdb FID、ObjectID 和业务主键；
/

- `spatial-cases.csv`：bbox/点包含测试及预期结果；
- `expected-values.csv`：长度、面积、关键坐标和容差；
- `source-notes.md`：ArcGIS Pro 版本、创建过程和原生曲线说明。

## 7. 后续补充项

当前可打开的真实样本包括 `acceptance_metadata.gdb`、`参数化数据_liqs.gdb` 和用户单独提供的
`/Users/edgarlqs/Downloads/testcurve.gdb`。后者有 53 个 `.gdbtable` 与 53 个 `.gdbtablx`，
GDAL 3.13.3 的 OpenFileGDB 可打开；本机 `test_data/gdb/testcurve.gdb` 旧副本仍是独立目录，
不能因外部样本完整就推定它已被修复。新样本已启用 M 曲线、稀疏 32 位 FID、部分坏拓扑和多 CRS
对照，但全部表均为 v3，且 `Multipatch_FC` 没有要素。

现有证据覆盖了普通字段/元数据、稀疏 32 位 FID、M 曲线两条记录和自交/空几何状态；64 位
ObjectID、所有曲线类型逐要素等价、完整坏拓扑矩阵、非空 MultiPatch 语义，以及 1～20GB 真实数据
性能仍待补充。该外部目录没有随附 ArcGIS 创建版本、生成步骤和独立真值清单；后续仍需记录来源并
取得对应真值：

/

1. 为外部 `testcurve.gdb` 补充 ArcGIS Pro/ArcPy 版本、创建步骤、完整 manifest 和逐图层真值；旧的仓库副本如需纳入回归，再单独恢复并校验；
/

2. 补充 64 位稀疏 ObjectID、各类曲线原生真值、非空 MultiPatch、附件和高级对象样本，并分别建立 ArcGIS 原生真值与 GDAL 对照；
3. 1～20GB 真实生产数据以及更大规模数据的读取、过滤、索引查询和内存占用仍需单独实测；
4. 新增 FileGDB 描述符或新增输出语义时，补充对应原生样本和逐要素对照。

当前支持范围内的发布结论见
历史发布验收报告已移除；新增范围必须重新完成本清单对应的测试和对照。
