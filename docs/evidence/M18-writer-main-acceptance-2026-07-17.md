# M18 Writer main 正式验收记录（2026-07-17）

## 1. 范围

本记录覆盖 M18 Writer 正式收口。Reader 10M fresh-open 后续工作与本记录分开判定，但按用户要求继续保留在同一开发分支 `codex/m18-main-closeout`，最终统一审核和合并。

Reader 工具与静态审查记录见：`docs/evidence/reader-fresh-open-followup-static-2026-07-17.md`。

## 2. 基线

- M18 squash 提交：`9dd7edf73763b56d84677c8a246bc85f80a1a0c1`
- 收口开始时 main：`42d8f76620a8c39eeb8523a0f84fcde0eb719f01`
- 收口分支：`codex/m18-main-closeout`
- PR #11：Closed，已注明由 squash 提交替代
- Issue #12：Open
- PR #11 head SHA：`6b8d3d5565246be5cf306b4d95a4759455bdcbbe`

## 3. GitHub 状态证据

### PR #11

PR #11 已关闭。其正文已明确说明 M18 Writer 实现由 `main` 上的 squash 提交 `9dd7edf` 覆盖，原 Draft / Request Changes 仅作为 Actions 在 checkout 前失败的历史上下文。

关闭 PR 不等于正式验收通过；正式 artifact 仍由 Issue #12 和当前 PR #13 跟踪。

### Issue #12

Issue 仍为 Open，记录的根因类别是 Actions job 在任何 step 开始前失败，历史和当前观测均出现 `steps=null` 且无可下载日志。

### Actions

PR #13 当前分支提交触发了 Writer、Reader、release 和 geometry workflows，但 job 仍在 checkout 前失败，没有 steps、logs 或 artifact。失败重跑也未形成可审计运行结果。

### 远端旧分支

原 M18 分支按名称已无法通过分支搜索检索。PR #11 仍保留历史 head SHA，且当前连接器不能执行本地 tree-to-tree diff；因此不把“分支搜索为空”外推为最终树等价证明。

## 4. 本地验证证据状态

随后已在 macOS arm64 本地 checkout 对 `f9d5a1b7` 完成验收，证据保存在 `local-acceptance/20260717-135920/`：

| 验证项 | 要求 | 本次状态 |
|---|---|---|
| GDAL ON Release | 配置、构建、完整 CTest | 573/573，0 失败；69 个可解释 optional SKIP |
| GDAL OFF Release | 配置、构建、完整 CTest | 272/272，0 失败、0 SKIP |
| Writer 基础合同 | 连续 3 次 | required 全部 3/3 PASS；3 个 manual gate SKIP |
| Append 合同 | 连续 3 次 | 5 个场景全部 3/3 PASS |
| Update 合同 | 连续 3 次 | 5 个场景全部 3/3 PASS |
| Delete 合同 | 连续 3 次 | 5 个场景全部 3/3 PASS |
| Transaction/Recovery 合同 | 连续 3 次 | 6 个场景全部 3/3 PASS |
| 性能合同 | samples=3、profile=5s、reader10m=false | 5% 门禁 PASS，最大回退 3.332%；两个 profile 完成，Point-XYZM 以 2000 万行取得有效 sample |
| no-GDAL consumer | 编译并运行 | PASS |
| legacy consumer | 编译并运行 | PASS |
| GDAL consumer | Index/Append/Transaction 编译并运行 | PASS |

## 5. 六个 macOS artifacts

以下 artifacts 均未获得绑定当前收口 SHA 的可验证实例：

1. `writer-macos-contract-<sha>`
2. `writer-append-macos-<sha>`
3. `writer-update-macos-<sha>`
4. `writer-delete-macos-<sha>`
5. `writer-transaction-macos-<sha>`
6. `writer-macos-performance-<sha>`

## 6. SKIP、故障注入与未覆盖项

- 本地 CTest 与合同 SKIP 已保存在证据目录；required 场景无异常 SKIP。
- publish/rollback/cleanup 故障注入无本次新证据。
- Recovery 的损坏源、损坏 backup、伪造候选、歧义候选、错误动作与发布后验证失败没有本次新 artifact。
- Linux、Windows、50M、35GB/5 亿、原生曲线与 MultiPatch 继续 Deferred。
- Reader 10M fresh-open 本地性能门禁已完成，正式 artifact 仍缺失。

## 7. 最终判定

**Code accepted / Formal acceptance blocked**

理由：本地 GDAL ON/OFF Release、完整 CTest、五套 Writer 合同、三类 package consumer、性能和 raw profile 已完成；六个 GitHub Actions artifacts 与人工故障注入仍不齐全。

在上述证据齐全前，禁止标记 `Accepted`；Issue #12 保持 Open。当前统一开发 PR #13 保持 Draft，不提前合并。
