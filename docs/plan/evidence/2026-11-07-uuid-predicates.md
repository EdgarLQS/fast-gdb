# UUID WHERE 值比较与 GUID 索引安全回退

基线 `5ce10e8`。本次提交修复前一阶段保留的 GUID 查询结果差异，并增加独立扫描对照；原生 GUID ATX 解码仍未完成。

## 计划与诊断

历史现代库的 GUID 查询曾走顺序回退，但结果与 GDAL 字段值不一致。最小测试同时构造 FileGDB 原始 16 字节 UUID 和物化后的文本，先验证错误可以复现，见 [失败日志](2026-11-07-uuid-predicates/before.log)。

根因有两处：零拷贝 `FieldRef` 将原始 16 字节当作字符串；物化 `FeatureRecord` 输出小写 UUID，而 GDAL 字段文本使用带花括号的大写形式，普通文本比较并不相等。问题不在字段值解码本身，故保留已有公开字段输出格式。

## 执行与合同

- WHERE 编译依据 UUID_1 / UUID_2 字段类型标记 UUID 字面量，按 32 个小写十六进制数字规范化。支持标准连字符 UUID，可带外层花括号，大小写等价；不接受非法字符、错误分隔或截断。
- 原始行求值先按 FileGDB 前三段小端布局解码，再比较；物化行的标准文本转换为同一比较形式。六种运算符以及 IN 使用相同的值比较，NULL 仍保持 UNKNOWN。
- 普通 String/XML 比较保持原有大小写和内容规则。新增测试验证普通字符串 `abc` 不等于 `ABC`，并覆盖两个 UUID 类型、NULL 和不足 16 字节的输入。
- 已识别的 GUID 索引请求显式使用顺序值比较并报告原因，不尝试把 GUID ATX 当普通 UTF-16 索引。非法 UUID 字面量的 WHERE 编译失败；直接属性请求的回退谓词编译失败时返回 `InvalidRequest` 和错误信息。

## 验证

历史库来自固定 GDAL v3.10.3 归档，哈希见 [manifest](2026-11-07-gdal-historical-fixtures-manifest.json)。期望结果由 GDAL 无过滤器逐记录字段扫描产生，独立规范化文本后对六种运算符生成完整 FID 集合；查询分别使用小写无花括号及大写带花括号字面量。未使用 GDAL 属性索引候选作真值。

| 样本 | UUID WHERE 完整 FID 对照 | 已识别 GUID ATX 顺序回退对照 |
|---|---:|---:|
| testopenfilegdb | 684 次通过 | 36 次通过 |
| testopenfilegdb92 | 612 次通过 | 0（v9 元数据不支持，未计验收） |
| testopenfilegdb93 | 612 次通过 | 0（同上） |

[原始 XML 和日志目录](2026-11-07-uuid-predicates) 同时保留以下最终代码结果：

- Linux ARM64 / GCC 14.2 / GDAL 3.10.3 Release 全量：607 项，588 PASS / 19 SKIP / 0 FAIL，22.77 秒。
- GDAL-free ASan/UBSan/LeakSanitizer：416 项，411 PASS / 5 SKIP / 0 FAIL，21.33 秒，无 sanitizer 报告。
- macOS `build-s4-release`：WHERE 求值 15 项全部通过。
- `git diff --check` 通过。自审检查原始字节长度、混合字节序、NULL、非法字面量错误和普通字符串边界，未增加依赖。

此处未验收原生 GUID ATX、日期 ATX、UUID 的 SpatialWhere 专项矩阵、大型 GUID 索引或 ArcGIS 独立语义真值。整体只读计划仍在执行；剩余外部语料及平台门禁见 [总计划](../22_完整只读能力与GDAL对照升级计划.md)。用户原始 testcurve 目录未修改。
