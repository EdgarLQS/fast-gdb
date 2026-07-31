> 状态：Current
> 适用版本：v0.2.0+
> 维护入口：docs/gdb/learning/

# FileGDB 数据结构学习课程

这是一个 18 周、概念与 AI 审核导向的 FileGDB 课程。先阅读 [`MISSION.md`](MISSION.md)，再按顺序学习；不要把“读完页面”记录为掌握，只有完成复述或实操后才新增 `learning-records/`。

## 使用方法

每周建议分三次完成：

1. 概念与结构图：约 2 小时；
2. 真实数据、字节和源码锚点：约 2 小时；
3. AI 审核、闭卷复述和间隔复习：约 1–3 小时。

统一参考：

- [`RESOURCES.md`](RESOURCES.md)：一手资料；
- [`reference/0001-knowledge-map.html`](reference/0001-knowledge-map.html)：八层知识地图；
- [`reference/0002-file-family.html`](reference/0002-file-family.html)：文件家族速查；
- [`reference/0003-evidence-review.html`](reference/0003-evidence-review.html)：证据和 AI 审核清单；
- [`../../plan/04_FileGDB数据结构完整学习路线.md`](../../plan/04_FileGDB数据结构完整学习路线.md)：总体计划。

## 第一阶段：矢量 FileGDB

| 周 | 课程 | 验收问题 |
|---:|---|---|
| 1 | [Geodatabase 基础模型](lessons/0001-geodatabase-model.html) | Feature Class 为什么本质上是带几何字段的表？ |
| 2 | [二进制格式基础](lessons/0002-binary-foundations.html) | offset、Varint 和 bitmap 分别解决什么问题？ |
| 3 | [目录与文件体系](lessons/0003-file-family.html) | 逻辑图层名为什么不能直接由文件名得到？ |
| 4 | [系统表与 Catalog](lessons/0004-system-catalog.html) | UUID 如何连接系统表和业务表？ |
| 5 | [GDBTABLE 布局](lessons/0005-gdbtable-layout.html) | Schema 与 Record 如何共存在一个表文件中？ |
| 6 | [字段描述符](lessons/0006-field-descriptors.html) | 字段类型、标志和默认值为什么必须一起读？ |
| 7 | [记录编码](lessons/0007-record-encoding.html) | NULL、空字符串和零长度 Binary 有何不同？ |
| 8 | [TABLX、FID 与生命周期](lessons/0008-tablx-fid-lifecycle.html) | FID、slot、offset 和物理空间如何区分？ |
| 9 | [空间参考与精度](lessons/0009-spatial-reference.html) | origin、scale、resolution 和 tolerance 如何关联？ |
| 10 | [基础几何编码](lessons/0010-basic-geometry.html) | multipart 几何如何由 part 和 point 组成？ |
| 11 | [高级几何与拓扑](lessons/0011-advanced-geometry.html) | Degraded 与 Unsupported 有何不同？ |
| 12 | [SPX 空间索引](lessons/0012-spatial-index.html) | 为什么空间索引只产生候选？ |
| 13 | [ATX、查询与 Cursor](lessons/0013-query-cursor.html) | 查询规划为什么必须保留最终过滤？ |
| 14 | [Schema、兼容性与可靠性](lessons/0014-schema-compatibility.html) | “记录可读”为何不等于“行为完整”？ |

## 第二阶段：高级数据集全景

| 周 | 课程 | 验收问题 |
|---:|---|---|
| 15 | [Raster 与影像](lessons/0015-raster-imagery.html) | Raster 与普通 Feature Class 的存储职责有何不同？ |
| 16 | [空间约束型数据集](lessons/0016-constrained-datasets.html) | 为什么基础要素可读不代表规则和派生结构可用？ |
| 17 | [现代数据模型](lessons/0017-modern-models.html) | 如何给高级数据集划定证据边界？ |
| 18 | [综合知识图与 AI 审核](lessons/0018-capstone-ai-review.html) | 如何证明 AI 的结论和修改是可信的？ |

## 学习记录规则

第一次真正证明掌握后再创建 `learning-records/0001-<主题>.md`，只写“掌握了什么、证据是什么、这会改变后续什么”，不写流水账。

所有实验从仓库根目录执行，临时输出放入 `build/learning/`；课程本身不要求修改 Reader 代码。
