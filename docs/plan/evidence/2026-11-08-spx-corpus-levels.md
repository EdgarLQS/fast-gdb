# 真实 SPX 层级清单与覆盖边界

本次产品 Reader 未修改；新增只读清单工具及边界测试，用于确定下一步真实多级索引和源覆盖验证缺口。总计划仍未完成。

用户已明确暂时无法提供 Windows/ArcGIS 环境及所需真实特殊样本；这些验收保持未完成，不继续假定外部条件可用。

## 实际扫描

`tools/ci/inventory_spx_levels.py` 从 v1/v2 trailer 读取布局、沿最左路径进入叶链，流式统计网格层级、叶项数和 signed 叶序；不把索引项数当作 feature 数。读取前后检查句柄及路径的文件身份。工具不写源文件，不重建索引；外部 Writer 应先关闭。

| GDB | SPX 文件 | 叶项数 | 存储层级 |
|---|---:|---:|---|
| 用户 testcurve | 45 | 1,142,594 | 全部 level 0 |
| acceptance_metadata | 7 | 60 | 全部 level 0 |
| testopenfilegdb | 35 | 328 | 全部 level 0 |
| testopenfilegdb92 | 19 | 246 | 全部 level 0 |
| testopenfilegdb93 | 19 | 246 | 全部 level 0 |
| 合计 | 125 | 1,143,474 | 未发现 level 1/2 条目 |

macOS 与 Linux 逐文件清单完全一致，所有已扫描叶链的 signed 顺序检查均为 true。原始结果见 [macOS](2026-11-08-spx-corpus-levels/macos-inventory.json)及 [Linux](2026-11-08-spx-corpus-levels/linux-inventory.json)。这证明当前语料未提供粗层条目验收，不能用其绿色回归代替真实多级索引对照。

## 工具验证与限制

标准库 unittest 3 组覆盖 v1/v2、有符号混排层级、叶链计数、循环、越界页、坏页计数、零 ID、trailer 数量不符、截断和非单调顺序。每次测试比较源 bytes 不变。CTest 注册 `full.spx-level-inventory`，macOS/Linux 各 1/1 通过。见 [单元结果](2026-11-08-spx-corpus-levels/unit.log)、[macOS CTest](2026-11-08-spx-corpus-levels/macos-ctest.log)及 [Linux 检查](2026-11-08-spx-corpus-levels/linux-check.log)。

这是调查工具，不是 SPX 完整性验收器。它不验证所有内部页、不验证 source FID/几何映射；叶序为 false 时单独报告，并不伪装为源覆盖结论。没有记录全库并发快照，也不能证明 Writer 活动期间安全。没有重跑产品全套或 sanitizer：本次未改产品代码。

复用示例：

```sh
python3 tools/ci/inventory_spx_levels.py --gdb /path/data.gdb > levels.json
python3 tools/ci/test_inventory_spx_levels.py
```

缺失目录、未知布局、越界或计数错误返回 2；零个 SPX 文件会如实记录数量为零，不宣称存在索引。

下一步实施依据见[源几何验证目录设计](../25_SPX源几何验证目录实施方案.md)。
