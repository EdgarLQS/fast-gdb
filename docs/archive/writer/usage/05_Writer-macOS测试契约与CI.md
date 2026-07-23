> **历史归档**：本文记录已废弃的 Writer 方案或阶段性证据，不代表当前产品能力。fast-gdb 当前仅提供 Reader；现行边界见 [ADR-007](../../../adr/ADR-007-reader-only-gdal-edit-boundary.md)，归档说明见 [Writer 历史索引](../README.md)。

# Writer macOS 测试契约与 CI

本文是计划 18 的统一 macOS 测试入口，冻结 Writer/Index/package consumer 与 Reader 并发、长稳场景的编号、门禁语义和证据格式。M18.2 的稳定会话和安装面仍复用同一 manifest，不另建一套测试编号。

当前只实施 macOS。Linux、Windows 和 50M 阶梯不进入本工作流、不计入完成率，也不得因未执行而标记为通过。

## 1. 固定入口

- 场景 manifest：`tests/contracts/writer-macos-v2.json`
- 统一执行器：`scripts/run_test_contract.py`
- GitHub Actions：`.github/workflows/writer-macos-contract.yml`
- 自动证据目录：`writer-contract-results/`
- 稳定 API ADR：`docs/adr/ADR-001-writer-session-api.md`

CI 使用 Release、GDAL OpenFileGDB 和完整测试目标，构建一次后按 manifest 逐场景执行。小型必选场景默认连续执行 3 次；package consumer 和 1K 观察项各执行 1 次。

## 2. 场景映射

| 场景编号 | Google Test / 命令 | 普通 CI | 主要断言 |
|---|---|---:|---|
| `W-SCHEMA-001` | `WriterTest.W1_OpenExistingTargetsRequestedLayer` | 必须通过 | 精确图层、数量、FID、属性、几何 |
| `W-SCHEMA-002` | `WriterTest.W1_OpenExistingRejectsNonEmptyLayerWithoutModification` | 必须通过 | 非空拒绝、原数据不变、错误信息 |
| `W-SCHEMA-003` | `WriterSessionTest.*` | 必须通过 | 稳定会话生命周期、所有权、错误、回滚、GDAL 重开 |
| `W-FIELD-001` | `WriterTest.W2_*` | 必须通过 | 数值、UTF-8、Binary/XML、GUID、时间、Null、类型错误 |
| `W-GEOM-001` | `T_W01`、`T_W02`、`T_W04` | 必须通过 | 基础面、量化、GDAL 重开 |
| `W-GEOM-002` | `T_W05`–`T_W07` | 必须通过 | Point、Polyline、MultiPoint |
| `W-GEOM-003` | `T_W08`–`T_W16` | 必须通过 | Z/M/ZM 编码矩阵 |
| `W-FAIL-001` | `WriterTest.W4_DiskLimitFailurePreventsAtomicPublish` | 必须通过 | flush 失败、错误锁定、禁止发布 |
| `W-FAIL-002` | `W1_AtomicSession*`、`W1_RowValidation*`、`W1_OpenExistingRejects*` | 必须通过 | 行状态、输入错误、原子发布失败路径 |
| `W-FAIL-003` | 其余 `WriterTest.W4_*` | 必须通过 | flush/close/tablx 等 I/O 失败 |
| `W-INDEX-001` | `IndexCreatorTest.*` | 必须通过 | 空间、属性、联合索引 |
| `W-PKG-001` | 无 GDAL 安装后的 `fast_gdb::writer` consumer | 必须通过 | 稳定头文件、可重定位、无 GDAL/内部头依赖 |
| `W-PKG-002` | GDAL 安装后的稳定 Writer/Index consumer | 必须通过 | 稳定会话、索引助手、GDAL 包配置 |
| `W-PKG-003` | `fast_gdb::writer_legacy` consumer | 必须通过 | 旧代码可编译、兼容头隔离、deprecated 目标 |
| `R-CONC-001` | `TablxCacheTest.ConcurrentAccess` | 必须通过 | 并发缓存结果一致 |
| `R-CONC-002` | `QueryEngineIntegrationTest.ConcurrentIndependentReadersReturnDeterministicResults` | 必须通过 | 8 个独立 QueryEngine 结果一致 |
| `W-GEOM-090` | `PerformanceBenchmarkFixture.W0_ReleaseEvidence_1K` | 只观察 | 1K 正确性和吞吐证据 |
| `W-FAIL-090` | `WriterTest.W1_LargeFileCrosses4GiBAndReopens` | 手工门禁 | 4 GiB、64 位偏移、重开 |
| `W-FAIL-091` | `PerformanceBenchmarkFixture.W10_WriterLongSteady_100KCycles` | 手工门禁 | 1800 秒、median/p95、RSS |
| `R-STABLE-001` | `PerformanceBenchmarkFixture.R7_ReaderLongSteady_100KCycles` | 手工门禁 | fresh-open、结果一致、RSS |

manifest 是机器可读的权威映射。本文只解释边界，修改过滤器或门禁时必须同步更新 manifest。

## 3. 结果和跳过语义

结果只能是：

- `PASS`：本次真实执行且所有声明断言通过；
- `FAIL`：测试失败、命令失败、过滤器没有匹配测试、结果文件损坏，或发生无结构化原因的跳过；
- `SKIP`：未执行或测试主动跳过，并且原因是冻结枚举之一。

允许的跳过原因：

- `missing_gdal`
- `missing_dataset`
- `insufficient_disk`
- `manual_gate_disabled`
- `unsupported_platform`

`SKIP` 永远不计入通过数。普通 macOS CI 中，所有 `required` 场景必须为 `PASS`；`observe` 场景保留真实结果但不阻断发布门禁；`manual` 场景默认写入 `manual_gate_disabled`，不得伪装成自动通过。

## 4. schema v2 证据

每次执行至少生成：

- `manifest.snapshot.json`：本次实际使用的场景清单；
- `execution-manifest.json`：提交、完整命令、结果路径和跳过项；
- `writer-contract-results-v2.json`：完整结构化结果；
- `benchmark_results-v2.csv`：稳定 CSV header；
- 每个场景每次执行的 stdout 和 Google Test JSON。

公共字段包括：场景编号、提交、macOS 版本、架构、编译器、构建类型、GDAL 版本、数据规模、随机种子、缓存状态、open/schema/write/flush/close/index/reopen 分段、total、median、p95、吞吐量、RSS、磁盘、断言范围、结果与跳过原因。

现有测试尚未输出某个分段指标时，该字段保留为空，不得以推算值填充。执行器始终写 `benchmark_results-v2.csv`，不依赖旧 CSV header。

## 5. 本地执行

```bash
brew install ninja gdal

cmake -S . -B build-contract -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DFAST_GDB_BUILD_FULL_TESTS=ON \
  -DBUILD_TESTING=ON
cmake --build build-contract --target gdb_tutorial_test_runner --parallel
```

package consumer 的无 GDAL、GDAL 和 legacy 安装构建完成后，运行：

```bash
python3 scripts/run_test_contract.py \
  --manifest tests/contracts/writer-macos-v2.json \
  --full-test-binary build-contract/bin/gdb_tutorial_test_runner \
  --workspace "$PWD" \
  --output-dir "$PWD/writer-contract-results" \
  --build-type Release \
  --repeat 3
```

只运行指定场景可重复传入 `--scenario`：

```bash
python3 scripts/run_test_contract.py \
  --manifest tests/contracts/writer-macos-v2.json \
  --full-test-binary build-contract/bin/gdb_tutorial_test_runner \
  --output-dir "$PWD/writer-contract-results" \
  --scenario W-SCHEMA-003 \
  --scenario W-PKG-001
```

手工门禁必须同时使用 `--include-manual` 和对应环境变量。例如 4 GiB：

```bash
FAST_GDB_RUN_WRITER_4GB_TEST=1 \
python3 scripts/run_test_contract.py \
  --manifest tests/contracts/writer-macos-v2.json \
  --full-test-binary build-contract/bin/gdb_tutorial_test_runner \
  --output-dir "$PWD/writer-contract-results-4gib" \
  --scenario W-FAIL-090 \
  --include-manual
```

## 6. 阶段出口

M18.1/M18.2 只有在 macOS Release CI 的 required 场景全部通过、小型矩阵连续 3 次一致、没有无原因 SKIP，并保留本次 manifest、命令和 schema-v2 JSON/CSV 后才能记为当前验收完成。

4 GiB、10M、磁盘真实写满和 1800 秒长稳仍是显式手工门禁。历史记录、短时 smoke、推算值或 SKIP 均不能替代当前验收。GitHub Actions 无法启动 runner 的外部问题单独记录，不得误记为产品测试失败或通过。
