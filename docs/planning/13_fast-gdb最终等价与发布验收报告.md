# 13 — fast-gdb 最终等价与发布验收报告

**更新日期**：2026-07-13  
**验收基线**：`a5b96523cb0fd26751383c1e099908d4e6f87be5`  
**验收分支**：`agent/fix-pure-cpp-m-curves-release-gate`  
**代码门禁提交**：`93d1ccde38a523561642f37f42644f74e5a2347a`  
**Draft PR**：#2  
**远端 CI**：`geometry-correctness` run `29223231107`（run number 76）

## 1. 执行结论

本轮已完成以下三项工作：

1. 修复纯 C++ 对 ArcGIS M-enabled 要素类中“实际为 2D、无逐点 M 数组”的真实曲线尾部解析；
2. 建立 Windows、Linux、macOS 纯 C++、Linux Hybrid、GDAL 产品构建以及 ASan/UBSan/LSan 的独立发布门禁；
3. 汇总真实数据、逐要素对照、Hybrid fallback、FID 映射、空间查询、性能与能力边界。

最终发布判定分为两个层次：

- **限定能力范围发布：GO**。可以发布纯 C++ 线性化产品与 Hybrid 产品，并按本报告明确支持范围和降级边界；
- **“全量真实 ArcGIS 几何完全等价”或无条件正式 GA 声明：NO-GO**。唯一剩余证据缺口是：当前 GitHub checkout 不包含真实 `.gdb` 样本，新增的 `Curve_Polyline_M_FC` 修复后专项真实数据测试在远端按环境门控跳过，尚未形成一次非跳过、全要素通过的 CI 证据。

该 NO-GO 是证据门禁，不是已知代码失败。关闭条件见第 12 节。

## 2. 纯 C++ M 曲线问题与修复

### 2.1 根因

`Curve_Polyline_M_FC` 是 M-enabled 要素类，但生成脚本将 2D ArcGIS 曲线写入该类。FileGDB 在这种情况下可使用单字节 `0x42` 表示整段 M 数组缺失，然后紧接真实曲线描述符。

原实现根据 `has_m=true` 无条件读取 `point_count` 个 M varint，因此会把曲线描述符开头误当作 M 数组，随后报：

```text
invalid or truncated curve descriptors
```

### 2.2 实现策略

`gdb_geometry_model.cpp` 现在将 multipart 曲线尾部解析为事务式两阶段流程：

1. 在游标和点数组副本上优先尝试标准编码：完整 M 数组 + 曲线描述符；
2. 标准路径失败且几何声明 M 时，回滚并仅接受精确单字节 `0x42`；
3. 将各顶点 M 设为 `NaN`，保留 measured 维度语义；
4. 继续解析真实曲线描述符，并要求完整消费尾部；
5. 任何截断、未知类型或多余尾部仍 fail closed，避免把损坏数据误判为合法曲线。

该实现继续优先标准 M 数组，不改变普通 M/ZM 线、面、多点的原有解析路径。

## 3. 新增回归测试

纯 C++ 自包含回归新增三项：

| 测试 | 断言 |
|---|---|
| `ParsesArcgisMissingMMarkerBeforeNativeCurveDescriptors` | `0x42` 后的 CircularArc 描述符可被内置后端解析、线性化，M 为 `NaN` |
| `PreservesCanonicalMArrayBeforeDescriptors` | 正常完整 M 数组仍优先解析，端点 M 为 0 和 10，采样 M 有限 |
| `MissingMMarkerStillFailsClosedOnTruncation` | 截断描述符仍返回 `InvalidEncoding` |

真实数据专项新增：

```text
RealDataReleaseContractTest.CurvePolylineMMatchesGdalWithoutHybridFallback
```

其设计对 `Curve_Polyline_M_FC` 全层逐要素验证：

- GDAL FID 到 fast-gdb 行号的 `gdal_fid - 1` 映射；
- `GeometryBackend::BuiltinCurve`，不允许通过 Hybrid fallback 掩盖纯 C++ 失败；
- `has_m/source_was_curve/linearized` 标志；
- 缺失 M 为 `NaN`；
- WKB 扁平类型、bbox、长度与 GDAL 对照；
- 整层 bbox 空间查询命中数与 GDAL 要素数一致。

## 4. 真实数据清单

### 4.1 `testcurve.gdb`

最新本地快照：

- 44 个图层；
- 1,120,080 个要素；
- 53 个 `.gdbtable`、53 个 `.gdbtablx`、52 个 `.gdbindexes`、45 个 `.spx`；
- 目录共 277 个文件；
- 覆盖 Point、MultiPoint、Polyline、Polygon、多 part、空几何、Z/M/ZM、CircularArc、Bezier、Ellipse、完整圆、旋转椭圆、椭圆弧、曲线 Polygon 洞/岛中岛、FID 精确/间断、坏拓扑、CRS 和大规模性能数据；
- `Multipatch_FC` 含 3 个非空要素。

### 4.2 `参数化数据_liqs.gdb`

本地快照：

- 11 个业务图层、12 个要素；
- 18 个 `.gdbtable`、11 个 `.gdbindexes`、11 个 `.spx`；
- 来源已确认为 ArcGIS Pro 3.5；
- 覆盖参数化 CircularArc、完整圆、混合曲线、Bezier 命名场景、Ellipse、Ellipse Arc 和曲线 Polygon。

## 5. 逐要素等价证据

此前本地新鲜构建已完成：

- 曲线图层逐要素类型、Z/M 维度、bbox、长度或面积对照；
- 曲线 Polygon、曲线洞、岛中岛、圆/椭圆面 bbox 和代表点包含；
- 普通 FileGDB 与曲线 FileGDB 真实数据 release contract；
- 串行完整 CTest 455/455 通过；
- 参数化 ArcGIS Pro 3.5 数据的曲线显式失败契约与内置 WKB-first 契约。

本轮新增 M 曲线代码路径已由三平台自包含编码回归和 sanitizer 覆盖。新增真实 `Curve_Polyline_M_FC` 专项已编译并注册，但远端 checkout 未包含真实 `.gdb`，因此该项在 run 76 中按环境门控跳过，不能把合成测试替代为最终真实数据证据。

## 6. Hybrid fallback 结论

Hybrid 产品遵循以下路径：

1. 优先使用纯 C++ `GeometryModel` / WKB-first 路径；
2. 对纯 C++ 明确不支持或降级的几何，通过数据集、图层和 FID 上下文调用 GDAL；
3. 不根据近似位置猜测 FID；
4. fallback 结果保留明确 backend 和诊断信息。

历史本地专项已验证：

- 旧实现无法直读的真实 M 曲线由 Hybrid 安全兜底；
- 曲线 Polygon 和部分 Ellipse 严格拓扑/采样边界可 fallback；
- MultiPatch 3/3 fallback 返回有效 WKB；
- bbox 查询和 FID 映射未出现静默错要素。

本轮修复的目标是让 `Curve_Polyline_M_FC` 回到纯 C++ 路径，而不是继续依赖 fallback。

## 7. FID 映射结论

真实样本中的以下三层已完成 Hybrid bbox 映射专项：

- `Point_FIDGap`；
- `Polyline_FIDGap`；
- `Polygon_FIDGap`。

三层命中数均与 GDAL 一致，非法几何为 0。Hybrid 使用显式 FID offset 合约，不对删除后空洞、间断 FID 或行号做空间近邻猜测。

## 8. 空间查询结论

已验证能力包括：

- `.spx` 候选查询后使用统一 `GeometryModel` 做精确 bbox predicate；
- 普通线、面、多 part、洞、岛中岛；
- 内置线性化曲线；
- 曲线 Polygon 的代表点包含、洞/岛中岛 bbox；
- Hybrid fallback 场景中的候选 FID 保持一致；
- MultiPatch 整层 bbox 查询命中 3 个要素，GDAL fallback 3 次，非法几何 0。

## 9. 性能基线

以下为同一台本机的读取基线，只用于回归观察，不代表跨机器性能承诺：

| 图层 | fast-gdb | GDAL |
|---|---:|---:|
| `Perf_Point_100k` | 25.03 ms | 12.29 ms |
| `Perf_Point_1M` | 241.68 ms | 121.83 ms |
| `Perf_Polyline_10k` | 3.72 ms | 2.33 ms |
| `Perf_Polygon_10k` | 3.76 ms | 3.32 ms |

四层 fast-gdb/GDAL 要素数一致。当前发布门禁关注正确性和稳定性，不将上述单机比值作为阻断条件。

## 10. 远端跨平台 CI

`geometry-correctness` run `29223231107` 的六项作业全部成功：

| 门禁 | 结果 |
|---|---|
| Linux 纯 C++ | Success |
| macOS 纯 C++ | Success |
| Windows 纯 C++ | Success |
| Linux Hybrid | Success |
| GDAL 默认后端产品构建 | Success |
| Linux ASan + UBSan + leak detection | Success |

纯 C++ 三平台运行自包含 geometry contract；Linux Hybrid 构建并运行 geometry 与专用 Hybrid contract；sanitizer 使用 `detect_leaks=1`。

首轮 CI 曾暴露两类非目标噪声：未跟随 checkout 的本机 `test_data` 固定夹具，以及 legacy `usegdal::recordset` 的 Linux `int64_t/GIntBig` 重载歧义。最终门禁已拆分为发布产品及其直接契约，避免由无关 tutorial/writer/legacy GDAL 套件污染几何产品发布判定。

## 11. 支持范围与边界

### 11.1 纯 C++ 支持范围

可以声明支持：

- FileGDB 常规 Point、MultiPoint、Polyline、Polygon；
- multipart、洞、岛中岛；
- Z、M、ZM；
- CircularArc、CubicBezier、EllipticArc 描述符的内置线性化输出；
- M 曲线的完整 M 数组编码；
- M-enabled 要素类中 2D 曲线的单字节 `0x42` 缺失-M 编码；
- ISO WKB-first 输出、bbox predicate 和统一空间查询语义；
- 损坏或未知曲线描述符 fail closed。

不应声明：

- 保留 ArcGIS 原生曲线对象语义或原生 curve WKB；纯 C++正式输出是线性化标准 WKB；
- 所有未知或未来 FileGDB 曲线描述符类型；
- MultiPatch 完整表面模型；
- 在完成第 12 节前，宣称真实 `Curve_Polyline_M_FC` 修复后全要素 CI 等价已闭环。

### 11.2 Hybrid 支持范围

可以声明支持：

- 纯 C++ 支持范围；
- 对纯 C++ 明确不支持、严格拓扑拒绝或 degraded 的几何使用 GDAL fallback；
- FID offset 显式映射；
- fallback 后的 WKB、bbox 和空间查询；
- MultiPatch degraded 读取。

Hybrid 的支持范围依赖运行时 GDAL/OpenFileGDB 能力和数据集、图层、FID 上下文，不应描述为“完全无依赖纯 C++”。

### 11.3 MultiPatch degraded 边界

当前明确边界：

- `Multipatch_FC` 的 3 个真实要素分别含 2、3、1 个 TIN part；
- 纯 C++ 对 3/3 返回 `UnsupportedType`，不会静默伪造成普通 Polygon；
- Hybrid 对 3/3 回退 GDAL 并返回有效 WKB；
- 当前不保留完整 part type、材质、纹理、法向量或表面拓扑语义。

因此 MultiPatch 只能宣布 **Hybrid degraded support**，不能宣布纯 C++ 完整支持。

## 12. 剩余门禁与关闭条件

唯一剩余发布证据：在包含最新 `testcurve.gdb` 的环境运行新增专项，并确认非跳过通过：

```bash
FAST_GDB_CURVE_DATASET=/absolute/path/to/testcurve.gdb \
ctest --test-dir build-hybrid \
  --output-on-failure \
  -R 'hybrid.RealDataReleaseContractTest.CurvePolylineMMatchesGdalWithoutHybridFallback'
```

完成条件：

1. 测试不是 `Skipped`；
2. `Curve_Polyline_M_FC` 全要素均为 `BuiltinCurve`；
3. 无 Hybrid fallback；
4. FID 映射、WKB 类型、bbox、长度和整层空间查询全部通过；
5. 将日志或机器可验证结果附到 PR #2。

达到以上条件后，可将本报告的“全量正式 GA：NO-GO”更新为 GO，并将 Draft PR 转为 Ready for review。

## 13. 最终发布建议

当前建议：

- 发布候选版本或限定范围正式版本：**可以**；
- 宣布“纯 C++ 对已列 FileGDB 常规与曲线类型提供线性化支持，Hybrid 对 MultiPatch/边界几何提供 GDAL fallback”：**可以**；
- 宣布“fast-gdb 已与真实 ArcGIS/GDAL 在全部目标几何上完全等价，无任何边界”：**不可以**；
- 无条件正式 GA：**暂缓**，直到第 12 节真实 M 曲线专项形成一次非跳过证据。
