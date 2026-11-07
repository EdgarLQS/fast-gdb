# 2026-11-08 数值索引的 NaN 与无穷边界

## 发现与修复

原 ATX 比较以两个 `<` / `>` 都不成立表示相等。遇到 NaN 键或 NaN 查询参数时，此规则会错误返回相等，与字段 WHERE 的 IEEE 比较不一致。

本次提交范围：

- FLOAT32/FLOAT64 物化解析遇到 NaN 叶键返回失败，并清空部分条目。
- 分页查询解码到 NaN 叶键或用于导航/排序检查的分隔键时返回失败；调用者原有结果向量保持不变，QueryEngine 回退原字段扫描。NaN 查询参数同样返回失败。
- 公开 QueryEngine 的 AttributeDouble 请求明确要求有限数值。NaN、+Inf、-Inf 均返回 InvalidRequest，有索引、缺索引行为一致；与 CLI/WHERE 的有限数值输入规则一致。
- 原始物理索引 API 保留正负无穷的有序比较和正负零相等。字段读取和原有 WHERE 的源值比较合同保持不变。

## 验证覆盖

`AttributeIndexSafetyTest.NaNKeysCannotCompareEqualOrPublishPartialResults` 检查 FLOAT32/FLOAT64 NaN 叶键的六操作符、解析失败清空以及多叶树 NaN 分隔键；`NaNProbeIsRejectedForFiniteNumericIndexes` 检查有限索引与 NaN 探针；`InfinitiesAndSignedZeroRetainOrderedIeeeComparisons` 使用独立的物化/分页解析器，逐项对照 C++ IEEE 六操作符期望（两种物理浮点宽度、六个探针）。

`SpatialWhereIndexFallbackTest.NaNFloatKeyFallsBackToActualFieldsForEveryOperator` 用 GDAL 生成真实浮点字段和索引，再明确将索引键改为 NaN。原字段仍为 5/10/15，普通属性和空间 WHERE 的六操作符均得到原值期望，且不把该损坏 ATX 算作成功索引路径。`NonFinitePublicAttributeProbesAreExplicitErrors` 检查索引存在/缺失时公开请求的错误状态。

[修复前红测记录](2026-11-08-numeric-special-values/before-fix.log) 保留了 NaN 错误相等的复现；其中早期无穷对照测试复用了会清空物化条目的分页解析器，最终测试改为两个独立实例，不能将该测试误用算作产品缺陷。最新 [macOS 专项](2026-11-08-numeric-special-values/macos-focused.log) 29/29 通过。

| 构建 | 结果 | 原始记录 |
| --- | --- | --- |
| Linux Release | 617 PASS / 19 SKIP / 0 FAIL，22.18 s | [日志](2026-11-08-numeric-special-values/linux-release.log) |
| GDAL-free ASan/UBSan/LSan | 430 PASS / 5 SKIP / 0 FAIL，17.31 s | [日志](2026-11-08-numeric-special-values/linux-asan.log) |
| macOS Release | 567 PASS / 28 SKIP / 0 FAIL，71.26 s | [日志](2026-11-08-numeric-special-values/macos-release.log) |

## 仍需完成

分页检查覆盖访问到的键；未访问分支的完整性、陈旧/错误的有限键、数值/GUID 普通属性候选的原值复核仍需继续审查。真实 ArcGIS 大索引、百万长键/更大字符串规模及 Windows 等门禁保持总计划原状态，S4.2 尚未完成。本次真实 GDB 的损坏键由测试明确改写，不冒充 ArcGIS-authored NaN 索引语料；未修改原始 testcurve.gdb。
