# S5.3 当前源码 sanitizer 补验

被测源码：`becba745a69a53a7e9df94bdbad7f88f248fff78`，工作区在构建时无改动。包含 `36eede8` 类型/宽度、`4a74821` 扫描长度溢出、`a655695` 最后子页、`becba74` 空间求值失败传播四项后续修复。本次仅新增验证记录，没有修改产品代码。

## 环境与执行

Linux ARM64 / GCC 14.2，既有容器 `fast-gdb-linux-s52`。源码 `/src` 以及用户曲线样本 `/data/testcurve.gdb` 只读挂载。复用独立构建目录 `/tmp/fast-gdb-linux-asan`，本轮单线程完成 29 项增量构建，未并行编译其他配置，避免此前已知的虚拟机内存限制。

Debug、GDAL OFF、Unified ON；编译参数 `-fsanitize=address,undefined -fno-omit-frame-pointer`，executable linker 参数 `-fsanitize=address,undefined`。`ldd` 实际确认测试程序链接 `libasan.so.8` 和 `libubsan.so.1`，见 [构建](2026-11-08-current-sanitizers/build.log)和 [runtime 配置](2026-11-08-current-sanitizers/runtime.log)。

执行命令（容器内）：

```sh
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
FAST_GDB_CURVE_DATASET=/data/testcurve.gdb \
ctest --test-dir /tmp/fast-gdb-linux-asan --output-on-failure -j1
```

## 结果与覆盖边界

发现 441 项，436 PASS / 5 SKIP / 0 FAIL，16.49 秒，无 ASan、UBSan、LeakSanitizer 诊断。完整结果见 [CTest 日志](2026-11-08-current-sanitizers/ctest.log)。新增的显式键类型宽度、未知键宽度拒绝、溢出几何长度以及既有坏行、v1/v2、UUID 和 NaN 合同均实际执行通过。真实曲线样本通过环境变量提供，具体执行合同以日志为准。

5 项跳过：两条需要另行提供 ArcGIS 附件样本的合同，以及三条未显式启用的并发/缓存/位图性能基准。没有把它们记为验收。

GDAL OFF 配置不包含依赖 GDAL 生成临时库的空间错误传播、规模和部分跨后端集成测试；这些继续由各修复对应的 macOS/Linux GDAL ON Release 证据证明，不能称为它们的 sanitizer 验收。没有重跑 TSan；已有 Linux TSan runtime 地址映射失败仍保留为环境限制，Windows 映射及发布合同仍待验证。本次也不代表所有潜在输入均已证明安全。

总计划保持未完成。后续继续空间索引候选完整性、旧格式索引元数据及真实 ArcGIS/Windows 语料门禁。
