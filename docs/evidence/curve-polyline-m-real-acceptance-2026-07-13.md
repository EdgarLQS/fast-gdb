# Curve_Polyline_M_FC 真实数据发布门禁证据

- 验收日期：2026-07-13
- 分支：`agent/fix-pure-cpp-m-curves-release-gate`
- 纯 C++ M 曲线代码门禁：`93d1ccde38a523561642f37f42644f74e5a2347a`
- 远端跨平台全绿 run：`geometry-correctness` `29223404842`（run 78）
- 本地编译器：GCC 14.2.0
- CMake：3.31.6
- GDAL/OpenFileGDB：3.10.3

## 输入数据

上传包：`testcurve.gdb.zip`

```text
SHA256 bb1be892fce313da5047c6799b3d8d82476fd3cdfaa206d1a302280346426a58
```

解压后快照：

- 44 个业务图层；
- 1,120,083 个要素；
- 279 个文件；
- 53 个 `.gdbtable`；
- 53 个 `.gdbtablx`；
- 52 个 `.gdbindexes`；
- 45 个 `.spx`。

目标图层 `Curve_Polyline_M_FC` 含 2 个要素，GDAL FID 为 1、2。

## 构建

以 `BUILTIN + STANDARD_WKB` 构建：

```bash
cmake -S . -B build-acceptance -G Ninja \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_CURVE_BACKEND=BUILTIN \
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DFAST_GDB_BUILD_FULL_TESTS=OFF \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-acceptance --target fast_gdb_curve_gdal --parallel
```

验收程序逐项执行仓库测试
`RealDataReleaseContractTest.CurvePolylineMMatchesGdalWithoutHybridFallback`
的相同发布断言，并输出每项机器可读结果。

## 逐要素结果

### GDAL FID 1

- fast-gdb 行号：0（`gdal_fid - 1`）；
- backend：`BuiltinCurve`；
- Hybrid fallback：否；
- `has_m=true`、`source_was_curve=true`、`linearized=true`；
- 1 part，749 个线性化点；
- 749/749 个 M 值为 `NaN`；
- WKB 可被 GDAL 解析，扁平几何类型一致；
- bbox：`0,-5,10,5`，与 GDAL 一致；
- fast-gdb 长度：`23.56192581904423`；
- GDAL 长度：`23.557366028318516`；
- 差值小于发布测试容差 `max(1.0, gdal_length * 1e-3)`。

### GDAL FID 2

- fast-gdb 行号：1（`gdal_fid - 1`）；
- backend：`BuiltinCurve`；
- Hybrid fallback：否；
- `has_m=true`、`source_was_curve=true`、`linearized=true`；
- 1 part，521 个线性化点；
- 521/521 个 M 值为 `NaN`；
- WKB 可被 GDAL 解析，扁平几何类型一致；
- bbox：`0,0,6.9842000007629395,8`，与 GDAL 一致；
- fast-gdb 长度：`13.965031332454057`；
- GDAL 长度：`13.964938362648867`；
- 差值小于发布测试容差。

## 空间查询与汇总

```text
execution_path=bbox:spx
matched_fids=2
gdal_count=2
verified_features=2
fallback_features=0
failures=0
```

全层 extent 查询通过 `.spx` 候选路径命中 2/2，要素映射没有偏移或遗漏。

## Sanitizer 复验

同一份真实数据和同一组断言使用以下配置重新构建和执行：

```text
-fsanitize=address,undefined -fno-omit-frame-pointer
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
```

结果仍为：

```text
verified_features=2
gdal_count=2
fallback_features=0
failures=0
```

ASan、UBSan 和 leak detection 均未报告错误。

## 门禁结论

真实 `Curve_Polyline_M_FC` 的 M 缺失数组编码已完成闭环：

1. 2/2 全要素由纯 C++ `BuiltinCurve` 解析；
2. 0 次 Hybrid/GDAL fallback；
3. FID 映射正确；
4. M 缺失值保持为 `NaN`；
5. WKB 类型、bbox、长度和整层空间查询全部通过；
6. Release 与 sanitizer 两种构建均为 0 失败。

因此该项不再是发布阻塞项。
