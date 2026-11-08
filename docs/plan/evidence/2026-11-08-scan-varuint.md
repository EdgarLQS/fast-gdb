# 字段与几何扫描长度溢出自查

继续总计划的损坏输入与只读完整性检查；本次不代表总计划完成。

## 问题与修改

字段候选扫描和几何定位各有一个 `read_varuint_checked`。旧实现允许第十字节携带大于 1 的有效载荷；在左移 63 位时，高位会被截掉。例如九个 `0x80` 后跟 `0x02` 会回绕成长度 0，而不是报告溢出。字段扫描因此可能发布假空值，几何扫描可能把坏长度视为合法空 blob。完整记录读取使用 BinaryReader，已拒绝同类输入，导致各路径不一致。

两个热路径现在在移位前检查第十字节，只允许剩余的 bit 63，不允许继续延续。规则与 BinaryReader 一致；没有新增依赖或改变正常值编码，未强制拒绝范围内的非最短编码。

新增 `GeometryLengthConformance.OverflowingVaruintCannotWrapIntoEmptyGeometry`：非 nullable 的 ObjectID/Geometry 合成行，覆盖第十字节 `02/04/7e/80/81`，分别检查完整记录读取、字段候选扫描、几何全扫描。字段扫描不发布坏行；几何扫描明确返回不可读标记（null pointer、非 NULL），供查询层传播失败。不是 ArcGIS 真实语料验收。

测试使用映射内存的合成行；没有把不支持该映射模式的 `scan_geometry_candidates` 当作已执行覆盖。该路径与全扫描共用已修复的几何定位器，但仍需真实文件 I/O 合同补充。

## 验证

macOS 定向组 16 PASS / 1 SKIP（显式未启用的位图规模基准），包括新增溢出、Offset 前置字段、可空历史布局和坏行合同，见[日志](2026-11-08-scan-varuint/macos-focused.log)。

最终完整回归均配置固定元数据、用户曲线和历史参考库：macOS GDAL 3.13.0 共 608 项，590 PASS / 18 SKIP / 0 FAIL；Linux ARM64 GDAL 3.10.3 共 649 项，630 PASS / 19 SKIP / 0 FAIL。见 [macOS](2026-11-08-scan-varuint/macos-release.log)及 [Linux](2026-11-08-scan-varuint/linux-release.log)。本次不沿用此前 sanitizer 结果作为当前源码通过证明。

尚需继续核对原始 B-tree 最后子页、空间候选完整性、特殊格式及真实 ArcGIS/Windows 门禁，详见[总计划](../22_完整只读能力与GDAL对照升级计划.md)。
