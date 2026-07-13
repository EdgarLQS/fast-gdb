# 13 — fast-gdb 最终等价与发布验收报告

**更新日期**：2026-07-13  
**验收基线**：`a5b96523cb0fd26751383c1e099908d4e6f87be5`  
**验收分支**：`agent/fix-pure-cpp-m-curves-release-gate`  
**纯 C++ M 曲线代码门禁**：`93d1ccde38a523561642f37f42644f74e5a2347a`  
**验收 PR**：#2  
**跨平台代码 CI**：`geometry-correctness` run `29223404842`（run 78）  
**真实数据证据**：`docs/evidence/curve-polyline-m-real-acceptance-2026-07-13.md`

## 1. 最终结论

本轮三个支持范围内的目标均已完成：

1. 修复纯 C++ 对 ArcGIS M-enabled 要素类中“实际为 2D、无逐点 M 数组”的真实曲线尾部解析；
2. 完成 Windows、Linux、macOS 纯 C++、Linux Hybrid、GDAL 产品和 ASan/UBSan/LSan 门禁；
3. 完成真实数据、逐要素对照、Hybrid fallback、FID 映射、空间查询、受控性能基线和能力边界报告。

这里的性能结论仅覆盖报告中的受控本机基线，不代表 35GB/5 亿级生产数据的耗时或吞吐；该规模需要另行建立真实数据基准。

最终发布判定：

- **在本报告声明的支持范围内正式发布：GO**；
- **纯 C++ 线性化产品发布：GO**；
- **Hybrid 产品发布：GO**；
- **真实 `Curve_Polyline_M_FC` M 曲线门禁：GO**；
- **MultiPatch 纯 C++ 完整表面模型：不在当前发布范围**；
- **“支持所有未知、未来或未列出的 ArcGIS 几何编码”声明：不允许**。

此前阻塞正式发布的真实 M 曲线证据缺口已经关闭。这里的 GO 是能力矩阵约束下的正式发布结论，不是对未实现类型的无限范围承诺。

## 2. 纯 C++ M 曲线问题与修复

### 2.1 根因

`Curve_Polyline_M_FC` 是 M-enabled 要素类，但其中可以存储实际不带逐点 M 值的二维 ArcGIS 曲线。FileGDB 在这种情况下使用单字节 `0x42` 表示整段 M 数组缺失，随后立即存放真实曲线描述符。

原实现根据 `has_m=true` 无条件读取 `point_count` 个 M varint，因而把曲线描述符开头误当成 M 数组，最终报：

```text
invalid or truncated curve descriptors
```

### 2.2 实现策略

`gdb_geometry_model.cpp` 现在以事务式两阶段流程解析 multipart 曲线尾部：

1. 在游标和点数组副本上优先尝试标准编码：完整 M 数组 + 曲线描述符；
2. 标准路径失败且几何声明 M 时，回滚并仅接受精确单字节 `0x42`；
3. 将各顶点 M 设为 `NaN`，保留 measured 维度语义；
4. 继续解析真实曲线描述符，并要求完整消费尾部；
5. 截断、未知描述符或多余尾部继续 fail closed。

普通 M/ZM Point、MultiPoint、Polyline 和 Polygon 的原有解析路径不受影响。

## 3. 回归测试

纯 C++ 自包含回归包括：

| 测试 | 断言 |
|---|---|
| `ParsesArcgisMissingMMarkerBeforeNativeCurveDescriptors` | `0x42` 后的 CircularArc 描述符由内置后端解析和线性化，M 为 `NaN` |
| `PreservesCanonicalMArrayBeforeDescriptors` | 标准完整 M 数组仍优先解析，M 插值保持有限 |
| `MissingMMarkerStillFailsClosedOnTruncation` | 截断描述符继续返回 `InvalidEncoding` |

真实数据专项：

```text
RealDataReleaseContractTest.CurvePolylineMMatchesGdalWithoutHybridFallback
```

专项覆盖：

- GDAL FID 到 fast-gdb 行号的 `gdal_fid - 1` 映射；
- `GeometryBackend::BuiltinCurve`；
- `has_m/source_was_curve/linearized`；
- 缺失 M 的 `NaN` 语义；
- ISO WKB 可解析性和扁平类型；
- bbox、长度和整层空间查询；
- 禁止 Hybrid fallback 掩盖纯 C++ 失败。

## 4. 当前真实数据快照

### 4.1 `testcurve.gdb`

本轮收到的原始数据压缩包：

```text
SHA256 bb1be892fce313da5047c6799b3d8d82476fd3cdfaa206d1a302280346426a58
```

解压后由 GDAL/OpenFileGDB 3.10.3 和目录清单确认：

- 44 个业务图层；
- 1,120,083 个要素；
- 279 个文件；
- 53 个 `.gdbtable`；
- 53 个 `.gdbtablx`；
- 52 个 `.gdbindexes`；
- 45 个 `.spx`；
- 覆盖 Point、MultiPoint、Polyline、Polygon、multipart、空几何、Z/M/ZM、CircularArc、Bezier、Ellipse、完整圆、旋转椭圆、椭圆弧、曲线 Polygon 洞/岛中岛、FID 间断、坏拓扑、CRS、MultiPatch 和性能图层。

### 4.2 `参数化数据_liqs.gdb`

已有验收快照：

- 11 个业务图层、12 个要素；
- 18 个 `.gdbtable`、11 个 `.gdbindexes`、11 个 `.spx`；
- 来源确认为 ArcGIS Pro 3.5；
- 覆盖参数化 CircularArc、完整圆、混合曲线、Bezier 命名场景、Ellipse、Ellipse Arc 和曲线 Polygon。

## 5. 真实 M 曲线最终证据

目标图层 `Curve_Polyline_M_FC` 含 2 个要素。Release 构建和 sanitizer 构建均执行了与仓库真实数据专项相同的逐项断言。

### 5.1 FID 1

- fast-gdb 行号：0；
- backend：`BuiltinCurve`；
- Hybrid fallback：否；
- 1 part，749 个线性化点；
- 749/749 个 M 值为 `NaN`；
- WKB 类型与 GDAL 一致；
- bbox：`0,-5,10,5`；
- fast-gdb 长度：`23.56192581904423`；
- GDAL 长度：`23.557366028318516`；
- 长度差位于发布测试容差内。

### 5.2 FID 2

- fast-gdb 行号：1；
- backend：`BuiltinCurve`；
- Hybrid fallback：否；
- 1 part，521 个线性化点；
- 521/521 个 M 值为 `NaN`；
- WKB 类型与 GDAL 一致；
- bbox：`0,0,6.9842000007629395,8`；
- fast-gdb 长度：`13.965031332454057`；
- GDAL 长度：`13.964938362648867`；
- 长度差位于发布测试容差内。

### 5.3 汇总

```text
execution_path=bbox:spx
matched_fids=2
gdal_count=2
verified_features=2
fallback_features=0
failures=0
```

该证据满足原第 12 节全部关闭条件：全要素 BuiltinCurve、无 fallback、FID/WKB/bbox/长度/空间查询全部通过，并已固化为仓库证据文件。

## 6. Sanitizer 真实数据复验

同一份原始数据使用以下配置重新构建和执行：

```text
-fsanitize=address,undefined -fno-omit-frame-pointer
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
```

结果仍为 2/2、0 fallback、0 failures；ASan、UBSan 和 leak detection 均无报告。

## 7. Hybrid fallback 结论

Hybrid 产品遵循：

1. 优先使用纯 C++ `GeometryModel` / WKB-first；
2. 仅对纯 C++ 明确不支持、严格拓扑拒绝或 degraded 的几何调用 GDAL；
3. 使用数据集、图层和显式 FID 上下文；
4. 不根据近似位置猜测 FID；
5. 返回明确 backend、fallback reason 和诊断信息。

本轮修复后，`Curve_Polyline_M_FC` 不再需要 fallback。曲线 Polygon 的部分严格拓扑边界、部分 Ellipse 边界和 MultiPatch 仍可按已记录规则进入 Hybrid。

## 8. FID 映射与空间查询

已完成 Hybrid FID Gap 专项：

- `Point_FIDGap`；
- `Polyline_FIDGap`；
- `Polygon_FIDGap`。

三层命中数与 GDAL 一致，非法几何为 0。

空间查询已验证：

- `.spx` 仅提供候选，最终由统一 `GeometryModel` 精确过滤；
- 普通线、面、multipart、洞和岛中岛；
- 内置线性化曲线；
- 曲线 Polygon 的代表点包含和洞/岛中岛 bbox；
- Hybrid fallback 场景的候选 FID 一致性；
- `Curve_Polyline_M_FC` 全层 extent 查询命中 2/2，执行路径 `bbox:spx`。

## 9. MultiPatch degraded 边界

`Multipatch_FC` 的 3 个真实要素分别包含 2、3、1 个 TIN part：

- 纯 C++ 对 3/3 返回 `UnsupportedType`，不静默伪造成 Polygon；
- Hybrid 对 3/3 回退 GDAL 并返回有效 WKB；
- 当前不保留完整 part type、材质、纹理、法向量或表面拓扑语义。

因此当前可声明 **Hybrid degraded support**，不可声明纯 C++ MultiPatch 完整支持。

## 10. 性能基线

同一台机器的顺序读取基线：

| 图层 | fast-gdb | GDAL |
|---|---:|---:|
| `Perf_Point_100k` | 25.03 ms | 12.29 ms |
| `Perf_Point_1M` | 241.68 ms | 121.83 ms |
| `Perf_Polyline_10k` | 3.72 ms | 2.33 ms |
| `Perf_Polygon_10k` | 3.76 ms | 3.32 ms |

四层要素数一致。该数据用于回归观察，不作为跨机器性能承诺。

### 10.1 发布后复测（2026-07-13）

使用干净构建（macOS 26.4、Apple Clang 21.0.0、GDAL 3.13.0、`FAST_GDB_CURVE_BACKEND=BUILTIN`、`FAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB`）复测了两个真实曲线契约：

```bash
FAST_GDB_REAL_DATASET="$PWD/test_data/gdb/test_spatial_gdb.gdb/test_spatial_gdb.gdb" \
FAST_GDB_CURVE_DATASET="$PWD/test_data/gdb/testcurve.gdb" \
gdb_tutorial_test_runner \
  --gtest_filter='RealDataReleaseContractTest.CurveFileGdbUsesBuiltinWkbFirstPath:RealDataReleaseContractTest.CurvePolylineMMatchesGdalWithoutHybridFallback'
```

两项均通过：`CurveFileGdbUsesBuiltinWkbFirstPath`（68 ms）和 `CurvePolylineMMatchesGdalWithoutHybridFallback`（8 ms）。这再次确认支持范围内的 CircularArc、Bezier、Ellipse 和 M 曲线可由纯 C++ 读取并线性化为 ISO WKB；M 曲线为 2/2、0 次 Hybrid fallback。原生 curve WKB、未知/未来描述符和 MultiPatch 完整表面语义的边界不变。

同轮还以 `FAST_GDB_RUN_10M_BENCHMARKS=1` 运行 `Large10mDataBenchmarkFixture.LARGE_DATA_10M_Query`，仅复用 `test_data/large_10m/large_10m_test.gdb`（约 1.9 GB）做读取/空间查询。Large 窗口返回 8,172,990 个候选，fast-gdb 为 40,881.4 ms、GDAL component 为 3,842.5 ms；本数据集上 GDAL component 更快。该夹具与历史合成 0.1% 查询的实现、数据集和计时阶段不同，不能比较加速比，也不构成 10M 写入复测。本轮未改写该数据集，不扩大当前发布范围。

## 11. 远端跨平台 CI

`geometry-correctness` run `29223404842` 的六项作业全部成功：

| 门禁 | 结果 |
|---|---|
| Linux 纯 C++ | Success |
| macOS 纯 C++ | Success |
| Windows 纯 C++ | Success |
| Linux Hybrid | Success |
| GDAL 默认后端产品构建 | Success |
| Linux ASan + UBSan + leak detection | Success |

该 run 覆盖当前发布代码。随后提交只增加验收证据、更新报告和清理一次性验收工作流，不改变产品代码。

最终文档 head 触发的 run 92 在 GitHub 创建任何 step 之前统一失败：六个 job 的 steps 均为空，未执行 checkout、configure、build 或 test。该记录属于 Actions runner/provisioning 层异常，不能解释为代码或测试失败；已发起失败作业重跑。发布代码仍以 run 78 的完整六项绿灯和本轮真实数据 Release/sanitizer 验收为依据。

## 12. 支持范围

### 12.1 纯 C++ 可以正式声明

- FileGDB Point、MultiPoint、Polyline、Polygon；
- multipart、洞和岛中岛；
- Z、M、ZM；
- CircularArc、CubicBezier、EllipticArc 描述符的内置线性化标准 WKB；
- M 曲线完整 M 数组编码；
- M-enabled 要素类中二维曲线的 `0x42` 缺失-M 编码；
- ISO WKB-first、bbox predicate 和统一空间查询；
- 损坏、截断或未知描述符 fail closed。

### 12.2 Hybrid 可以正式声明

- 全部纯 C++ 支持范围；
- 对纯 C++ 明确不支持、严格拓扑拒绝或 degraded 的几何进行 GDAL fallback；
- 显式 FID offset 映射；
- fallback WKB、bbox 和空间查询；
- MultiPatch degraded 读取。

### 12.3 不应声明

- 纯 C++ 保留 ArcGIS 原生曲线对象或原生 curve WKB；
- 支持所有未知或未来 FileGDB 描述符；
- 纯 C++ MultiPatch 完整表面模型；
- 在没有新增真实数据证据时自动扩展支持矩阵。

## 13. 正式发布建议

- 发布纯 C++ 线性化产品：**GO**；
- 发布 Hybrid 产品：**GO**；
- 宣布真实 `Curve_Polyline_M_FC` 纯 C++ 兼容完成：**GO**；
- 宣布当前支持矩阵内的正式版本：**GO**；
- 宣布“所有 ArcGIS 几何类型无边界完全等价”：**不作此声明**。

原发布门禁已全部关闭。后续新增描述符类型、原生曲线保真输出或 MultiPatch 完整语义，应作为新的能力迭代，而不是当前版本阻塞项。
