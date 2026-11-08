# 原始 ATX 类型与键宽度校验

本次继续 S4.2 代码自查，修复独立原始 ATX 入口的显式类型约束；总计划仍未完成。

## 修改与复现

显式 Int16/Int32/Float32/String/Geometry 类型曾可接受不兼容的 8 字节数值索引，并按物理宽度解码为 double。新增测试先在旧实现复现失败，见 [修复前记录](2026-11-08-atx-key-layout/before.log)。

两个构造入口现在分别表达未指定类型和显式类型。前者保留历史 2/4/8 字节数值及偶数字节字符串解释；后者必须匹配源字段类型对应的数值宽度、字符串标志或 GUID 的 38 字节布局。不支持的字段类型拒绝。物化与分页共享同一校验，空树也不能绕过。

未知数值宽度不再物化为零。旧 `DecodeNumericUnknownSize` 测试曾要求制造零值，现改为明确失败。分页失败按既有合同不修改调用方原有输出，物化失败清空条目。不能把未指定类型的兼容模式当作已自动识别源 schema；生产调用仍应传入源字段类型。

## 验证

最终完整回归：macOS GDAL 3.13.0 共 607 项，589 PASS / 18 SKIP / 0 FAIL；Linux ARM64 GDAL 3.10.3 共 648 项，629 PASS / 19 SKIP / 0 FAIL。测试均配置固定元数据、用户曲线和历史参考库。见 [macOS 日志](2026-11-08-atx-key-layout/macos-release.log)及 [Linux 日志](2026-11-08-atx-key-layout/linux-release.log)。本次没有重跑 sanitizer，不沿用前次结果作为本次通过结论。

测试覆盖正常旧入口、明确 Float64、五种不兼容显式类型和未知宽度；既有整数、日期、GUID、字符串和 v1/v2 合同一并回归。

本次没有验证所有原始索引语义。真实 B-tree 最后子页下探、高位 ObjectID 键解释、超长 varuint、空间索引完整性等仍按[前次剩余清单](2026-11-08-attribute-integrity.md)继续；ArcGIS 真实语料和 Windows 门禁也保持未完成。
