# 19 — M18 正式收口与 Reader 10M fresh-open 性能优化计划

- **更新日期**：2026-07-17
- **状态**：执行中
- **当前基线**：`main@42d8f76620a8c39eeb8523a0f84fcde0eb719f01`
- **执行分支**：`codex/m18-main-closeout`
- **执行顺序**：M18 收口 → Reader 10M 性能 → 统一审核 → 最终合并
- **当前操作**：阶段一文档收口已完成；阶段二工具扩展已开始

## 1. 目标与原则

本计划承接已经 squash 合入 `main` 的 M18 Writer 变更，在同一个开发分支中先完成合并后的状态同步、正式 CI 证据和远端收口，再处理 Reader 在 macOS 上 Point、MultiPoint、Polyline 三类 10M 数据的 fresh-open 性能失败档，全部完成后统一审核并合入 `main`。

执行时遵守以下原则：

- M18 收口和 Reader 性能优化在 `codex/m18-main-closeout` 上串行实施，最后统一审核和合并；
- Writer 本地代码门禁、GitHub Actions 正式 artifact 和项目整体发布验收分别判定；
- 性能优化以正确性为前提，FID 集合或几何判断不一致时，性能结果无效；
- 不通过调整阈值、减少精确判断或隐藏 `SKIP` 获得表面通过；
- Linux、Windows、50M、35GB/5 亿、原生曲线和 MultiPatch 继续 Deferred；
- 测试数据、构建目录、profile 原始文件和临时输出不得误提交；
- 每项优化必须有独立基线、profile 依据和可归因的前后对比。

## 2. 当前基线与已知状态

- M18 squash 提交为 `9dd7edf73763b56d84677c8a246bc85f80a1a0c1`；
- 收口开始时 `main` 为 `42d8f76620a8c39eeb8523a0f84fcde0eb719f01`；
- PR #11 已作为 squash 后的历史交付入口关闭；
- Issue #12 继续跟踪 GitHub Actions 在 step 前失败的问题；
- Append、Update、Delete、Transaction 和 Recovery 已进入 `main`；
- M18 当前正式验收缺口是 main/收口分支级 GitHub Actions artifact，而不是重新实现高级编辑；
- Reader 10M fresh-open 已有 2026-07-15 基线，正确性通过但部分性能档失败；
- 当前工作统一保留在 `codex/m18-main-closeout`，不提前合并。

本地 Reader 10M fixture 固定为：

| 几何 | 路径 | 规模 |
|---|---|---:|
| Point | `test_data/spatial_matrix/point_10000000.gdb` | 10M |
| MultiPoint | `test_data/spatial_matrix/multipoint_10000000.gdb` | 10M |
| Polyline | `test_data/spatial_matrix/line_10000000.gdb` | 10M |

## 3. 单分支交付组织

### 3.1 分支

```text
codex/m18-main-closeout
```

该分支依次处理：

1. M18 合并后文档状态同步；
2. main 上的 Writer 本地复验和 GitHub Actions 正式证据；
3. PR #11、Issue #12 和远端旧分支收口；
4. Reader 三类 10M fresh-open 基线、profile、优化和回归；
5. `main...HEAD` 统一 P0/P1/P2 审核；
6. 所有可验收证据齐全后最终合入 `main`。

不得在证据不足时把 Writer 或 Reader 标记为正式 Accepted，也不得提交测试数据、构建目录或 profile 原始临时文件。

### 3.2 当前进度

- M18 当前状态文档已同步；
- PR #11 已关闭，Issue #12 保持 Open；
- M18 正式验收记录已建立，判定为 `Code accepted / Formal acceptance blocked`；
- `scripts/run_spatial_regression.py` 已扩展多数据集、fresh-open、交替执行、JSON/CSV/环境摘要及正确性检查；
- 已增加回归工具参数辅助函数测试；
- Reader 算法优化尚未在没有真实 profile 的情况下实施。

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

比较 `main` 与远端原 M18 分支的最终树。提交历史不同是 squash 合并的预期结果，但最终文件树不得存在未解释的代码差异。

### 4.2 GitHub 状态核实

- PR #11 已关闭并明确由 squash 提交替代；
- Issue #12 在 Actions 能正常执行并产生 steps、logs 和 artifact 前保持 Open；
- 无法读取或验证的 GitHub 状态不得猜测为完成；
- 远端旧分支仅在最终树覆盖得到可靠证明后清理。

### 4.3 当前文档状态同步

当前入口统一表达：

- 实现基线为 `main@42d8f76`，M18 squash 为 `9dd7edf`；
- Append、Update、Delete、Transaction 和 Recovery 均已实现；
- 本地代码门禁与 GitHub Actions 正式验收分别描述；
- 正式 artifact 缺失时只允许 `Code accepted / Formal acceptance blocked`；
- 不把 M18 macOS 实现扩展为 Linux、Windows 或整个项目全面完成；
- 不把合成性能、fresh-open 和 strict-cold 混写。

### 4.4 GDAL ON/OFF、CTest、Writer 合同和 package consumer

正式验收仍要求：

- GDAL ON Release 完整构建与 CTest；
- GDAL OFF Release 完整构建与 CTest；
- Writer 基础、Append、Update、Delete、Transaction/Recovery 五套功能合同各连续三次；
- 性能合同固定 `samples=3`、`profile_duration_seconds=5`、`run_reader_10m=false`；
- no-GDAL、legacy、GDAL 三类 package consumer 编译并运行；
- required 场景全部 PASS，无无原因 `SKIP`。

### 4.5 GitHub Actions 正式验收

需要六个绑定当前 SHA 的 artifact：

1. `writer-macos-contract`；
2. `writer-append-macos`；
3. `writer-update-macos`；
4. `writer-delete-macos`；
5. `writer-transaction-macos`；
6. `writer-macos-performance`。

每个 workflow 必须实际完成 checkout、Release 构建、合同、consumer 和 artifact 上传。仓库 workflow 可复现错误只做最小修复；账户、runner 或平台问题继续由 Issue #12 跟踪。

## 5. 阶段二：Reader 10M fresh-open 性能

### 5.1 基线和环境

固定 baseline 为本分支创建时的 `main@42d8f76`。每次测量记录 current/baseline SHA、macOS 和架构、CPU/内存、Apple Clang、CMake、GDAL、Release 选项、fixture、fresh-open 模式、采样次数和 profile 状态。

默认 fresh-open，但不清理 OS page cache，必须写为“fresh-open，非 strict-cold”。

### 5.2 15 档基线

Point、MultiPoint、Polyline 各覆盖 1%、10%、30%、80%、100%，每档保存：

- fast-gdb median/p95；
- GDAL median/p95；
- fast/GDAL 比值；
- result count 和完整 FID 对照；
- `invalid_geometries`；
- execution path 和候选信息；
- `.spx`、geometry scan、decode、predicate 分段耗时。

### 5.3 性能比较工具

`scripts/run_spatial_regression.py` 支持：

```text
--mode steady-state|fresh-open
--dataset <label>=<gdb-path>
--trials <1-100>
--max-regression <ratio>
--output <directory>
```

已实现重复 `--dataset`、current/main 交替执行、CSV/JSON/环境摘要、非 strict-cold 标记、`invalid_geometries` 失败、result count 对照、可用时 FID 签名对照、5% 回退门禁，并兼容原 `--gdb` 调用。

### 5.4 Profile 与优化准入

优先 profile：Point 10%、30%、80%、100%，MultiPoint 10%、30%，Polyline 30%。至少拆分 catalog、table/tablx open、`.spx` open/mmap、candidate lookup、FID 定位、blob 读取、decode、bbox/predicate、结果收集排序去重。

只有可重复且约占总耗时 20% 以上的阶段进入优化。没有当前 profile 证据时禁止提交算法优化。

### 5.5 候选优化

- Point 10%：仅在候选阶段为主要热点时优化候选复制、排序去重、tablx 定位和最小 XY 解析；
- Point 30%–100%：仅在顺序扫描为热点时增加安全的 Point/PointZ/PointM/PointZM 最小 XY 路径；
- MultiPoint/Polyline：仅在 profile 证明通用 scan/decode/predicate 为热点时优化；
- `.spx` 始终只生成候选，不能替代精确判断；
- 未知、截断或非法编码必须回退完整解析或明确报错。

每项优化必须满足目标场景稳定改善至少 5%、完整 FID 一致、`invalid_geometries=0`、其他档位无超过 5% 回退，否则撤销。

### 5.6 最终验证

1. 目标快速测量 3 次；
2. 正式测量 5 次；
3. 同几何五档；
4. 三类几何完整正确性矩阵；
5. current/main 5% 回退；
6. Reader 专项；
7. GDAL ON/OFF 完整 CTest。

## 6. 最终验收和合并

最终记录必须同时列出 Writer 正式证据、Reader 15 档 current/main/GDAL 对比、profile、保留和撤销的优化、SKIP、Deferred、预存问题和未覆盖项。

判定：

- `Accepted`：计划内本地验证和正式 artifacts 全部齐全；
- `Code accepted / Formal acceptance blocked`：代码门禁成立但正式 artifact 缺失；
- `Rejected`：存在正确性、恢复安全、构建、安装消费或不可接受的性能回退。

最终只在统一 `main...HEAD` P0/P1/P2 审核无阻塞问题、证据结论一致且没有测试数据或构建产物时合入 `main`。
