# M18 Writer main 正式验收记录（2026-07-17）

## 1. 范围

本记录仅覆盖“阶段一：M18 正式收口”。Reader 10M fresh-open 性能优化明确留待后续分支 `codex/reader-fresh-open-followup`。

## 2. 基线

- M18 squash 提交：`9dd7edf73763b56d84677c8a246bc85f80a1a0c1`
- 收口开始时 main：`42d8f76620a8c39eeb8523a0f84fcde0eb719f01`
- 收口分支：`codex/m18-main-closeout`
- PR #11：Draft/Open
- Issue #12：Open
- PR #11 head SHA：`6b8d3d5565246be5cf306b4d95a4759455bdcbbe`

## 3. GitHub 状态证据

### PR #11

PR 仍为 Draft/Open，base 为 `main`。其描述仍明确声明 Actions 在 checkout 前失败，且不得宣称 macOS build、合同重复执行、package consumer、性能或故障注入 artifact 已通过。

### Issue #12

Issue 仍为 Open，记录的根因类别是 Actions job 在任何 step 开始前失败，历史观测为 `steps=null` 且无可下载日志。

### Actions

对 `main@42d8f76620a8c39eeb8523a0f84fcde0eb719f01` 查询未返回关联 workflow runs。因此没有当前 main 对应的 checkout、steps、logs 或 artifact 证据。

### 远端旧分支

按分支名 `codex/m18` 搜索未返回旧分支。由于连接器无法执行本地 tree-to-tree diff，且 PR 仍保留 head SHA，本次不宣称已完成最终树等价证明，也不执行 PR 关闭后的远端分支清理动作。

## 4. 本地验证证据状态

当前执行环境无法克隆该私有仓库，也无法在 macOS runner 上执行构建。因此以下项目均未在本次收口中生成新的、可审计的原始证据：

| 验证项 | 要求 | 本次状态 |
|---|---|---|
| GDAL ON Release | 配置、构建、完整 CTest | 未执行/无新证据 |
| GDAL OFF Release | 配置、构建、完整 CTest | 未执行/无新证据 |
| Writer 基础合同 | 连续 3 次 | 未执行/无新证据 |
| Append 合同 | 连续 3 次 | 未执行/无新证据 |
| Update 合同 | 连续 3 次 | 未执行/无新证据 |
| Delete 合同 | 连续 3 次 | 未执行/无新证据 |
| Transaction/Recovery 合同 | 连续 3 次 | 未执行/无新证据 |
| 性能合同 | samples=3、profile=5s、reader10m=false | 未执行/无新证据 |
| no-GDAL consumer | 编译并运行 | 未执行/无新证据 |
| legacy consumer | 编译并运行 | 未执行/无新证据 |
| GDAL consumer | Index/Append/Transaction 编译并运行 | 未执行/无新证据 |

## 5. 六个 macOS artifacts

以下 artifacts 均未获得绑定当前收口 SHA 的可验证实例：

1. `writer-macos-contract-<sha>`
2. `writer-append-macos-<sha>`
3. `writer-update-macos-<sha>`
4. `writer-delete-macos-<sha>`
5. `writer-transaction-macos-<sha>`
6. `writer-macos-performance-<sha>`

## 6. SKIP、故障注入与未覆盖项

- 无本次新运行结果，因此无法形成可信的 SKIP 清单。
- publish/rollback/cleanup 故障注入无本次新证据。
- Recovery 的损坏源、损坏 backup、伪造候选、歧义候选、错误动作与发布后验证失败没有本次新 artifact。
- Linux、Windows、50M、35GB/5 亿、原生曲线与 MultiPatch 继续 Deferred。
- Reader 10M fresh-open 性能不在本分支范围。

## 7. 最终判定

**Code accepted / Formal acceptance blocked**

理由：M18 计划内实现已通过 squash 提交进入 main，但正式验收所要求的 GDAL ON/OFF Release、完整 CTest、五套 Writer 合同各三次、三类 package consumer、六个 macOS artifacts、故障注入及 raw profile 证据不齐全。

在上述证据齐全前，禁止标记 `Accepted`；Issue #12 保持 Open。PR #11 仅应在最终树覆盖得到可靠证明后关闭。