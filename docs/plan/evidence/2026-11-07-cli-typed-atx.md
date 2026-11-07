# CLI 属性索引类型传递与无损 INT64 诊断

基线 `8267729`。本次补齐 CLI 的 explore、dump-atx、query-atx 和 query attr 类型传递，并修复物化路径发现的数值标志问题。

## 计划与复现

[修复前命令](2026-11-07-cli-typed-atx/before-cli.log) 显示真实 `idx_float = 1.5` 错误返回 0 条，GUID dump 解析失败。原因是 CLI 使用默认 INT32 解析器、根据 trailer 的 string 位选择查询，并把所有数值先转成 double。

接通类型后，真实浮点 dump 仍显示空字符串；[标志回归失败](2026-11-07-cli-typed-atx/before-flags.log) 复现了这个独立问题。参考库 Float32/Float64/日期索引 flags=0x60，原逻辑将其中的 0x20 位直接认成字符串；数字位应优先解释。

## 执行

- GDB 内索引按直接字段表达式和 schema 自动确定类型，独立文件支持显式末尾 `field_type`；有 schema 时类型冲突拒绝。未知布局不猜字段和键类型。
- explore 现在按实际 `.idx_name.atx` 文件枚举，不再使用只能匹配字面扩展名 `.atx` 的旧查找。
- 查询复用分页接口；INT64 严格解析为 int64，含正负极值和 2^53 附近相邻值。非法输入、重复符号、部分数值、非有限值、错误类型/操作符以及读失败返回非零退出码。
- query attr 删除文件名子串匹配，准确绑定索引名称；dump/query 输出实际字段类型；FLOAT64 dump 使用 17 位有效数字，并用 `0.10000000000000002` 验证可往返显示。物化与分页 ATX 均修正 0x60 数值标志分类。
- `AttributeIndexEntry` 增加可选 `integer_value`，typed INT64 物化保留精确值；新增 `query_int64()`。typed INT64 的 double 物化/分页入口拒绝使用，调用方应使用精确整数方法；QueryEngine 的原 double 请求可使用既有 WHERE 顺序回退。`numeric_value` 保留兼容的近似 double 视图，不作为无损 INT64 值。

低层公开结构增加成员，使用这些低层 C++ 类型的消费者需要重新编译；这是开发候选变更，不声明兼容旧二进制布局。统一 Reader 的公开字段值类型没有变化。使用说明见 [CLI 教程](../../tutorial/02_只读CLI属性索引.md)。

## 验证

新增 Python 标准库真实进程回归 `full.cli.typed-atx`，依赖固定语料 manifest 门禁；没有新增第三方依赖。测试构造明确合成的单页原始 ATX，覆盖六运算符、FLOAT32 真实重叠标志、GUID、INT64 正负极值和相邻大整数、中文路径、拒绝缺失类型/冲突/非法输入。自动类型使用仓库固定 ArcGIS 样本及可选现代历史库，而非按测试输入猜类型。

- Linux CLI：57 个真实进程检查通过，包含历史 Float32 的 query-atx、query attr、dump-atx。固定 GDB 的 111 个文件和全部临时输入 SHA-256 前后一致。
- macOS CLI：54 个进程检查通过；未配置现代历史库，不把缺少的 3 项算作通过；111 个文件不变。
- macOS ATX 安全与 WHERE 定向：30/30 通过。
- Linux ARM64 / GCC 14.2 / GDAL 3.10.3 Release 全量：618 项，599 PASS / 19 SKIP / 0 FAIL，19.26 秒。
- GDAL-free ASan/UBSan/LeakSanitizer：422 项，417 PASS / 5 SKIP / 0 FAIL，15.38 秒，无报告。此配置 tools=OFF，验证索引核心，不包含 CLI 进程。
- 自审检查字段绑定、精确整数解码、退出状态、无效值不发布结果和所有 CLI 构造点；顺带为相关历史测试的 ASSERT 宏补上显式条件块，清除编译警告。`git diff --check` 通过。

[原始日志](2026-11-07-cli-typed-atx)。整体计划未完成：仍需核对字符串 ATX 的 Unicode/空格语义、非有限索引值和其它旧 CLI 入口；v9 字段发现、真实 ArcGIS 附件/高级对象/MultiPatch/高位稀疏 v4、Windows 与冷 I/O 等验收仍待完成。原始 testcurve 没有修改。
