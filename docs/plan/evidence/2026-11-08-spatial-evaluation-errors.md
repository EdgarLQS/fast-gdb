# 空间谓词求值失败传播

继续只读完整性自查，总计划仍未完成。

## 问题及修复

源行布局可读，但非空几何模型不支持或损坏时，普通空间查询和融合空间/属性扫描曾增加 `invalid_geometries` 后跳过该行，继续返回成功。这样调用方无法区分完整空结果与无法求值造成的部分结果。[修复前复现](2026-11-08-spatial-evaluation-errors/before.log) 使用真实 GDAL 点表的临时副本，保持行和 blob 长度，仅将某个几何类型改为不支持的值。

现在两个执行器遇到需要精确求值、但模型非 Empty/Null 且无效、坐标变换不可用或查询网格非有限时，返回 Unsupported、非空错误并清空部分 FID。融合路径停止当前扫描，不将语义失败当成可忽略候选。空间候选物理读取失败仍按前次实现尝试完整回退。

合法 NULL、零长度及明确 Empty/Null 模型继续按既有不相交语义处理；它们可以计入既有诊断计数，但不会导致 Unsupported。属性过滤本身不强制解码几何。

## 验证及范围

新增测试覆盖普通 SpatialBbox、SpatialWhere 的宽窗口和窄窗口；万点网格 fixture 的窄窗口明确断言 `fused_spatial_attribute_scan=true`，验证融合路径也返回非部分错误。原三点 fixture 默认保持不变。macOS 相关定向 22/22 通过，见 [日志](2026-11-08-spatial-evaluation-errors/macos-focused.log)。

最终 macOS GDAL 3.13.0 完整回归 609 项：591 PASS / 18 SKIP / 0 FAIL；Linux ARM64 GDAL 3.10.3 完整回归 650 项：631 PASS / 19 SKIP / 0 FAIL，33.06 秒。两者均配置用户曲线、固定元数据和历史参考库，原始日志见 [macOS](2026-11-08-spatial-evaluation-errors/macos-release.log)及 [Linux](2026-11-08-spatial-evaluation-errors/linux-release.log)。

本次没有取消已有 bbox 不相交/完全包含的快捷判断；这些路径能从已读取范围作出决定，不一定构建模型。测试证明的是实际精确求值失败的传播，不能据此声称每条几何都经过完整编码或拓扑验收，也不能证明 SPX 缺失候选的完整性。后者仍需独立核对。没有重跑 sanitizer，不将此前通过结果当作本次源码证据。

真实 ArcGIS MultiPatch、旧格式、高位 ObjectID、附件、高级对象、Windows 及完整空间索引一致性继续按[总计划](../22_完整只读能力与GDAL对照升级计划.md)推进。
