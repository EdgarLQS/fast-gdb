> 状态：Current
> 适用版本：v0.2.0+
> 维护入口：docs/gdb/learning/modules/

# 课程模块清单

本目录保存 M04–M18 的独立课程定义，主入口 `../course.json` 通过 `module_files` 按顺序加载。拆分的目的，是以后补充知识点时只修改对应模块，不必编辑一个巨大的 JSON 文件。

每个模块必须完整包含：

- `micro_units`：课件锚点和建议时长；
- `diagnostic_questions`、`quiz_questions`：诊断与验收题；
- `standard_views`：默认折叠、可直接展开的标准图或标准表；
- `question_standard_views`：每道题对应的标准答案；
- `labs`：只读、白名单输入、可重复生成的证据报告；
- `evidence_task`、`oral_review`：证据判断和 AI 口试；
- `review_tasks`：D+2、D+7、D+21 闭卷复习。

## 怎样补充知识点

1. 在对应 `Mxx.json` 增加或修改微型单元、题目和标准答案。
2. 若增加题目，同时更新 `question_standard_views` 和 `tools/learning/answer_key.json`。
3. 若增加实验输入，只能使用课程工具允许的只读白名单路径；重新生成并审核实验摘要 SHA-256。
4. 在对应 `lessons/00xx-*.html` 增加与微型单元 ID 相同的锚点。
5. 运行 `python3 -m unittest discover -s tests/learning -p 'test_*.py'`。

标准答案必须说明：标准结论、证据等级、不能证明什么、常见误解和来源。高级数据集没有样本或公开格式证据时继续标记 `Unknown`。
