# fast-gdb 文档索引

fast-gdb 当前产品定位为 **FileGDB Reader only**。项目不提供受支持的 FileGDB Writer；创建、追加、更新、删除、Schema 编辑、索引维护和 REPACK 统一由 GDAL/OpenFileGDB 完成。

`src/edgar/usegdal` 作为历史 GDAL/OGR 包装探索代码保留，仅供设计比较和后续研究；它不构建、不安装、不导出，也不属于产品兼容性范围。

## 核心入口

| 文件 | 内容 |
|---|---|
| [Reader 读取流程专题](technical/06_Reader读取流程专题.md) | 目录扫描、系统表、表解析、索引规划、FeatureCursor、WKB-first 和对象生命周期 |
| [GDAL 写入与 fast-gdb 读取边界](testing/03_GDAL边界与读写测试.md) | 当前停读→GDAL 写→重开合同，以及计划中的 Adaptive Reader 使用语义 |
| [ADR-007：Reader-only 与 GDAL 编辑边界](adr/ADR-007-reader-only-gdal-edit-boundary.md) | 当前产品定位、决策理由、支持合同和非目标 |
| [ADR-008：Adaptive Reader 写入检测与 fresh GDAL 回退](adr/ADR-008-adaptive-reader-write-detection-gdal-fallback.md) | Proposed：写期间 fail closed、源稳定后 fresh GDAL 只读恢复 |
| [Adaptive Reader 实施计划](planning/22_AdaptiveReader写入检测与GDAL回退计划.md) | 文件快照、协调探针、Reader 失效、fresh GDAL、测试和平台验收阶段 |
| [GDAL/Reader 边界架构说明](architecture/gdal-write-reader-boundary.md) | 生命周期、缓存失效、并发可见性和 Adaptive 计划架构 |
| [并发可见性观测证据](evidence/gdal-write-fast-gdb-read-characterization-2026-07-22.md) | 真实 OpenFileGDB 测试设计、分类结果和证据边界 |
| [`usegdal` 参考层说明](../src/edgar/usegdal/README.md) | 非产品 GDAL/OGR RAII、查询、事务和批量写入参考代码的边界 |
| [Writer 历史归档](archive/writer/README.md) | 已废弃 Writer ADR、规划、评审和证据；仅用于历史追溯 |

## 产品目标

| 安装目标 | 说明 |
|---|---|
| `fast_gdb::linear` | 无 GDAL 依赖的纯 C++ Reader |
| `fast_gdb::hybrid` | fast-gdb Reader 主路径 + GDAL 复杂几何回退 |

计划中的 Adaptive Reader 仍处于 ADR-008 Proposed 阶段，尚未作为安装 target 或当前能力发布。

不存在 `fast_gdb::writer`。`include/fast_gdb/writer`、自研二进制 Writer、Writer 工具、Writer 工作流和 Writer 专项文档均不属于当前产品。

`src/edgar/usegdal` 中可能存在 write、transaction 或 batch-write 示例，但这些文件不进入根 CMake target、安装包、package consumer 或发布门禁，其存在不构成 Writer 支持声明。

## 当前读写边界

### 支持

```text
关闭全部 fast-gdb Reader
    → GDAL/OpenFileGDB 独占修改目标 .gdb
    → 关闭全部 GDAL 对象
    → 重新创建 fast-gdb Reader
```

重开后的 Reader 必须读取 GDAL 已提交的新数据。

### 不支持

```text
fast-gdb Reader 保持打开
    + GDAL 同时 update 同一个 .gdb 目录
```

并发期间可能出现 old/new/mixed/error，项目不承诺任何固定结果。旧 Reader 在 GDALClose 后也不能继续复用，必须销毁并完整重开。

## 计划中的 Adaptive Reader 边界

ADR-008 计划增加一个可选 Reader 编排层：

```text
稳定数据源
  → fast-gdb 快路径

活动 Writer / 读取期间源变化
  → 丢弃结果
  → SourceBusy / ReaderExpired

写入结束且源稳定
  → fresh GDAL read-only fallback
  → 完整物化并关闭 Dataset
  → 后置验证通过后返回
```

协调模式通过调用方提供的 `writer_active/generation` 获得确定性行为。未知外部 Writer 只能采用 best-effort 文件快照检测，不能承诺绝对无漏检。

该计划不引入 Writer、GDAL update wrapper、事务、发布层或 marker 写入能力。ADR-008 Accepted 前，ADR-007 仍是唯一正式合同。

## 教程文档

| 文件 | 内容 |
|---|---|
| [组件库设计与使用](usage/01_组件库设计与使用.md) | Reader、可选 GDAL Hybrid 和 reference-only `usegdal` 边界 |
| [几何 WKB 曲线支持与迁移](usage/02_几何WKB曲线支持与迁移.md) | GeometryModel/GeometryValue、WKB、曲线和 Hybrid |
| [真实数据验收资料清单](usage/05_fast-gdb真实数据验收资料清单.md) | Reader 新能力的真实数据验收规范 |
| [空间属性联合查询代码审核指南](usage/10_空间属性联合查询代码审核指南.md) | 查询路径、回退和审核点 |
| [GDAL 写入与 fast-gdb 读取边界](testing/03_GDAL边界与读写测试.md) | 当前 Reader/Writer 阶段合同和 Adaptive Reader 计划用法 |
| [GDB 二进制格式图解教程](tutorial/04_GDB二进制格式图解教程.md) | FileGDB 二进制结构和 Reader 链路 |

## 测试文档

| 文件 | 内容 |
|---|---|
| [测试总览与验收规则](testing/01_测试总览与验收规则.md) | 结果分类和发布门禁 |
| [功能测试矩阵](testing/02_功能测试矩阵.md) | 功能、GDAL parity、真实数据和 Adaptive 覆盖 |
| [GDAL 边界与读写测试](testing/03_GDAL边界与读写测试.md) | 写前关闭、写后重开和并发观测边界 |
| [构建与平台矩阵](testing/04_构建与平台矩阵.md) | GDAL 3.9.3 基线和后续平台验收 |
| [测试数据与真实数据验收](testing/05_测试数据与真实数据验收.md) | fixture、manifest 和真实数据 |
| [性能基准与回归门禁](testing/06_性能基准与回归门禁.md) | benchmark 和回归规则 |
| [测试索引](testing/07_测试索引.md) | 文档到测试 target 的索引 |

## 技术专题

| 文件 | 内容 |
|---|---|
| [性能模型与优化约束](testing/06_性能基准与回归门禁.md) | Reader 基准和性能根因 |
| [索引构建方案](technical/02_索引构建方案.md) | `.spx/.atx` 读取、候选和验证 |
| [技术探索与教训](technical/03_技术探索与教训.md) | B+ 树、LRU、mmap 和失败实验 |
| [读查询性能与工程实践问答](technical/05_读查询性能与工程实践问答.md) | Reader 工程问题 |
| [Reader 读取流程专题](technical/06_Reader读取流程专题.md) | Reader 端到端流程和生命周期 |

## 状态原则

- fast-gdb 正式产品只读；
- 所有 FileGDB 编辑统一由 GDAL/OpenFileGDB 或 ArcGIS 完成；
- `usegdal` 只保留为非产品参考，不建立 API/ABI 或运行时承诺；
- 当前同一 GDB 的外部写入和 fast-gdb 并发读取不支持；
- 当前写前关闭 Reader，写后完整重开；
- 计划中的 Adaptive Reader 检测到活动 Writer 时 fail closed；
- fresh GDAL fallback 只能在源稳定后执行，且必须前后验证；
- 无协调外部 Writer 检测只能标记 best-effort；
- 在线不停读更新需要业务系统实现副本和原子切换；
- Reader 正式输出保持 ISO WKB-first；
- `.spx/.atx` 候选必须最终复核；
- MultiPatch、关系、域、层级、栅格和稀疏 64-bit ObjectID 仍按专项 profile 验收；
- 观测性测试结果不能被解释为并发读写支持声明。
