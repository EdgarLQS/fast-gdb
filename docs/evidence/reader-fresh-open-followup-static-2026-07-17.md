# Reader 10M fresh-open 后续静态证据（2026-07-17）

## 1. 范围

本记录覆盖在 GitHub runner 无法启动、当前执行环境没有仓库 checkout 的条件下，能够完成并审计的 Reader 10M fresh-open 后续工作。

不把静态检查、脚本实现或历史性能数字替代为新的 macOS profile、构建、CTest 或性能验收。

## 2. 固定基线

- baseline：`main@42d8f76620a8c39eeb8523a0f84fcde0eb719f01`
- 开发分支：`codex/m18-main-closeout`
- 历史 15 档基线：`docs/evidence/reader-fresh-open-macos-2026-07-15.md`
- 模式：fresh-open，未清理 OS page cache，不是 strict-cold

历史基线已经证明 Point、MultiPoint、Polyline 的完整 FID 与 GDAL 一致且 `invalid_geometries=0`，但部分性能档未通过。该历史结论不等于当前分支的复测结果。

## 3. 已完成的工具工作

`scripts/run_spatial_regression.py` 已完成以下扩展：

- 支持重复 `--dataset LABEL=PATH`，一次执行 Point、MultiPoint、Polyline；
- 保留原 `--gdb` 单数据集调用兼容；
- 支持 `--mode steady-state|fresh-open`；
- current/main 在数据集和覆盖率之间交替执行，降低单向温度与缓存偏差；
- 固定 1%、10%、30%、80%、100% 五档覆盖率；
- 输出逐场景原始日志、CSV、JSON 和 `environment.json`；
- `environment.json` 明确记录 `strict_cold=false`；
- `invalid_geometries != 0` 立即失败；
- current/main result count 不一致时失败；
- benchmark 输出 FID 签名时执行签名一致性检查；
- current 相对 baseline median 回退超过默认 5% 时失败；
- 任一门禁失败时返回非零。

## 4. 静态审查修复

本轮静态审查发现并修复：

1. Python 测试通过 `importlib` 动态加载脚本时未先注册 `sys.modules`，部分 Python 版本中的 `dataclass` 处理可能失败；
2. `--dataset Point=` 会把空路径解析成当前目录，现已明确拒绝空路径；
3. 将 current/main 判定提取为纯函数 `compare_samples()`，便于独立验证性能回退、result count 与 FID 签名漂移；
4. 增加参数空值、空格规范化、性能回退、数量漂移和 FID 签名漂移测试。

## 5. 推荐执行命令

```bash
python3 -m unittest tests/scripts/test_run_spatial_regression.py

python3 scripts/run_spatial_regression.py \
  --current-build /tmp/fast-gdb-reader-current \
  --baseline-build /tmp/fast-gdb-reader-main \
  --mode fresh-open \
  --trials 5 \
  --max-regression 0.05 \
  --dataset Point="$PWD/test_data/spatial_matrix/point_10000000.gdb" \
  --dataset MultiPoint="$PWD/test_data/spatial_matrix/multipoint_10000000.gdb" \
  --dataset Polyline="$PWD/test_data/spatial_matrix/line_10000000.gdb" \
  --output /tmp/fast-gdb-reader-fresh-open
```

运行前必须分别从固定 baseline 和当前分支生成 Release 测试 runner。输出目录必须位于仓库外，不得提交原始 benchmark 数据、构建目录或 profile 临时文件。

## 6. 当前 GitHub Actions 证据

PR #13 对应提交触发了 Writer、Reader、release 和 geometry workflows，但 job 仍在任何 step 前失败：

- checkout 未开始；
- `steps=None`；
- 无 job log；
- 无 artifact。

失败重跑仍未形成可审计执行结果。因此 Issue #12 保持 Open。

## 7. 未执行与禁止外推

以下项目仍未执行：

- 当前分支三类 10M × 五档正式复测；
- 目标失败档的当前 profile；
- Point geometry-only scan 或候选路径算法优化；
- MultiPoint/Polyline 算法优化；
- Reader 专项测试、GDAL ON/OFF 完整 CTest；
- current/main/GDAL 最终对比 artifact。

由于计划要求热点可重复且约占总耗时 20% 以上才允许优化，本轮不提交无 profile 支撑的 Reader 算法修改。

## 8. 判定

**Tooling complete / Runtime acceptance blocked**

可在无 runner 条件下完成的工具、静态审查、测试代码和执行说明已经完成。Reader 性能本身仍未验收，不能标记 Accepted。
