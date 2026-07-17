# 19 — M18 正式收口与 Reader 10M fresh-open 性能优化计划

- **更新日期**：2026-07-17
- **状态**：待执行
- **当前基线**：`main@9dd7edf`
- **执行顺序**：M18 收口 → Reader 10M 性能
- **当前操作**：仅编写计划，尚未开始实施

## 1. 目标与原则

本计划承接已经 squash 合入 `main` 的 M18 Writer 变更，先完成合并后的状态同步、正式 CI
证据和远端收口，再处理 Reader 在 macOS 上 Point、MultiPoint、Polyline 三类 10M
数据的 fresh-open 性能失败档。

执行时遵守以下原则：

- M18 收口和 Reader 性能优化使用两个串行分支，分别审核、验收和合入；
- Writer 本地代码门禁、GitHub Actions 正式 artifact 和项目整体发布验收分别判定；
- 性能优化以正确性为前提，FID 集合或几何判断不一致时，性能结果无效；
- 不通过调整阈值、减少精确判断或隐藏 `SKIP` 获得表面通过；
- Linux、Windows、50M、35GB/5 亿、原生曲线和 MultiPatch 继续 Deferred；
- 测试数据、构建目录、profile 原始文件和临时输出不得误提交；
- 每项优化必须有独立基线、profile 依据和可归因的前后对比。

## 2. 当前基线与已知状态

计划开始前固定以下事实：

- 本地和远端目标分支为 `main`；
- M18 squash 提交为 `9dd7edf`；
- 原本地分支 `codex/m18-1-macos-test-contract-ci` 已删除；
- 远端仍可能保留 `origin/codex/m18-1-macos-test-contract-ci`；
- PR #11、Issue #12 和 GitHub Actions 状态必须在执行时重新查询，不能沿用旧文档推断；
- M18 的 Append、Update、Delete、Transaction 和 Recovery 已进入 `main`；
- 当前正式验收缺口是 main 级 GitHub Actions artifact，而不是重新实现高级编辑；
- Reader 10M fresh-open 已有 2026-07-15 基线，正确性通过但部分性能档失败。

本地 Reader 10M fixture 固定为：

| 几何 | 路径 | 规模 |
|---|---|---:|
| Point | `test_data/spatial_matrix/point_10000000.gdb` | 10M |
| MultiPoint | `test_data/spatial_matrix/multipoint_10000000.gdb` | 10M |
| Polyline | `test_data/spatial_matrix/line_10000000.gdb` | 10M |

## 3. 分支和交付组织

### 3.1 第一分支：M18 正式收口

分支名：

```text
codex/m18-main-closeout
```

该分支只处理：

- M18 合并后文档状态同步；
- main 上的 Writer 本地复验；
- GitHub Actions 正式证据；
- PR #11、Issue #12 和远端旧分支收口；
- 必要且可复现的 Writer workflow 最小修复。

不得在该分支进行 Reader 算法优化或新增 Writer 功能。

### 3.2 第二分支：Reader fresh-open 性能

分支名：

```text
codex/reader-fresh-open-followup
```

该分支必须从第一分支完成并合入后的最新 `main` 创建，只处理：

- 三类 10M fixture 的 fresh-open 基线和复现工具；
- profile 证明的 Reader 热点；
- 内部候选读取、geometry-only scan 或几何快速路径优化；
- current/main/GDAL 对比和性能证据。

不得在该分支修改 Writer 公共 API 或重新解释 M18 验收结论。

### 3.3 合入策略

- 两个分支串行执行，不并行修改共同文档；
- 分支内部按可验证单元保留提交；
- 每个分支完成审核后分别 squash 合入 `main`；
- M18 收口可以独立完成，不因 Reader 性能优化周期延长而阻塞；
- Reader 未达到性能门禁时，不影响已经成立的 M18 Writer 结论。

## 4. 阶段一：M18 正式收口

### 4.1 开始前检查

创建分支前执行：

```bash
git status --short
git branch --show-current
git log -1 --oneline
git rev-list --left-right --count origin/main...main
git diff --check
```

必须满足：

- 当前分支为 `main`；
- 工作区干净；
- 本地与远端 `main` 没有未处理分叉；
- `HEAD` 包含 M18 squash 结果；
- 不存在进行中的 merge、rebase 或 cherry-pick。

比较 `main` 与远端原 M18 分支的最终树：

```bash
git diff --stat main origin/codex/m18-1-macos-test-contract-ci
git diff --check main origin/codex/m18-1-macos-test-contract-ci
```

提交历史不同是 squash 合并的预期结果，但最终文件树不得存在未解释的代码差异。若存在真实差异，立即停止远端分支删除，先形成差异清单并判断是否遗漏变更。

### 4.2 GitHub 状态核实

通过已登录的 GitHub 页面、Connector 或可用 CLI 查询：

- PR #11 是否仍为 Open/Draft；
- PR #11 的 head、base 和当前 diff；
- Issue #12 是否仍为 Open；
- `main@9dd7edf` 推送后触发了哪些 workflow；
- workflow 是否进入 checkout、是否存在 steps 和 logs；
- 是否已经产生当前提交对应的 artifact；
- 远端原 M18 分支是否仍存在。

处理规则：

- PR #11 若仍打开且代码树已由 `main` 覆盖，关闭 PR，并注明 squash 提交 `9dd7edf`；
- PR #11 若已关闭，不重复操作，只把最终状态写入验收记录；
- Issue #12 只有在 Actions 可以正常执行并产生 steps、logs 和 artifact 后才能关闭；
- Actions 仍在 checkout 前失败时，Issue #12 保持 Open；
- 删除远端旧分支前必须确认其最终树已被 `main` 覆盖；
- GitHub 状态无法读取时，不猜测 PR、Issue 或 workflow 已完成。

### 4.3 当前文档状态同步

重点检查并同步：

- `docs/planning/00_规划文档状态索引.md`；
- `docs/planning/01_项目状态与规划.md`；
- `docs/planning/18_writer跨平台测试统一与后续编辑计划.md`；
- `docs/planning/18_writer执行进度.md`；
- `docs/roadmap/writer-roadmap.md`；
- `docs/overview/01_fast-gdb项目介绍与当前状态.md`；
- `docs/architecture/writer-known-limitations.md`。

统一修改为：

- 当前实现基线为 `main@9dd7edf` 或执行时更新后的 main 收口提交；
- M18 原功能分支已经 squash 合入主分支；
- Append、Update、Delete、Transaction 和 Recovery 均为实现完成；
- 删除“清零 Update Major”“实现 Delete”“设计 Transaction”“实现 Recovery”等过期待办；
- 高级编辑不再描述为“尚未实现”或“仍不支持”；
- 本地门禁与 GitHub Actions 正式验收分别描述；
- 正式 artifact 缺失时，只能写“代码已合入，正式验收 Blocked”；
- 不把 M18 macOS 完成扩展为 Linux、Windows 或整个项目全面完成；
- 不把合成性能、fresh-open 和 strict-cold 结果混写为同一结论。

全仓库搜索状态漂移：

```bash
rg -n \
  "codex/m18-1-macos-test-contract-ci|PR #11|373bb265|保持 Draft|Request Changes|高级编辑.*未完成|事务仍不支持|清零 Update|Delete.*尚未" \
  README.md docs
```

历史 self-review 保留原始结论，不重写历史证据。容易被误认为当前状态的历史文档只增加“历史快照，当前状态以 main 验收记录为准”的说明。

### 4.4 GDAL ON Release 验证

使用全新临时目录：

```bash
cmake -S . -B /tmp/fast-gdb-m18-gdal \
  -DCMAKE_BUILD_TYPE=Release \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_CURVE_BACKEND=BUILTIN \
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DFAST_GDB_BUILD_FULL_TESTS=ON \
  -DBUILD_TESTING=ON

cmake --build /tmp/fast-gdb-m18-gdal --parallel
ctest --test-dir /tmp/fast-gdb-m18-gdal --output-on-failure
```

要求完整 CTest 通过。若 Writer 专项通过但 Reader 存在失败，只能分别报告，不能声明完整仓库门禁通过。

### 4.5 GDAL OFF Release 验证

```bash
cmake -S . -B /tmp/fast-gdb-m18-nogdal \
  -DCMAKE_BUILD_TYPE=Release \
  -DFAST_GDB_WITH_GDAL=OFF \
  -DFAST_GDB_CURVE_BACKEND=BUILTIN \
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DFAST_GDB_BUILD_FULL_TESTS=ON \
  -DBUILD_TESTING=ON

cmake --build /tmp/fast-gdb-m18-nogdal --parallel
ctest --test-dir /tmp/fast-gdb-m18-nogdal --output-on-failure
```

无 GDAL 配置必须确认稳定 Writer 核心仍可构建和消费，GDAL-only 头不会进入稳定安装面。

### 4.6 Writer 合同连续执行

依次运行：

- `tests/contracts/writer-macos-v2.json`；
- `tests/contracts/writer-append-macos-v1.json`；
- `tests/contracts/writer-update-macos-v1.json`；
- `tests/contracts/writer-delete-macos-v1.json`；
- `tests/contracts/writer-transaction-macos-v1.json`；
- `tests/contracts/writer-macos-performance-v1.json` 对应的性能流程。

功能合同统一执行三次：

```bash
python3 scripts/run_test_contract.py \
  --manifest <manifest> \
  --full-test-binary /tmp/fast-gdb-m18-gdal/bin/gdb_tutorial_test_runner \
  --workspace "$PWD" \
  --output-dir <独立输出目录> \
  --build-type Release \
  --repeat 3
```

验收要求：

- required 场景全部 PASS；
- 三次执行结果一致；
- 无无原因 `SKIP`；
- `SKIP` 必须包含结构化原因且不计入通过数；
- JSON、CSV、manifest 快照和环境元数据完整；
- Recovery 覆盖旧命名、损坏源、损坏 backup、伪造候选、歧义候选、错误动作和发布后验证失败。

### 4.7 安装面和 package consumer

#### 无 GDAL 安装

确认：

- 稳定目录包含 `writer_session.h` 和 `writer_recovery.h`；
- 不包含 GDAL-only Index、Append、Update、Delete 和 Transaction 头；
- legacy 物理布局头只安装到 `writer_legacy`；
- 无 GDAL consumer 编译并运行；
- legacy consumer 编译并运行。

#### GDAL 安装

稳定目录应包含：

- `writer_session.h`；
- `writer_recovery.h`；
- `writer_index.h`；
- `writer_append.h`；
- `writer_update.h`；
- `writer_delete.h`；
- `writer_transaction.h`。

确认内部 `row_buffer`、`tablx_writer`、`gdb_table_writer` 等物理布局头没有泄露到稳定目录。GDAL consumer 必须覆盖 Index、Append 和 Transaction 编译与运行。

### 4.8 GitHub Actions 正式验收

核实或手工触发：

1. `writer-macos-contract`；
2. `writer-append-macos`；
3. `writer-update-macos`；
4. `writer-delete-macos`；
5. `writer-transaction-macos`；
6. `writer-macos-performance`。

性能 workflow 参数固定为：

- `samples=3`；
- `profile_duration_seconds=5`；
- `run_reader_10m=false`。

每个 workflow 检查：

- checkout 实际执行；
- Ninja Release 构建通过；
- required 合同连续三次通过；
- package consumer 实际运行；
- artifact 名称绑定当前 SHA；
- artifact 内含环境、manifest、日志和结果；
- 正确性失败时性能结果被判无效；
- `continue-on-error` 没有把比较失败伪装成 workflow 通过。

仓库内 workflow 配置存在可复现错误时，只做最小修复并重新运行。若仍是账户、runner 或 GitHub 平台问题，不修改业务代码，保留 Issue #12 并记录阻塞证据。

### 4.9 M18 正式验收记录

新增：

```text
docs/evidence/M18-writer-main-acceptance-2026-07-17.md
```

记录：

- main SHA；
- macOS、架构、编译器、CMake、GDAL 和生成器；
- GDAL ON/OFF 构建命令；
- 完整 CTest 结果；
- 六套合同结果；
- package consumer 结果；
- workflow run 链接和 artifact 名称；
- current/main/GDAL 性能摘要；
- raw profile；
- publish/rollback/cleanup 故障验证；
- `SKIP` 清单；
- Deferred 和自动化未覆盖项；
- 最终判定。

判定只允许：

- `Accepted`：本地和正式 artifact 全部齐全；
- `Code accepted / Formal acceptance blocked`：代码门禁通过，但 GitHub artifact 缺失；
- `Rejected`：存在正确性、恢复安全、构建、安装消费或性能回退失败。

### 4.10 阶段一提交和远端收口

分支内部建议提交：

1. `docs: sync M18 status after main squash merge`；
2. `ci: fix M18 acceptance workflow`，仅实际需要时创建；
3. `docs: record M18 main acceptance evidence`。

完成审核后 squash 合入 `main`。确认 `main` 和正式证据安全后：

- 关闭已被 squash 替代的 PR #11；
- Issue #12 根据 Actions 是否恢复决定关闭或保留；
- 删除远端 `codex/m18-1-macos-test-contract-ci`；
- 再次确认远端分支不存在且 `main` 没有分叉。

## 5. 阶段二：Reader 10M fresh-open 性能

### 5.1 创建性能分支

阶段一合入后，从最新 `main` 创建 `codex/reader-fresh-open-followup`，记录基线 SHA。性能比较中的 baseline 必须固定为该提交，不得在优化过程中漂移。

### 5.2 测试环境固定

每次测量记录：

- current 和 baseline SHA；
- macOS 版本和架构；
- CPU、内存；
- Apple Clang、CMake 和 GDAL 版本；
- Release 构建选项；
- fixture 路径和目录大小；
- `fresh-open` 模式；
- 是否清理 OS page cache；
- 采样次数和执行顺序；
- profile 是否开启。

默认使用 fresh-open，但不清理 OS page cache，因此必须明确写为“fresh-open，非 strict-cold”。

### 5.3 优化前 15 档基线

三个几何分别运行：

```bash
FAST_GDB_RUN_SPATIAL_BENCHMARKS=1 \
FAST_GDB_BENCHMARK_PATH="<gdb-path>" \
FAST_GDB_BENCHMARK_LABEL="macOS / <geometry> / 10m / fresh-open" \
FAST_GDB_BENCHMARK_MODE=fresh-open \
FAST_GDB_BENCHMARK_TRIALS=5 \
./build/bin/gdb_tutorial_test_runner \
  --gtest_filter='SpatialDensityBenchmark.DensityMatrixConfigured'
```

每类几何覆盖：

- 1%；
- 10%；
- 30%；
- 80%；
- 100%。

每档保存：

- fast-gdb median 和 p95；
- GDAL median 和 p95；
- fast/GDAL 比值；
- result count；
- 完整 FID 对照；
- `invalid_geometries`；
- execution path；
- candidate count/ratio；
- `.spx` lookup、geometry scan、decode 和 predicate 阶段耗时。

优化前输出必须单独保存，后续结果不得覆盖基线。

### 5.4 性能比较工具扩展

扩展 `scripts/run_spatial_regression.py`：

```text
--mode steady-state|fresh-open
--dataset <label>=<gdb-path>
--trials <1-100>
--max-regression <ratio>
--output <directory>
```

`--dataset` 支持重复传入，一次运行 Point、MultiPoint 和 Polyline。

行为要求：

- current 和 baseline 交替执行，降低温度和缓存单向偏差；
- 每个数据集运行五个覆盖率；
- fresh-open 时显式设置 benchmark mode；
- 非 strict-cold 状态写入输出；
- 检查完整 FID 集合，而不只比较数量；
- `invalid_geometries != 0` 立即失败；
- current 相对 baseline 回退超过 5% 时失败；
- 输出 CSV、JSON、原始日志和环境摘要；
- 任一子场景失败时返回非零；
- 保持现有参数和 POSIX warm regression 调用兼容。

### 5.5 第一轮 profile

对当前失败档启用：

```bash
FAST_GDB_SPATIAL_PROFILE=1
FAST_GDB_BENCHMARK_TRIALS=1
FAST_GDB_BENCHMARK_CASE=<coverage>
```

优先顺序：

1. Point 10%；
2. Point 30%；
3. Point 80%；
4. Point 100%；
5. MultiPoint 10%；
6. MultiPoint 30%；
7. Polyline 30%。

至少拆分：

- catalog scan；
- table/tablx open；
- `.spx` open/mmap；
- `.spx` candidate lookup；
- candidate FID 到物理记录定位；
- geometry blob 读取；
- geometry decode；
- bbox 和精确判断；
- FID 收集、排序和去重。

只有可重复且约占总耗时 20% 以上的阶段才进入优化。profile 与历史推测冲突时，以当前测量为准。

### 5.6 优化 A：Point 10% 候选路径

候选阶段为主要热点时：

- 检查 candidate vector 的复制、排序和去重；
- 检查 `.gdbtablx` 映射是否逐 FID 重复查找；
- 优先复用批量 candidate scanner；
- Point 只解析空间判断需要的 XY；
- 保留损坏 `.spx` 时的安全 fallback；
- 不改变 FID 语义。

保留条件：

- Point 10% median 稳定改善至少 5%；
- FID 集合完全一致；
- 其他 Point 档位无超过 5% 回退；
- MultiPoint、Polyline 正确性不变。

### 5.7 优化 B：Point geometry-only 密集扫描

30%–100% 主要走顺序扫描时：

- 为 Point、PointZ、PointM、PointZM 增加最小 XY 解码路径；
- 跳过查询无关属性、WKT 和完整模型构造；
- 遇到未知编码、截断或非法 blob 时进入完整解析或错误路径；
- 不修改公开 GeometryModel；
- 不把 bbox 候选直接当成最终结果。

保留条件：

- 目标档至少改善 5%；
- 失败档达到既定门禁，或明显缩小差距且没有回退；
- Point 全部五档正确；
- 非法几何仍被报告；
- 完整几何解析测试通过。

### 5.8 优化 C：MultiPoint/Polyline

只有 profile 证明通用几何扫描、解码或精确判断为热点时才实施：

- 复用 geometry blob 的零拷贝读取；
- 减少临时容器和完整模型构造；
- bbox 已能确定结果时避免多余拓扑步骤；
- 边界相交、曲线、Z/M 和异常编码继续走完整模型；
- `.spx` 仍只生成候选，不能替代精确判断。

保留条件：

- 目标档 median 稳定改善至少 5%；
- FID 与 GDAL 完全一致；
- Polyline 边界相交测试通过；
- 其他几何和覆盖率无超过 5% 回退。

### 5.9 禁止的性能改动

- 没有 profile 证据就调整 29% 或其他规划阈值；
- 放宽 `GDAL + 200ms` 或 0.9 倍门禁；
- 删除精确空间判断；
- 静默过滤错误记录；
- 把 fresh-open 写成 strict-cold；
- 根据 Point 结果直接外推 MultiPoint/Polyline；
- 一次提交多个无法独立衡量的优化；
- 为性能引入新的 GDAL 主路径依赖。

### 5.10 每项优化验证循环

每项候选优化依次执行：

1. 目标场景 3 次快速测量；
2. 目标场景 5 次正式测量；
3. 同几何五个覆盖率；
4. 三类几何完整正确性矩阵；
5. current/main 5% 回退比较；
6. Reader 专项测试；
7. GDAL ON/OFF 完整 CTest。

只有目标改善稳定、正确性不变、其他场景无明显回退时才保留。无收益或产生回退的实验必须撤销，不进入最终分支。

### 5.11 Reader 最终验收

最终 current 和 baseline 交替执行，每个场景至少 10 次，报告 median 和 p95。

正确性要求：

- 15/15 场景完整 FID 集合与 GDAL 一致；
- `invalid_geometries=0`；
- result count 一致；
- `.spx` 缺失或损坏时 fallback 正确；
- Point、MultiPoint、Polyline 和 Z/M 回归通过。

性能门禁：

- 1% 和 10%：`fast-gdb <= GDAL + 200ms`，或 `fast-gdb <= 0.9 × GDAL`；
- 30%、80% 和 100%：`fast-gdb <= 0.9 × GDAL`；
- current 相对固定 baseline：每档回退不超过 5%。

目标为 15/15 通过。仍有失败档时：

- 不降低门禁；
- 分支不标记性能验收通过；
- 保留正确性和 profile 证据；
- 列出失败几何、覆盖率、差距和下一热点；
- 结论保持“正确性通过、性能未验收”。

### 5.12 Reader 测试范围

至少执行：

- GDAL ON 完整 CTest；
- GDAL OFF 完整 CTest；
- `SpatialQueryAdaptiveTest` 连续三次；
- `SpatialDensityBenchmark` 正确性路径；
- geometry model/parser；
- `.spx` parser；
- `.gdbtablx` 映射和缓存；
- 损坏索引 fallback；
- Point/MultiPoint/Polyline 空间判断；
- 多 QueryEngine 独立并发测试。

脚本检查：

```bash
python3 -m py_compile scripts/run_spatial_regression.py
python3 scripts/run_spatial_regression.py --help
git diff --check
```

### 5.13 Reader 性能证据

保留 2026-07-15 原始记录，新建：

```text
docs/evidence/reader-fresh-open-macos-2026-07-17.md
```

记录：

- 优化前后 SHA；
- 环境和 fixture；
- 15 档优化前后表；
- current/main/GDAL 对比；
- median、p95 和比值；
- profile 热点；
- 保留和撤销的优化；
- 正确性与完整 CTest；
- fresh-open 非 strict-cold 边界；
- Deferred；
- 最终判定。

同步更新技术性能文档和当前状态入口，但只写测量能够支持的结论。

### 5.14 阶段二提交

建议分支内部提交：

1. `test: add reproducible fresh-open current-main comparison`；
2. `perf: optimize measured spatial candidate path`；
3. `perf: optimize measured geometry scan path`；
4. `docs: record Reader 10M fresh-open acceptance`。

没有收益的实验不得保留在最终提交历史。审核通过后 squash 合入 `main`。

## 6. 公共接口和兼容性

本计划默认：

- 不新增 Writer 公共 API；
- 不修改 `WriterErrorCode` 数值和稳定名称；
- 不改变 Writer 安装头布局；
- 不改变 `QueryEngine` 公共查询语义；
- 不改变 FID 映射；
- 不改变 `.spx` 候选加精确判断模型；
- 不改变 GDAL fallback 的触发和报告方式；
- Reader 优化限定在内部实现。

计划内唯一明确的工具接口扩展是 `scripts/run_spatial_regression.py`，新增参数必须向后兼容。

## 7. 验收判定

### 7.1 M18

- Writer 正确性、恢复覆盖、Release 构建或安装消费存在 P0：`不可提交`；
- 本地通过但 GitHub artifact 缺失：`代码已收口，正式验收 Blocked`；
- 六个 workflow、合同、安装消费和 artifact 全部通过：`M18 macOS Accepted`。

### 7.2 Reader 性能

- FID 或非法几何结果不一致：`不可提交`；
- 正确性通过但仍有性能失败档：`正确性通过、性能未验收`；
- 15 档全部达标且 current 无超过 5% 回退：`Reader 10M macOS fresh-open 性能验收通过`。

### 7.3 整体边界

两个阶段完成后仍不自动代表以下范围通过：

- Linux Writer；
- Windows Writer；
- 50M；
- 35GB/5 亿真实生产数据；
- 原生曲线写入；
- 完整 MultiPatch；
- 跨平台绝对性能一致性。

## 8. 最终报告格式

每个阶段结束时按以下结构报告：

### 计划

- 原计划目标；
- 实际执行边界；
- 是否发生范围调整。

### 执行

- 修改内容；
- 分支和提交；
- GitHub 操作；
- 保留或撤销的性能优化。

### 验证

- 构建和测试命令；
- PASS、FAIL、SKIP；
- artifact 和性能数据；
- 验收判定；
- 预先存在但未处理的问题；
- 自动化未覆盖风险；
- 文档状态漂移检查结果。
