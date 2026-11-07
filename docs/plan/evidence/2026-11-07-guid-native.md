# GUID 原生属性索引解析与联合查询

基线 `0e9125a`。本阶段实现已识别 GUID/GlobalID 字段的 ATX 物化与分页查询，并保留损坏索引的顺序回退。整体只读计划仍在执行。

## 计划与证据依据

现代历史参考库 `a0000000a.idx_guid.atx` 为 4118 字节：v1、38 字节键、flags=0、5 个值。真实键为带花括号的大写 ASCII UUID。固定 [GDAL v3.10.3 索引读取实现](https://github.com/OSGeo/gdal/blob/v3.10.3/ogr/ogrsf_frmts/openfilegdb/filegdbindex.cpp#L1131-L1145) 检查 GUID 键长，分支和叶页按固定 ASCII 字节比较；它与普通 UTF-16 字符串不是相同编码。

[修复前测试](2026-11-07-guid-native/before.log) 证明真实库 GUID 请求仍走顺序回退，合成两叶页 GUID 也不能正确解码/分页查询。真实参考库的样本哈希沿用 [历史 manifest](2026-11-07-gdal-historical-fixtures-manifest.json)。

## 执行

- 把既有 WHERE 的 UUID 规范化提取为共用头文件，GUID 字面量大小写与可选花括号合同不变；可索引谓词转换成存储用的 38 字节规范 ASCII。
- ATX 依据 `FieldType::UUID_1/UUID_2` 区分 GUID 编码，物化和分页均验证键长、标志和规范文本。普通字符串保留 UTF-16 路径。无法识别的无类型标志物理布局不再物化成数值零。
- GUID 直接查询支持六运算符，SpatialWhere 也允许完整、未截断 GUID 的六种比较；最终仍按 WHERE 值语义复核。普通字符串与数值的 `!=` 安全门槛保留。
- 损坏/未知 GUID 键使索引失败，结构化 QueryEngine 继续顺序值扫描。非法 UUID 在空树时同样拒绝，避免空成功；将 UUID 当数值查询返回 InvalidRequest。
- 低层独立 ATX 调用必须传 UUID 字段类型；QueryEngine 从元数据和 schema 传递类型。CLI 尚有默认 INT32 的旧入口，列入下一项，不能据本次 SDK 测试宣称 CLI 已全部贯通。

## 验证

| 参考库 | UUID WHERE | UUID SpatialWhere | 原生直接 ATX | 原生联合 ATX |
|---|---:|---:|---:|---:|
| testopenfilegdb | 684 | 540 | 36 | 36 |
| testopenfilegdb92 | 612 | 468 | 0 | 0 |
| testopenfilegdb93 | 612 | 468 | 0 | 0 |

所有次数均执行完整 FID 集合断言；现代库原生路径另有诊断断言。92/93 的 v9 元数据仍不支持，原生次数为零；查询扫描结果通过不代表原生索引支持。期望值来自 GDAL 无过滤器字段/几何扫描，不使用其索引候选生成期望集合。

补充两类测试，不能替代 ArcGIS 独立真值：

1. 合成 v1/v2、两层两叶页：覆盖所有 UUID 组件、跨叶页重复键、六运算符、UUID_1/UUID_2、v2 高位 64 位 FID、非法键、空树与失败输出不变。
2. GDAL 创建 512 行物理 GUID 表，独立读回 495 个非 NULL UUID；因 GDAL 3.10.3 拒绝新建 GUID 索引，ATX 与索引声明明确手工合成，形成 7 页索引。正常与损坏两组各 120 次直接/联合查询对照通过，包括多种值、重复、NULL 和 NULL 几何。没有将合成索引称为 GDAL 或 ArcGIS 写出的索引。物理字段采用 [GDAL COLUMN_TYPES 实现](https://github.com/OSGeo/gdal/blob/v3.10.3/ogr/ogrsf_frmts/openfilegdb/ogropenfilegdblayer_write.cpp#L1159-L1212) 指定 GUID，并断言实际 schema。

最终原始结果见 [日志/XML](2026-11-07-guid-native)：

- Linux ARM64 / GCC 14.2 / GDAL 3.10.3 Release：616 项，597 PASS / 19 SKIP / 0 FAIL，23.99 秒。
- GDAL-free ASan/UBSan/LeakSanitizer：421 项，416 PASS / 5 SKIP / 0 FAIL，17.57 秒，无 sanitizer 报告。
- macOS 索引安全、日期、WHERE 与 GUID 集成：46/46 通过。
- 自审检查规范化与存储比较顺序一致、未知编码不猜值、GUID 与普通 UTF-16 不混用、空树输入校验、两种 ID 宽度和所有调用处类型传递；`git diff --check` 通过，未新增依赖。

后续仍需：CLI 类型传递、v9 索引元数据、大型/多值 ArcGIS 原生 GUID 索引、真实 Date/Time-only/Offset 索引与原计划的 ArcGIS 语料和 Windows 门禁。用户原始样本始终只读。
