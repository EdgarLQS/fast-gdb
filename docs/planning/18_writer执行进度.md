# M18 Writer 执行进度

- 更新日期：2026-07-16
- 执行分支：`codex/m18-1-macos-test-contract-ci`
- 关联 PR：#11
- 基线：`main@373bb265212a04fe20e44b8b8d8bb0b64e2e8faf`
- 当前结论：实现持续推进，所有阶段在取得当前 macOS 证据前均不得记为正式验收通过。

## 阶段状态

| 阶段 | 实现状态 | 自审状态 | 当前验收状态 | 说明 |
|---|---|---|---|---|
| M18.1 macOS 测试契约 | 已实现 | 已完成 | Blocked | schema-v2 runner、Writer/Index/package consumer 合同及手工门禁已建立 |
| M18.2 稳定 API 与安装面 | 已实现 | 已完成 | Blocked | `WriterSession`、结构化错误、稳定/legacy 安装面已冻结 |
| M18.3 macOS 性能专项 | 测量基础设施已实现 | 已完成 | Blocked | current/main/GDAL 比较、5% 门禁和 `sample(1)` profile 链路已建立；尚无真实 profile artifact |
| M18.4.1 非空追加 | 已实现 | 已完成 | Blocked | staging 追加、FID 单调、逐行回读、索引查询、回滚及专用合同已实现 |
| M18.4.2 Update | 进行中 | 已发现 3 个 Major | 未达出口 | 核心 staging Update、ADR、manifest、测试和 workflow 已加入；见下方未完成项 |
| M18.4.3 Delete | 未开始 | — | — | 等 Update 代码级 Major 清零后开始 |
| M18.4.4 事务/崩溃恢复 | 设计阶段 | API/架构评审待完成 | — | 本轮先冻结模型，不实现运行时代码 |

## M18.4.2 当前未完成项

1. 保存全部 updated FID，并在 commit 重开阶段逐 FID 验证存在性、FID 不变和更新结果。
2. Binary 字段增加 byte-for-byte 立即回读和 commit 重开验证。
3. 清理公共头文件中的测试专用标准库依赖，将 `<fstream>`、`<limits>` 等移回测试文件。
4. 完成上述修复后重新执行 Update 独立自审，确认没有未处理 Critical/Major。

## 当前外部阻塞

GitHub Actions 在 checkout 前失败，多个新旧 workflow 均返回：

- `status=completed`
- `conclusion=failure`
- `steps=null`
- `logs_url=null`

该问题记录于 Issue #12。它不再阻止静态实现和文档设计，但阻止以下结论：

- 不得宣称 macOS 当前构建通过；
- 不得宣称 required contract 连续三次通过；
- 不得宣称安装消费、性能门禁或 raw profile 已实际执行；
- PR #11 保持 Draft，审查结论保持 Request Changes。

## 完成率口径

完成率只统计当前 macOS 范围，不包含 Linux、Windows、50M 阶梯、原生曲线或 MultiPatch。

- 合同与测试基础设施：已实现，待运行证据。
- 稳定 API 与安装面：已实现，待运行证据。
- 性能测量基础设施：已实现，待真实 profile。
- 高级编辑：Append 已实现；Update 进行中；Delete 和事务运行时未实现。

任何历史结果、静态检查、推算或 `SKIP` 都不能替代当前验收。

## 下一执行顺序

1. 清零 Update 自审中的 3 个 Major。
2. 完成 Public API Freeze 审查并冻结统一错误模型。
3. 完成事务 ADR 与设计文档，确认 Delete API 不与事务模型冲突。
4. 开始 Delete：staging 重写、FID 不复用、范围收缩、索引无残留、全删空表。
5. CI 恢复后按 M18.1 → M18.2 → M18.3 → Append → Update 顺序补齐当前证据。
