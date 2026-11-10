# 曲线保守原包络与源证明接入

日期：2026-11-10。适用源码：`6ef8e8a` 之后的工作区。

状态：曲线几何的保守原包络已实现并用于空间源证明；单位/解码/源回调测试通过。真实 `testcurve.gdb` 曲线图层端到端验收仍缺（样本当前不在原路径），不能据此宣称曲线图层在真实 ArcGIS 数据上已建立源证明。

## 一、问题

`gdb_spatial_source_table.cpp` / `source_bounds` 原先对任何 `source_was_curve || linearized` 直接返回 false，导致曲线图层即使有可用 SPX 也只能整层顺序扫描。线性化采样顶点不能证明原始曲线包络（弧可能凸出采样弦之外），因此必须计算原始曲线载荷的保守包络。

## 二、实现

新增 `explorgdb::conservative_curve_bounds(const CurveRequest&)`（`src/edgar/explorgdb/reader/geometry/curve_geometry.cpp`）：

- **CubicBezier**：取 4 个控制点的包围盒。三次 Bezier 位于控制点凸包内，控制点包围盒是严格保守包络。
- **CircularArc**：复用线性化器的圆心/半径/起始角/扫掠方向判定；包络包含起点、终点以及扫掠区间内的 4 个坐标轴极值点（0、π/2、π、3π/2）。三点式、圆心式、完整圆、`kArcLine` 均覆盖。
- **EllipticArc**：使用完整旋转椭圆的轴对齐包围盒
  `hx = hypot(a·cosθ, b·sinθ)`、`hy = hypot(a·sinθ, b·cosθ)`。对任意子弧保守（可能偏大，但不漏）。
- **线型弧/椭圆**：只取端点。
- 空/点描述符、端点向量椭圆、非法轴、重复/跨部件曲线起点、非法 part sizes、非有限控制点 → 返回 `nullopt`，调用方必须完整回退。

`GeometryModel` 新增 `std::optional<GeometryBounds> conservative_source_bounds`；Builtin 曲线解码成功后由 `GdbGeomDecoder` 附加。`decode_spatial_source_bounds`（`gdb_spatial_source_table.cpp`）对曲线模型优先使用该包络，校验有限性和 `xmin<=xmax/ymin<=ymax` 后返回。非曲线路径不变；MultiPatch 仍使用结构化顶点包络。

## 三、验证

- 几何全套：120/120 通过。
- 源证明相关：25 通过、4 跳过（环境门控基准/预算用例）。
- macOS Release 全套：649 项，0 失败。
- GDAL-free Release 全套：472 项，0 失败。
- GDAL-free ASan/UBSan 全套：471 项，0 失败，无 sanitizer 诊断。
- 首轮 ASan 捕获到 Bezier 包络实现了 4 个控制点的错误：`CurveDescriptor::values` 只有 5 个 double，Bezier 仅含 2 个内部参数点，按 4 点读取越界。修正为 2 个控制点后重跑三套件全部通过。该缺陷在 Release 下因相邻字段可读而未暴露，说明 sanitizer 验证必要。
- 新增定向：
  - `CurveGeometryContract.ConservativeBoundsContainAllLinearizedSamples`：对三点弧、完整圆、圆心式 CCW 弧、Bezier、轴对齐/完整/旋转椭圆、线型弧/椭圆、混合线性+曲线等 8 类，逐点解码线性化结果并断言落入保守包络。
  - `CurveGeometryContract.ConservativeBoundsRejectInvalidOrUnsupportedDescriptors`：空/点弧、端点向量椭圆、点椭圆、零半径、重复起点、跨部件、非法 part sizes、NaN 控制点均返回 `nullopt`。
  - `GeometryModelDecoder.ReportsTruncationAndSupportsExplicitCurveModes`：解码器在 Builtin 曲线模型上附加包络，且所有线性化点位于包络内。
  - `SpatialSourceTableTest.CurvePayloadUsesConservativeSourceBounds`：实际 `FieldRef` + 解码器回调返回圆弧包络 x∈[-1,1]、y∈[0,1]。

日志：`2026-11-10-curve-source-bounds/geometry_full2.log`、
`source_proof2.log`、`ctest_macos2.log`、`ctest_linear.log`、
`ctest_sanitize2.log`、`sanitize_build2.log`、`linear_build.log`。
首轮 sanitizer 失败日志保留为 `ctest_sanitize_first_failure.log`。

## 四、边界

- 椭圆包络使用完整椭圆范围，对子弧保守但不紧；不追求与 SPX 网格等价的候选数量。
- 曲线源证明仍需该图层有可用 `.spx` 且 `HasSpatialIndex` 声明；否则按既有路径完整扫描。
- `FAST_GDB_CURVE_BACKEND=GDAL` 时解码器返回 `UnsupportedCurve`，不附加包络，曲线仍回退；默认 `BUILTIN` 构建生效。
- 真实曲线语料（含 M 曲线、参数化圆弧/椭圆）的 FID parity 与 GDAL 对照需在恢复 `testcurve.gdb` 后补验。
