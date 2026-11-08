# SPX CLI 失败状态贯通与多级网格线索

总计划仍未完成。本次修复命令行调用链遗漏，未完成 SPX 源几何覆盖验证。

## CLI 修改与验证

上一轮 SPX 增加遍历状态后，CLI 的 `query-spx` 与 `query ... spatial` 仍未消费状态。现在两者检查失败，不输出 `Matched FIDs`，向 stderr 输出原因，并返回 2。独立 SPX 解析失败同样返回 2；合法空结果继续返回 0。XY 网格必须为正、有限且相同，移除非正网格自动使用默认值的分支。

扩展真实 CLI 进程合同：正常单点、合法空窗口、页计数越界、零 FID、缺失文件、非法 bbox、非有限坐标和非正/不一致网格；同时复制固定 GDB 后破坏其 SPX，验证带表命令失败退出。临时副本与固定输入均比较前后哈希。

macOS GDAL 3.13.0 和 Linux ARM64 GDAL 3.10.3 各 82 个真实进程检查通过，含历史 FLOAT32 及此前属性 CLI 合同；固定 GDB 111 文件哈希不变。见 [macOS](2026-11-08-spx-cli-status/macos-cli.log)及 [Linux](2026-11-08-spx-cli-status/linux-cli.log)。修改仅涉及 CLI 和进程测试，没有修改 Reader 核心；本次未重跑全套与 sanitizer，不把先前结果当作本次新增 CLI 覆盖。

这些命令仍保留原有手工网格参数语义，没有改成根据表 schema 自动选择网格的 QueryEngine 查询；因此原始候选也不等于完整几何过滤结果。不能将返回码 0 描述为所有源几何和索引语义已核验。

## 下一步发现：多级网格偏移

后续状态：下述公式及混合 signed 键排序问题已在[粗网格修复](2026-11-08-coarse-spx-grid.md)中修复并留证；本节保留修复前调查经过与原始探针。

核对 [GDAL 3.10.3 Writer](https://github.com/OSGeo/gdal/blob/v3.10.3/ogr/ogrsf_frmts/openfilegdb/filegdbindex_write.cpp#L779) 与 [Reader 的 GetScaledCoord](https://github.com/OSGeo/gdal/blob/v3.10.3/ogr/ogrsf_frmts/openfilegdb/filegdbindex.cpp#L2366)：网格偏移参与分辨率比值缩放。当前实现先缩放坐标、再加固定 `2^29`，粗网格的偏移位置不同。

使用现有 CLI fixture builder `write_atx` 构造一条 8 字节 SPX 键：分辨率 `[1,4,16]`、level 2、点 `(0,0)`，cell 为 `2^29/16`，键为 `(2<<62)|(cell<<31)|cell`，FID 为 1。按该公式应命中 row slot 0，但当前返回成功的零候选。见[合成探针](2026-11-08-spx-cli-status/coarse-grid-probe.log)。这不是 ArcGIS 原生多级索引验收，却足以把下一步聚焦到可验证的公式差异。

下一步先补粗网格回归，并以 GDAL 实际生成的多级索引和完整源字段/几何真值验证；同时核对边界取整、负坐标、非整数分辨率比值、level 2 的符号排序，再修复。不能只更改公式后凭单个合成键宣布全部 SPX 正确。源索引的缺项、错键和任意窗口覆盖仍需后续证明。
