# 13 — fast-gdb 真实数据验收资料清单

**更新日期**：2026-07-13
**适用范围**：`fast_gdb_linear`、`fast_gdb_hybrid` Reader、统一几何模型、WKB/WKT 输出、空间查询和 GDAL 回退链路
**验收目标**：后续所有真实数据按本清单准备，避免只验证“能打开文件”，却没有足够证据判断几何语义、FID 映射和查询结果是否正确。

## 1. 使用原则

1. 原始数据必须保留为完整 FileGDB 目录，不能只提供 Shapefile、WKT 或已经被 GDAL 线性化的导出结果。
2. 合成测试只能证明代码路径存在，不能替代 ArcGIS Pro 原生数据的等价验收。
3. 每类数据都要同时验证读取、几何输出、空间查询和必要时的 Hybrid 回退。
4. 真实验收分为“数据覆盖完成”和“逐要素对照完成”；仅有样本不等于门禁完成。

## 2. 必需数据清单

下表中的“必需”是正式发布前必须具备的数据类别；已有样本仅表示当前仓库已经覆盖，不表示全部对照已完成。

| 编号 | 数据类别 | 至少包含的内容 | 当前状态 | 验收重点 |
|---|---|---|---|---|
| D01 | 普通二维基线 | Point、MultiPoint、Polyline、Polygon、MultiPolygon | 已有普通 FileGDB | 图层/字段/要素数、WKB、bbox、属性查询 |
| D02 | Z/M/ZM | Point、Polyline、Polygon 的 Z、M、ZM | `testcurve.gdb` 已有，需逐要素对照 | ISO WKB 维度、坐标插值、bbox、长度/面积 |
| D03 | CircularArc | 单弧、多弧、完整圆、跨象限圆弧 | 两份样本均已有 | 曲线识别、折线化、极值 bbox、长度、空间过滤 |
| D04 | Bezier | `参数化数据_liqs.gdb` 已含 ArcGIS Pro 3.5 原生参数化贝塞尔样本 | 数据和原生来源已确认 | 控制点语义、线性化误差和长度 |
| D05 | Ellipse | `参数化数据_liqs.gdb` 已含 ArcGIS Pro 3.5 原生完整椭圆、椭圆弧和旋转椭圆 | 数据和原生来源已确认 | 长短轴、旋转、起止角、闭合和面积 |
| D06 | 混合曲线 | `参数化数据_liqs.gdb` 已含直线 + CircularArc 混合样本，Bezier/Ellipse 场景已具备 | 数据已具备，组合等价待对照 | part 顺序、连接点、WKB、长度和查询 |
| D07 | 曲线 Polygon | `testcurve.gdb` 和 `参数化数据_liqs.gdb` 已含曲线外环/洞/多面 | 数据已具备，多洞等价待对照 | 环组织、洞语义、面积、点包含、bbox、空间查询 |
| D08 | 多 part | MultiPolyline、MultiPolygon、曲线 MultiPolyline/Polygon | 部分已有 | part 数量、顺序、方向和要素级输出 |
| D09 | FID/ObjectID 间断 | ObjectID 不连续、删除后重建、非 1 起始 ID | `testcurve.gdb` 已有 | fast FID、GDAL FID、ObjectID 三者映射和 Hybrid 回退 |
| D10 | 坏拓扑 | 自交、退化环、重复点、重复环、零面积、相切/重叠 | `testcurve.gdb` 已有，需专项对照 | fail-closed、诊断信息、GDAL 回退，不得静默修正 |
| D11 | 空几何 | 空 Point、空线、空 Polygon、空 MultiPatch | 部分合成测试已有 | 读取、WKB、计数和空间查询 |
| D12 | 非空 MultiPatch | `testcurve.gdb/Multipatch_FC`，3 个要素 | degraded 行为已验收 | 完整 part type 和表面拓扑仍明确为能力边界 |
| D13 | 坐标系 | 已知 WKID/WKT 的 FileGDB 图层 | 最新 `testcurve.gdb` 已有 WGS84/CGCS2000/Xian80 | SRS 读取、层级继承和输出一致性 |
| D14 | 空间索引 | 有效 `.spx`、缺失 `.spx`、损坏 `.spx` | 有效 `.spx` 已有 | 候选查询、精确过滤和回退原因 |
| D15 | 大规模性能 | 至少 10 万要素，推荐 100 万要素；点、线、面各一份 | 点 100K/1M、线/面 10K 顺序读取基线已完成 | bbox、属性查询、Hybrid 和内存使用扩展基线 |

## 3. 每份数据必须附带的资料

每个 `.gdb` 数据包至少附带一个 `manifest.md` 或 `manifest.json`，包含：

| 信息 | 要求 |
|---|---|
| 数据来源 | ArcGIS Pro/其他工具名称和版本、创建步骤 |
| 原始性 | 是否为 ArcGIS 原生曲线；是否经过 GDAL/其他工具导出或线性化 |
| 目录信息 | FileGDB 路径、文件总数、`.gdbtable`、`.gdbtablx`、`.spx`、索引数量 |
| 图层清单 | 图层名、几何类型、字段数、要素数、是否有几何字段 |
| 坐标参考 | WKID、WKT 或明确记录 unknown |
| FID 信息 | GDAL FID、ObjectID、业务主键的对应关系和是否存在间断 |
| 曲线信息 | 曲线类型、控制点/端点、起止角、旋转角、part 组成 |
| 拓扑信息 | 外环、洞、岛中岛、坏拓扑类型和预期处理方式 |
| 空间基准 | 图层 extent、测试 bbox、点包含位置及预期命中结果 |
| 数值基准 | 长度、面积、关键坐标、允许误差 |
| 预期行为 | 纯 C++ 成功、显式不支持、Hybrid 回退或 fail-closed |

对于 Bezier、Ellipse 和曲线 Polygon，必须额外说明“原生曲线”还是“已线性化结果”。当前
`参数化数据_liqs.gdb` 已确认来自 ArcGIS Pro 3.5，可作为 ArcGIS/GDAL 等价性的原生数据证据。

## 4. 每类数据的验收项目

### 4.1 读取与结构

- GDAL OpenFileGDB 能打开；
- fast-gdb 能扫描目录、系统表、业务表和索引；
- 图层数、字段数、要素数与 GDAL 一致；
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

- fast-gdb 失败/不支持时能触发预期回退；
- GDAL Dataset/Layer 缓存路径可用；
- FID 映射不依赖连续 ObjectID 的假设；
- 未命中、无效数据源和映射失败都有明确诊断。

## 5. 非数据发布要求

以下内容不是通过补充 `.gdb` 就能完成，必须单独验收：

1. Windows、Linux、macOS 纯 C++ 构建和测试；
2. Linux GDAL Hybrid、GDAL 曲线后端和产品构建；
3. ASan/UBSan/LSan 几何和解析安全测试；
4. 串行完整 CTest 通过；性能夹具不能因并发共享目录产生误报；
5. 10 万/100 万要素性能基线和资源使用记录；
6. 文档、能力矩阵、迁移指南与实际证据一致；
7. 代码、数据快照、测试命令和结果可复现；
8. 在所有门禁完成前，不得宣称“ArcGIS/GDAL 全量等价验收完成”。

## 6. 建议的数据交付目录

```text
acceptance-data/
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
- `spatial-cases.csv`：bbox/点包含测试及预期结果；
- `expected-values.csv`：长度、面积、关键坐标和容差；
- `source-notes.md`：ArcGIS Pro 版本、创建过程和原生曲线说明。

## 7. 当前缺口汇总

当前已有 `testcurve.gdb` 和 `参数化数据_liqs.gdb`，曲线、Z/M/ZM、FID、坏拓扑、CRS
和性能数据大部分已经具备。后续优先补充的是验收证据，而不是重复制作同类曲线数据：

1. 纯 fast-gdb M 曲线真实编码兼容性仍需修复或明确降级；Hybrid 路径已验收；
2. 曲线、曲线 Polygon、FID 间断和本机性能专项已完成；
3. MultiPatch degraded 行为已完成；完整 part type/表面拓扑如需支持，另立实现任务；
4. 完成跨平台 CI 和最终 ArcGIS/GDAL 等价报告。

当第 2 至第 7 项和第 5 节非数据门禁全部完成，并且第 4 节逐要素对照通过，才可将总体结论更新为“发布验收完成”。
