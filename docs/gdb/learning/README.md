> 状态：Current（M01–M18 已实现）
> 适用版本：v0.2.0+
> 维护入口：docs/gdb/learning/

# FileGDB 全面学习实验室

本课程包含 18 个完整 FileGDB 知识模块，但不把“读完 18 篇页面”视为完成。所有模块都提供题目对应的可展开标准答案、只读证据实验、AI 口试和间隔复习，并按掌握状态解锁。

## 快速开始

从仓库根目录执行：

```bash
python3 tools/learning/course.py doctor
python3 tools/learning/course.py serve --port 8766
```

启动后直接访问 `http://127.0.0.1:8766/`，无需令牌。学习台只监听 `127.0.0.1`，个人进度写入被 Git 忽略的 `build/learning/`。

常用命令：

```bash
python3 tools/learning/course.py status
python3 tools/learning/course.py lab M01-L01
python3 tools/learning/course.py review-export M01
python3 tools/learning/course.py review-import M01 build/learning/reviews/M01-result.json
python3 tools/learning/course.py export-record M01
```

`export-record` 只有在模块达到 `Mastered` 后才生成候选记录，不会自动写入正式 `learning-records/`。

## 学习闭环

```text
概念诊断 → 微型课程 → 标准答案浏览 → 真实实验 → 证据核查
         → AI 口试 → D+2 / D+7 / D+21 → 长期掌握
```

模块状态：

- `Locked`：先修模块尚未暂时掌握；
- `NotStarted`：可以开始；
- `Learning`：已有活动记录，但验收门禁未完成；
- `Provisional`：测验、实验、证据和 AI 口试均通过，可以学习下一模块；标准答案查看不计入门禁；
- `Mastered`：三次间隔复习均通过；
- `Planned`：为未来扩展保留的状态；当前 18 个模块均已交付。

## 当前课程模块

| 阶段 | 模块 | 标准答案和实验重点 |
|---|---|---|
| 基础 | M01–M03 | 逻辑关系、字节解码、文件家族 |
| 表与记录 | M04–M07 | Catalog、GDBTABLE、字段描述符、记录游标 |
| 几何 | M08–M11 | FID/TABLX、精度网格、基础与高级几何 |
| 索引查询 | M12–M13 | SPX 候选、ATX、Planner 与 Cursor |
| 行为与高级模型 | M14–M17 | Schema 行为、Raster、约束型和现代数据集 |
| 综合验收 | M18 | 八层知识图、问题定位和 AI 方案审核 |

验收要求为测验至少 85%、关键题全部正确、实验完成、证据作品通过校验，以及 AI 口试至少 10/12 且每项不低于 2。标准答案默认收起，点击后显示结论、图表、证据、边界和常见误解；查看记录只用于学习轨迹，不替代验收。实验结果根据版本化答案清单独立校验，不把 `explorgdb_cli` 的自由文本当作稳定评分接口。

证据说明必须包含该模块要求的证据等级和“不能证明什么”，并生成带 SHA-256 的 `build/learning/artifacts/<module>/` 作品。`review-export` 会创建待处理口试会话；`review-import` 只接受匹配会话、3–5 个问题、有效时间和完整四维评分的结果。课程工具防止通过正常接口跳过门禁，但本地学习者始终拥有文件系统权限，因此这些校验是学习完整性检查，不是防恶意篡改系统。

诊断分数只给出“详细学习 / 重点学习 / 快速复述”的深度建议，不能免除任何门禁。课程不要求学习者自己画图、手工分类文件或手工填写字节解码；每个已交付知识点都必须提供可展开的标准答案。学习者仍需通过真实实验、证据说明和 AI 口试证明理解。

M01–M03 直接维护在 `course.json`，M04–M18 分别维护在 [`modules/`](modules/) 中。新增知识点时，在对应模块增加唯一知识点、题目和 `standard_views`，再用 `question_standard_views` 把每道题映射到标准答案。现有 `graph`、`bytes`、`file-family`、`table` 四种展示类型可直接复用；详细规则见 [`modules/README.md`](modules/README.md)。

## M04–M18 已交付内容

[`lessons/`](lessons/) 提供概念课件，学习台同时提供对应题库、标准答案和验收闭环：

- M04–M07：Catalog、GDBTABLE、字段和记录；
- M08–M11：FID、空间参考和几何；
- M12–M13：SPX、ATX、Query Planner 和 Cursor；
- M14–M17：Schema 行为、Raster、约束型和现代数据集；
- M18：完整知识图谱与 AI 审核。

## 数据与证据

- 核心数据：`test_data/gdb/acceptance_metadata.gdb`，由 ArcGIS Pro 3.5 arcpy 生成并随课程版本控制；来源和 SHA-256 见 [`fixtures/core-fixture.json`](fixtures/core-fixture.json)。实验使用 `build/learning/fixtures/` 中的副本。
- 本机增强数据：其余被忽略的 `test_data/`，缺失时不影响 18 个核心模块。
- Windows 选修：使用 [`../../../tests/createdata/DATASET_GUIDE.md`](../../../tests/createdata/DATASET_GUIDE.md) 重新生成并对照 ArcGIS 行为。
- `explorgdb_cli` 当前可能显示 `Magic file ... UNEXPECTED`；这是已记录的诊断边界，不参与评分。

CLI 的状态目录被限制在仓库 `build/learning/` 内。进度读改写在同一服务内串行执行，跨进程使用崩溃后自动释放的文件锁，并在原子替换前保存上一版备份。

结论统一使用 `EsriConfirmed`、`GdalConfirmed`、`FastGdbConfirmed`、`DataObserved`、`Inferred` 和 `Unknown`。参考入口：

- [`MISSION.md`](MISSION.md)：学习使命；
- [`RESOURCES.md`](RESOURCES.md)：一手资料；
- [`reference/0001-knowledge-map.html`](reference/0001-knowledge-map.html)：八层知识地图；
- [`reference/0002-file-family.html`](reference/0002-file-family.html)：文件家族速查；
- [`reference/0003-evidence-review.html`](reference/0003-evidence-review.html)：证据和 AI 审核清单；
- [`../../plan/04_FileGDB数据结构完整学习路线.md`](../../plan/04_FileGDB数据结构完整学习路线.md)：总体计划和交付状态。

## 学习记录规则

不要预先创建 `learning-records/`。只有学习者在口试、实验和间隔复习中真正证明掌握后，才把候选记录整理为 `0001-<topic>.md`。记录非显然结论及其对后续学习的影响，不写活动流水账。
