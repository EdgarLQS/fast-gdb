# 11 — fast-gdb 几何正确性与曲线支持实施报告

**实施来源分支**：`agent/geometry-wkb-curve-plan`（已合入 `main`）
**基线分支**：`main`
**更新日期**：2026-07-11
**状态**：代码和本机自动化验收完成；当前提交的远端三平台 CI 与 ArcGIS Pro 扩展曲线样本仍是发布门禁

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

仓库内 `test_data/gdb/testcurve.gdb` 已提供真实 CircularArc：GDAL 识别为
`COMPOUNDCURVE/CIRCULARSTRING`，WKB-first 内置路径已与 GDAL 的类型、原生曲线 bbox
和长度完成对比。以下工作仍需要额外真实数据，不能通过继续编写合成单测虚构完成：

1. ArcGIS Pro 创建的 Bezier、Ellipse、完整圆/椭圆；
2. 2D/Z/M/ZM 曲线；
3. 多 part 和曲线 Polygon 洞；
4. 上述样本的 GDAL `hasCurveGeometry(TRUE)` 确认；
5. 面积、点包含和空间查询的曲线/曲线 Polygon 对比；
6. ObjectID 与 GDAL FID 映射抽样；
7. 真实复杂 Polygon、坏拓扑和 MultiPatch 数据集；
8. 普通几何与曲线几何性能基线。

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
- [ ] ArcGIS Pro Bezier/Ellipse/ZM/曲线 Polygon 差异数据

## 7. 分支与合并状态

- 实现已压缩合并至 `main` 的 `2daa907`；
- 本地实施分支已删除；
- PR 状态不作为本报告的验收证据；
- 在自动化和真实发布门禁未满足前，不应声明正式发布验收完成。

## 8. 后续兼容性收口

后续可实现项、外部数据依赖和验收顺序见
[12_fast-gdb几何兼容性收口计划.md](12_fast-gdb几何兼容性收口计划.md)。
