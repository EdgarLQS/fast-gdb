> 状态：Current
> 适用版本：v0.2.0+
> 维护入口：docs/gdb/learning/

# FileGDB 数据结构完整学习路线

本计划面向“会使用 C++ 和 AI，但对 FileGDB 格式从零开始”的学习者。目标不是手写完整解析器，而是建立足以解释格式、指导 AI 和审核实现的完整心智模型。

课程分为两个阶段，共 18 周，每周建议投入 5–7 小时：

- 第 1–14 周深入矢量 FileGDB 的逻辑模型、物理格式、几何、索引、元数据和 Reader 全链路；
- 第 15–18 周建立 Raster、Topology、Network、Terrain、Parcel Fabric 等高级数据集的知识地图；
- 每周均包含概念图解、真实数据观察、少量十六进制推导、源码锚点、AI 审核题和闭卷复述；
- 学习结论必须标记证据等级，未知或逆向推断的内容不得描述为公开规范。

## 阶段一：矢量 FileGDB

| 周 | 主题 | 可观察成果 |
|---:|---|---|
| 1 | Geodatabase 基础模型 | 逻辑对象关系图 |
| 2 | 二进制格式基础 | 手工解释字节序、Varint 和位图 |
| 3 | `.gdb` 目录和文件体系 | 文件家族职责表 |
| 4 | 系统表与 Catalog | 系统表关系和图层到物理表映射 |
| 5 | `.gdbtable` 总体布局 | Header、Schema、Record 布局图 |
| 6 | 字段描述符和字段类型 | 字段兼容矩阵 |
| 7 | 记录与字段值编码 | 一条记录的拆解表 |
| 8 | `.gdbtablx`、FID 和存储生命周期 | FID 到记录的完整追踪 |
| 9 | 空间参考和精度网格 | 坐标还原推导 |
| 10 | 基础几何编码 | Point 与 multipart 几何图解 |
| 11 | 高级几何与拓扑 | 曲线、Polygon 和 MultiPatch 边界判断 |
| 12 | `.spx` 空间索引 | 候选集到精确过滤流程 |
| 13 | `.atx`、查询与 Cursor | 查询执行路径图 |
| 14 | Schema 行为、兼容性与可靠性 | 能力和版本矩阵 |

## 阶段二：高级数据集全景

| 周 | 主题 | 可观察成果 |
|---:|---|---|
| 15 | Raster 与影像体系 | Raster/Mosaic 组成图 |
| 16 | Topology、Terrain 与 Network | 规则、派生结构和基础要素关系图 |
| 17 | 现代 Geodatabase 数据模型 | 高级数据集能力边界表 |
| 18 | 综合知识图谱与 AI 审核 | 完整知识图和补丁审核报告 |

## 完成定义

课程结束时，学习者应能：

1. 从 Geodatabase 逻辑对象一路解释到 `.gdbtable` 字节；
2. 解释系统表、TABLX、SPX、ATX、几何和 Query Planner 的协作关系；
3. 区分物理数据可读、Schema 可还原和完整 ArcGIS 行为可执行；
4. 区分 Esri 官方语义、GDAL 实现、fast-gdb 实现、真实数据观察和推断；
5. 审核 AI 生成的解析方案、源码修改和测试证据。

课程入口见 [`../gdb/learning/README.md`](../gdb/learning/README.md)。
