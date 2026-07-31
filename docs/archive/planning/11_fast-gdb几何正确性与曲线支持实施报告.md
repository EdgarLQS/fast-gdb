# 11 — fast-gdb 几何正确性与曲线支持实施报告

**实施来源分支**：`agent/geometry-wkb-curve-plan`（已合入 `main`）
**基线分支**：`main`
**更新日期**：2026-07-13
**状态**：代码和两份真实曲线样本的阶段性验收完成；最新 `testcurve.gdb` 复验通过；远端三平台 CI、完整曲线等价和最终 ArcGIS/GDAL 等价证据仍是发布门禁

## 1. 实施目标

本轮工作依据 `10_fast-gdb几何正确性与曲线支持执行计划.md`，解决以下核心问题：

1. Polygon 不再按 part 直接输出独立 Polygon；
2. WKT、WKB 和空间过滤不再维护三套几何解释；
3. 正式外部输出改为 ISO WKB-first；
4. FileGDB CircularArc、Bezier、Ellipse 可由内置算法折线化；
5. 可选 GDAL 后端只在曲线或不可靠拓扑时回退；
6. 纯 C++ 与 Hybrid 产物和依赖边界清晰；
7. 错误状态、后端路径和降级行为可观测。

## 2. 已完成代码

### 2.1 统一几何模型

新增：

- `geometry_model.h`
- `GeometryModel`
- `GeometryValue`
- `GeometryStatus`
- `GeometryBackend`
- `CoordinateTransform`
- 整数网格 `GridPoint/GridBbox`

Point、MultiPoint、Polyline、Polygon 和曲线折线化结果均进入同一个模型。

### 2.2 Polygon 拓扑

新增 `polygon_topology.*`，实现：

- 连续重复点和重复闭合点清理；
- 整数网格方向、线段相交和点在环内判断；
- 环直接包含父子关系；
- 深度奇偶构建外环、洞和岛中岛；
- 多个不相交外环形成 MultiPolygon；
- Z/M 连同顶点一起反转；
- 自交、重复环、退化环、相切/重叠明确失败；
- 不依赖 MSVC 不支持的 `__int128`，使用可移植的 64×64→128 位符号比较。

### 2.3 输出

新增：

- `wkb_writer.*`：ISO WKB 2D/Z/M/ZM；
- `wkt_writer.*`：兼容和调试输出；
- `decode_value()` / `read_geometry_value()`；
- `decode_model()` / `read_geometry_model()`。

原有 `decode()` WKT API 保留，但普通点线面由同一个 `GeometryModel` 生成，不再维护独立 Polygon 解释。

### 2.4 空间判断

新增 `spatial_predicate.*`：

- Point/MultiPoint；
- 线段与连续网格 bbox 裁剪；
- Polygon 外环、洞、岛中岛；
- `.spx` 仅负责候选，精确过滤复用统一模型；
- 查询框使用 `long double` 网格坐标，不因整数取整漏掉子网格窗口中的线段相交。

### 2.5 内置曲线

新增 `curve_geometry.*`，支持：

- 三点 CircularArc；
- 圆心 CircularArc；
- 完整圆；
- Cubic Bezier 自适应折线化；
- EllipticArc minor/major/complete/rotation；
- 混合直线/曲线 part；
- Z/M 参数插值；
- 最大弦高误差、最大角步长、最大分段数；
- 起点越界、`SIZE_MAX`、跨 part、截断和非法参数 fail closed。

FileGDB 椭圆描述符的旋转符号已与 GDAL/OpenFileGDB 的约定对齐。

### 2.6 GDAL Hybrid

新增：

- `gdal_curve_backend.*`
- `hybrid_geometry_reader.*`
- `HybridGeometryReader`
- `HybridQueryEngine`

执行策略：

```text
普通几何 -> fast-gdb
内置可处理曲线 -> 默认 fast-gdb
曲线拒绝/不支持或 Polygon 拓扑失败 -> GDAL FID fallback
```

GDAL Dataset/Layer 使用线程本地缓存，避免每条记录重新打开和跨线程共享 Layer 游标。

FID 映射默认：

```text
GDAL FID = fast-gdb FID + 1
```

偏移可配置；映射未命中不会尝试其他偏移，防止静默读取错误要素。

### 2.7 构建与平台

CMake 新增/整理：

- `fast_gdb_geometry_core`
- `fast_gdb_linear`
- `fast_gdb_curve_gdal`
- `fast_gdb_hybrid`
- `FAST_GDB_WITH_GDAL`
- `FAST_GDB_CURVE_BACKEND`
- `FAST_GDB_GEOMETRY_OUTPUT`
- `FAST_GDB_BUILD_FULL_TESTS`

Writer 的纯数据写入和 GDAL 索引助手已拆分依赖：无 GDAL 构建不编译 `gdb_index_creator.cpp`，有 GDAL 构建显式继承 GDAL include/link。

Windows Reader 使用无共享游标的 `ReadFile(OVERLAPPED)` 实现 `pread` 兼容路径；mmap 不可用时回退到位置读取。

## 3. 测试覆盖

已新增或扩展：

- Polygon 环顺序和方向随机化；
- 外环 + 洞 + 岛中岛；
- 多外环；
- 自交、重复、退化、相切；
- Z/M 方向反转绑定；
- ISO WKB 类型和结构；
- WKT 与 WKB 共用拓扑；
- 点线面空间判断和洞内查询；
- 连续子网格 bbox 与穿越线段；
- `int64` 全范围方向符号；
- CircularArc、完整圆；
- Bezier 和 Z/M 插值；
- Ellipse minor/major/complete/rotation；
- 混合 part；
- 截断前缀；
- 1000 组确定性随机垃圾输入；
- `SIZE_MAX` 曲线起点；
- Hybrid FID 映射和无效数据源诊断。

CI 矩阵：

- Windows/Linux/macOS 纯 C++；
- Linux GDAL Hybrid；
- `FAST_GDB_CURVE_BACKEND=GDAL` 独立产品构建；
- ASan/UBSan 几何核心。

最终通过状态将在本报告第 6 节收口后更新。

## 4. 兼容性决策

### 4.1 WKT

- `GdbGeometry::wkt` 暂不删除；
- 新代码优先使用 `GeometryValue::wkb`；
- WKT 兼容期至少覆盖一个稳定大版本；
- 性能路径禁止 WKT → Geometry → WKB 中转。

### 4.2 曲线

- 默认正式格式：折线化 ISO WKB；
- 原生 curve WKB：仅 GDAL Hybrid 显式 opt-in；
- REJECT 后端：明确 `UnsupportedCurve`；
- 不把端点弦线伪装成曲线结果。

### 4.3 MultiPatch

MultiPatch 仍为 degraded：

- 坐标和有限 WKT 可读；
- 完整 part type、TriangleStrip/Fan 和表面拓扑未纳入纯 C++ 统一模型；
- Hybrid 可配置 GDAL 回退；
- 发布说明必须继续明确该边界。

## 5. 尚未由合成测试替代的发布门禁

仓库内最新 `test_data/gdb/testcurve.gdb` 已提供一组阶段性真实样本。GDAL 可识别其中的
CircularArc、混合曲线、完整圆、Ellipse、旋转 Ellipse、Ellipse Arc 和曲线 Polygon；
Bezier 命名样本在 GDAL 输出中表现为已采样的 `MULTILINESTRING`；参数化数据来源已确认
为 ArcGIS Pro 3.5，但等价性仍需单独确认。以下工作仍需要补充对照，不能通过继续编写合成
单测虚构完成：

1. 对已确认来自 ArcGIS Pro 3.5 的 `参数化数据_liqs.gdb`/`testcurve.gdb` 的 Bezier、Ellipse、
   完整圆/椭圆完成逐要素对照；原生 provenance 门禁已满足；
2. 曲线 2D/Z/M/ZM 的完整组合与 GDAL `hasCurveGeometry(TRUE)` 确认；
3. 曲线 Polygon 的明确岛中岛样本，以及面积、点包含和空间查询对比；
4. `Point_FIDGap`、`Polyline_FIDGap`、`Polygon_FIDGap` 的 Hybrid FID 逐要素映射抽样；
5. MultiPatch 完整 part type/表面拓扑边界；
6. 普通几何与曲线几何性能基线；
7. 当前提交的远端 Windows/Linux/macOS 和 Linux Hybrid CI。

这些是发布验收数据要求，不是当前代码中可用猜测替代的未实现函数。

## 6. 验证记录

### 当前已确认

- macOS 本机纯 C++ 完整 CTest：`253/253` 通过；
- macOS 本机 GDAL Hybrid 完整 CTest：`452/452` 通过；
- macOS 本机 ASan/UBSan 几何专项：`88/88` 通过（macOS ASan 不支持
  `detect_leaks=1`，Linux CI 保留该检查）；
- `FAST_GDB_CURVE_BACKEND=GDAL` 产品目标 `fast_gdb_curve_gdal` 本机构建通过；
- 普通真实 FileGDB release contract：
  `test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb` 通过；
- 真实 CircularArc WKB-first contract：`test_data/gdb/testcurve.gdb` 通过，包含
  GDAL 原生曲线类型、bbox 和长度对比；
- 当前分支的 CMake/CTest 支持源目录之外的构建目录，不再依赖构建目录恰好位于仓库内；
- 第一轮 CI 暴露并修复 `QueryGridBbox` 头/实现类型、曲线安全索引重载歧义、
  Writer GDAL include/link 边界和 Windows POSIX 宏冲突；Actions 日志已配置为 artifact。

### 待最终填写

- [ ] Windows 纯 C++：当前提交的远端构建/测试
- [ ] Linux 纯 C++：当前提交的远端构建/测试
- [x] macOS 纯 C++：本机构建/测试
- [ ] Linux Hybrid：当前提交的远端构建/测试
- [x] GDAL 默认后端：本机构建
- [x] ASan/UBSan：本机几何核心（Linux LSan 仍由 CI 覆盖）
- [x] 普通真实 FileGDB release contract
- [x] 仓库真实 CircularArc WKB-first/GDAL 对比
- [x] `testcurve.gdb` 第一轮真实数据快照和 GDAL 图层基准
- [x] `testcurve.gdb` 的 Z/M/ZM、圆/椭圆、FID 间断和坏拓扑样本已发现
- [ ] Bezier/曲线 Polygon 岛中岛和逐要素等价对照
- [x] MultiPatch 非空样本的 degraded 行为对照；完整 part type/表面拓扑仍不在当前能力范围
- [x] 性能基线
- [ ] 远端 CI

### 6.1 第一轮真实数据验收：`testcurve.gdb`

验收路径：`test_data/gdb/testcurve.gdb`（当前为单层 FileGDB 目录）。该节记录的是早期
版本的第一轮样本快照：GDAL OpenFileGDB 当时发现 25 个图层、54 个要素、32 个
`.gdbtable` 和 26 个 `.spx`。最新文件已扩展，结果见第 6.3 节。
样本覆盖普通 Point/MultiPoint/Polyline/Polygon、洞和岛中岛、Z/M/ZM、CircularArc、
Bezier 场景、混合曲线、完整圆、Ellipse、旋转 Ellipse、Ellipse Arc、曲线 Polygon、
FID 间断、自交、退化环、重复点和零面积 Polygon；该早期快照中的 `Multipatch_FC` 存在但要素数为 0，
后续扩展数据已补充非空 MultiPatch，见第 6.3 节。

本机在源目录之外的干净构建目录使用 `BUILTIN + STANDARD_WKB` 配置重新构建成功。
完整 CTest 结果为 `455/455` 通过、0 失败；其中环境门控的真实数据测试和大规模/事务
测试按设计跳过。专项真实数据结果如下：

- 普通样本 `test_spatial_gdb.gdb`：`RegularFileGdbMatchesCoreReadContract` 通过；
- 最新 `testcurve.gdb`：`CurveFileGdbIsExplicitlyUnsupported` 通过；
- 最新 `testcurve.gdb`：`CurveFileGdbUsesBuiltinWkbFirstPath` 通过；
- Bezier 样本由 GDAL 展示为已线性化 `MULTILINESTRING`；来源已确认是 ArcGIS Pro 3.5，
  但逐要素几何等价性仍需对照；
- FID 间断、坏拓扑和曲线 Polygon 已完成结构发现，但逐要素 Hybrid/FID、面积、点包含
  和空间查询对照仍待专项执行。

本节结论为“第一轮真实数据验收完成”，不是正式发布完成。后续补充数据时在本节追加
结果，不覆盖本轮证据。

### 6.3 `testcurve.gdb` 最新数据复验：2026-07-13

本次以当前工作区数据重新执行 GDAL OpenFileGDB 快照和新鲜构建测试。最新数据发现：

- 44 个图层、1,120,080 个要素；
- 53 个 `.gdbtable`、53 个 `.gdbtablx`、52 个 `.gdbindexes`、45 个 `.spx`，目录共 277 个文件；
- 保留 Point/MultiPoint/Polyline/Polygon、多 part、空几何、Z/M/ZM、CircularArc、Bezier、
  Ellipse、完整圆、旋转椭圆、椭圆弧、曲线 Polygon 洞/岛中岛、FID 精确/间断和坏拓扑；
- 新增或确认 `CRS_WGS84_Point_FC`、`CRS_CGCS2000_Point_FC`、`CRS_Xian80_Point_FC`；
- 包含 `Perf_Point_100k`、`Perf_Polyline_10k`、`Perf_Polygon_10k` 和 `Perf_Point_1M`；
- `Multipatch_FC` 为 3 个要素，非空 MultiPatch 真实样本已覆盖；降级链路已通过专项对照，完整 part type 和表面拓扑语义仍不在当前能力范围。

复验命令使用 `/private/tmp/fast-gdb-real-acceptance` 的 `BUILTIN + STANDARD_WKB` 构建：

- 普通 FileGDB 与最新 `testcurve.gdb` 同时设置时，`RealDataReleaseContractTest` 为 3/3 通过；
- `CurveFileGdbIsExplicitlyUnsupported` 通过；
- `CurveFileGdbUsesBuiltinWkbFirstPath` 通过；
- 串行完整 CTest 为 455/455 通过，0 失败；环境门控的真实数据、事务和 10M/大规模基准按设计跳过；
- `explorgdb_cli` 能扫描目录文件，但对根部 4 字节 `gdb` 文件报告 `magic unexpected`；GDAL 和
  真实数据契约均能正常打开，因此该项记录为 CLI 诊断兼容性边界，不判为数据损坏。

本次复验确认当前数据已经覆盖原计划中的大部分真实样本类别，且已确认参数化数据来自 ArcGIS Pro 3.5，
包含贝塞尔、椭圆、椭圆弧和混合曲线，因此不再把这些类别列为“缺数据”，原生 provenance 门禁已满足。
逐要素曲线、曲线面空间语义、Hybrid FID 和性能基线已在本地专项中完成；跨平台远端 CI 和最终
ArcGIS/GDAL 全量等价结论仍未完成。MultiPatch 完整 part type/表面拓扑仍是明确能力边界。

MultiPatch degraded 专项已完成：GDAL 基准显示 3 个非空要素分别包含 2、3、1 个 TIN part；
fast-gdb 对 3/3 要素返回 `UnsupportedType`，未静默伪造普通 Polygon；Hybrid 对 3/3 要素
成功回退 GDAL 并返回有效 WKB；整层 bbox 查询使用 `.spx`，命中 3 个要素，GDAL 回退 3 次，
非法几何为 0。该结果验证降级链路，不代表 part type 和完整表面拓扑被 fast-gdb 保留。

### 6.2 参数化曲线数据验收：`参数化数据_liqs.gdb`

验收路径：`test_data/gdb/参数化数据_liqs.gdb`。GDAL OpenFileGDB 成功打开该目录，
发现 11 个业务图层、12 个要素、18 个 `.gdbtable`、11 个 `.gdbindexes` 和 11 个
`.spx` 空间索引；SRS 均为 unknown。

覆盖图层包括：参数化_贝塞尔（2 个要素）、参数化多线、参数化椭圆、参数化椭圆弧、
参数化圆、参数化圆弧、参数化圆弧和线、参数化多面、参数化椭圆_面、参数化圆_面、
参数化圆弧_面（其余各 1 个要素）。GDAL 观察到 CircularArc/混合曲线为 `MULTICURVE`
或 `CURVEPOLYGON`，圆/椭圆面为 `MULTISURFACE` 或线性化 `MULTIPOLYGON`；Bezier
图层为采样后的 `MULTILINESTRING`。

本机复用源目录之外的 `BUILTIN + STANDARD_WKB` 新鲜构建执行真实数据契约：普通样本与
本数据同时设置时 `RealDataReleaseContractTest` 为 3/3 通过；曲线显式失败契约和内置
WKB-first 曲线契约均通过，确认曲线不会静默伪装为普通线/面，并至少有一条 GDAL 曲线
完成内置模型线性化、WKB 解析、bbox 和长度对照。`explorgdb_cli` 也完成了目录、系统表、
业务表、索引和 `.spx` 文件扫描。

同一新鲜构建目录使用串行 CTest 复跑，结果为 `455/455` 通过、0 失败；真实数据契约、
大规模基准和事务用例中的环境门控跳过项按设计保留。并行执行不作为本次证据，因为性能
夹具共享 `test_data/benchmark/perf_100k.gdb`，会产生目录创建竞态。

该数据没有覆盖 Z/M/ZM、FID 间断、坏拓扑或大规模性能场景；GDAL 已将
Bezier 线性化；来源已确认来自 ArcGIS Pro 3.5，但仍需逐要素对照才能证明几何等价。
曲线面面积、点包含、空间查询和 Hybrid FID 映射已在第 6.4 节本地专项中完成。结论为“参数化真实曲线
数据阶段性验收通过”，不是正式发布完成。

### 6.4 本地完整专项验收：2026-07-13

使用 `/private/tmp/fast-gdb-real-acceptance` 新鲜构建，并以 GDAL OpenFileGDB 作为基准，
完成以下本地专项：

- 曲线图层逐要素类型、Z/M 维度、bbox、长度/面积对照：通过；
- 曲线 Polygon、曲线洞、岛中岛、圆/椭圆面 bbox 和代表点包含对照：通过；
- `Point_FIDGap`、`Polyline_FIDGap`、`Polygon_FIDGap` Hybrid bbox 映射：全部通过，命中数与 GDAL 一致，非法几何为 0；
- `Perf_Point_100k`、`Perf_Point_1M`、`Perf_Polyline_10k` 和 `Perf_Polygon_10k` 读取基线：
  fast-gdb/GDAL 要素数全部一致；本机实测分别为 25.03/12.29ms、241.68/121.83ms、
  3.72/2.33ms、3.76/3.32ms，仅作本机基线，不作为跨机器性能结论；
- `Multipatch_FC` degraded 行为：已在上一节完成，3/3 要素 GDAL fallback 有效。

本地专项同时暴露一个明确边界：`Curve_Polyline_M_FC` 的纯 fast-gdb 内置解析对 ArcGIS
真实 M 曲线编码返回 `invalid or truncated curve descriptors`；曲线 Polygon 和部分 Ellipse
样本也会因严格拓扑/采样限制触发 fallback。Hybrid 对这些要素均成功回退 GDAL，并完成类型、
bbox、长度/面积和空间结果对照。因此当前结论是“Hybrid 本地验收通过，纯 C++ 对所有真实曲线
的直读等价仍未完成”，不能将该边界隐藏为全量纯 C++ 通过。

## 7. 分支与合并状态

- 实现已压缩合并至 `main` 的 `2daa907`；
- 本地实施分支已删除；
- PR 状态不作为本报告的验收证据；
- 在自动化和真实发布门禁未满足前，不应声明正式发布验收完成。

## 8. 后续兼容性收口

后续可实现项、外部数据依赖和验收顺序见
[12_fast-gdb几何兼容性收口计划.md](12_fast-gdb几何兼容性收口计划.md)。
