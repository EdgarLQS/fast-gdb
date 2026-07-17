# 空间属性联合查询文档一致性自检

- 日期：2026-07-17
- 分支：`codex/spatial-attribute-query`
- 基线：`main@d8784e7`
- 目标：在独立代码审核前，统一实现进度、测试覆盖、执行路径、审核入口和正式阻断项
- 结论：**文档自检完成，Code review ready / Formal acceptance blocked**

## 1. 自检原则

本轮按照以下规则检查：

1. 分支实现不得写成已经进入 `main`；
2. 测试代码存在不得写成实际通过；
3. `SKIPPED` 不计为验收通过；
4. `.spx` 和 `.atx` 都只能作为候选；
5. 正式发布文档不得自动吸收当前分支未验收能力；
6. 代码审核入口必须能定位公共契约、实现、测试、已知风险和未覆盖项；
7. 文档之间的状态、执行路径、测试文件和缺口必须一致。

## 2. 发现的主要冲突

### D1：规划总索引仍指向旧工作流

`docs/planning/00_规划文档状态索引.md` 原先仍记录旧 `main@42d8f76`、M18 分支和 Reader 性能工作，没有把当前 `codex/spatial-attribute-query`、计划 21 或代码审核入口列为 active。

修复：

- 更新为当前分支和 `main@d8784e7` 基线；
- 增加 `Code review ready / Formal acceptance blocked`；
- 增加计划、审核指南、代码自审、文档自检、覆盖矩阵和 QUERY_FLOW 阅读顺序；
- 将 M18/Reader 性能计划明确标记为其他工作流。

### D2：项目状态未列联合查询

`docs/planning/01_项目状态与规划.md` 原先只列既有 Reader/Writer 状态，可能使审阅者误认为当前分支没有新增能力，或把分支能力误归入既有正式 Reader 发布范围。

修复：

- 新增“空间与属性联合查询”状态行；
- 区分既有发布能力与当前分支待审能力；
- 列出已完成实现、当前审核判定和未形成的实际证据；
- 增加代码审核为当前未完成事项。

### D3：文档总索引缺少当前审核入口

`docs/README.md` 原先没有计划 21、自审证据、文档自检和联合查询审核指南。

修复：新增“当前分支审核入口”，并把计划 21、审核指南、两份证据、覆盖矩阵和 QUERY_FLOW 加入索引。

### D4：覆盖矩阵仍把已完成测试写成缺口

`docs/usage/04_功能与基准测试覆盖矩阵.md` 原先仍把 Z/M/ZM 组合入口和部分索引组合列为待补，也没有列出 `.atx` fail-closed 合成测试、NULL、函数索引和并行测试隔离。

修复：

- 新增 `CODE_READY` 状态；
- 展开纯 C++、GDAL 集成和 `.atx` 安全测试；
- 列出 Point/Polyline/Polygon/MultiPoint、Z/M/ZM、NULL、Unicode、函数索引和索引故障；
- 明确代码完成但实际 CTest 未运行；
- 更新 benchmark 和 CI 分层。

### D5：测试索引未收录新增文件

`tests/INDEX.md` 原先只描述旧目录结构和固定测试总数，没有收录当前分支新增的 WHERE、索引安全、联合几何、维度、NULL、Unicode、函数索引和 benchmark 文件。

修复：

- 删除固定测试总数依赖，要求使用 `--gtest_list_tests` 或 `ctest -N`；
- 新增纯 C++、GDAL 集成和 package consumer 索引；
- 增加 GDAL ON/OFF、专项过滤、并行 CTest 和 benchmark 命令；
- 明确测试代码存在不等于通过。

### D6：Reader 查询流程缺少联合查询和 fail-closed

`src/edgar/explorgdb/reader/QUERY_FLOW.md` 原先只描述空间和属性分别查询，并包含已经过时的索引解析叙述。

修复：

- 增加 QueryKind 分派和 `SpatialWhere` 全流程；
- 增加精确空间、属性候选交集和完整 WHERE 复核；
- 增加 `.gdbindexes` 映射；
- 增加 `.atx` 文件/页/页链/计数/FID fail-closed；
- 增加非 BMP、`!=`、函数索引和不明确编码的回退边界；
- 增加稀疏候选字段扫描和测试对照。

### D7：Changelog 未记录当前分支能力和审核状态

`CHANGELOG.md` 的 Unreleased 只记录 Writer 事项，缺少当前分支联合查询、索引安全修复和“未进入 main”的边界。

修复：

- 添加 `SpatialWhere`、联合指标、WHERE 模块、索引映射、稀疏扫描和测试；
- 添加 `.atx` fail-closed、并行测试隔离和实际 benchmark 证据；
- 添加 Review status，明确仅存在于当前分支且正式验收阻塞。

### D8：项目总览仍使用旧基线和发布表述

`docs/overview/01_fast-gdb项目介绍与当前状态.md` 原先记录旧 main 基线，没有区分当前分支联合查询与既有正式 Reader 发布范围。

修复：

- 更新当前分支与基线；
- 增加联合查询公开入口、执行路径、实现、安全回退、测试和审核状态；
- 保留既有几何和性能发布结论，不把本分支能力自动加入 v0.1.0；
- 增加完整代码审核阅读顺序。

### D9：缺少独立代码审核指南

原文档只能从计划和提交历史推断审核顺序，不利于独立审核。

修复：新增 `docs/usage/10_空间属性联合查询代码审核指南.md`，按公共契约、WHERE、索引、扫描、测试和安装面组织审核，并给出 P0/P1 检查点、建议命令和报告格式。

## 3. 已更新文档

| 文件 | 更新结果 |
|---|---|
| `docs/planning/00_规划文档状态索引.md` | 当前分支审核总入口 |
| `docs/planning/01_项目状态与规划.md` | 增加联合查询分支进度和阻断项 |
| `docs/planning/21_空间属性联合查询实现计划.md` | 实现、自审、文档和验收进度统一 |
| `docs/README.md` | 增加当前分支审核入口 |
| `docs/overview/01_fast-gdb项目介绍与当前状态.md` | 更新分支状态和联合查询总览 |
| `docs/usage/04_功能与基准测试覆盖矩阵.md` | 更新测试覆盖、性能和 CI 状态 |
| `docs/usage/10_空间属性联合查询代码审核指南.md` | 新增独立代码审核入口 |
| `tests/INDEX.md` | 收录新增测试文件和命令 |
| `src/edgar/explorgdb/reader/QUERY_FLOW.md` | 更新实际联合执行链 |
| `CHANGELOG.md` | 记录 Unreleased 分支能力和审核边界 |
| `docs/evidence/spatial-attribute-query-self-review-2026-07-17.md` | 保持三轮代码自审事实来源 |
| 本文档 | 记录文档冲突、修复和最终结论 |

## 4. 有意保持发布级表述不变的文档

以下文档描述既有发布或历史证据，不应因为当前分支代码存在而直接修改结论：

- 根目录 `README.md` 的 v0.1.0 正式能力边界；
- `docs/releases/v0.1.0.md`；
- `docs/evidence/13_fast-gdb最终等价与发布验收报告.md`；
- 历史空间查询和 Writer 验收记录；
- `docs/planning/02_GDAL功能对比矩阵.md` 的正式产品能力，除非后续联合查询正式验收并进入 main。

当前分支通过 `docs/README.md`、项目总览、规划索引和 Changelog Unreleased 暴露审核状态，同时避免污染既有发布声明。

## 5. 文档一致性结果

当前所有审核入口统一使用：

```text
分支: codex/spatial-attribute-query
基线: main@d8784e7
实现状态: completed
三轮静态代码自审: completed
文档自检: completed
代码审核: ready
正式验收: blocked
```

执行路径统一为：

```text
spatial-where:spx+atx
spatial-where:spatial-candidates
spatial-where:sequential
spatial-where:invalid
```

测试覆盖统一包括：

- WHERE parser/evaluator；
- `.gdbindexes` 表达式分类；
- `.atx` fail-closed；
- Point、Polyline、Polygon 含洞、MultiPoint；
- Z/M/ZM；
- NULL、NaN、Unicode；
- 函数索引；
- `.spx/.atx` 缺失和损坏；
- package consumer；
- 100K schema-v2 benchmark runner。

## 6. 仍未完成的证据

文档已准备好支持代码审核，但以下内容仍没有实际运行证据：

- GDAL ON Release；
- GDAL OFF Release；
- 完整 CTest；
- `ctest -j`；
- package consumer；
- 100K 完整性能矩阵；
- 10M Point/MultiPoint/Polyline；
- current/main 交替采样；
- 5% 性能回退门禁；
- `git diff --check main...HEAD`。

## 7. 最终结论

```text
文档一致性自检完成
实现进度和测试代码已同步到审核入口
可以开始独立代码审核
Formal acceptance blocked
```
