# ADR 索引

当前架构决策以 Reader 产品为中心。

| ADR | 状态 | 决策范围 |
|---|---|---|
| [ADR-007：Reader-only 与 GDAL 编辑边界](ADR-007-reader-only-gdal-edit-boundary.md) | Accepted | fast-gdb 只提供 Reader；FileGDB 编辑交给 GDAL/OpenFileGDB；同目录并发读写不支持 |
| [ADR-008：Adaptive Reader 写入检测与 fresh GDAL 回退](ADR-008-adaptive-reader-write-detection-gdal-fallback.md) | Accepted | 可选同进程协调；通过协调状态或 best-effort 文件快照发现源变化；写期间返回 Busy；稳定后使用全新 GDAL 只读连接恢复 |
| [ADR-009：统一 FileGDB 访问与 GDAL/S3 路由](ADR-009-unified-filegdb-routing.md) | Accepted | 本地实现与门禁收口中；跨平台/多 GDAL CI 和真实 S3 证据待完成 |

ADR-001～ADR-005 和旧版 ADR-007 对应的自研 Writer、字段级 Append/Update/Delete、事务、恢复和版本发布方案已被 ADR-007 取代；历史材料保留在 [Writer 历史归档](../archive/writer/README.md)，不属于当前决策集。

## 当前决策关系

```text
fast-gdb
  ├─ fast_gdb::linear
  ├─ fast_gdb::hybrid
  ├─ FileGDB Reader / Query / Geometry / Index
  ├─ optional Adaptive Reader orchestration
  ├─ Unified Dataset/Group/Layer/Cursor routing
  └─ no Writer product

GDAL/OpenFileGDB
  ├─ all FileGDB creation and editing
  ├─ Experimental S3 route through OpenFileGDB
  └─ fresh read-only fallback after source becomes stable
```

ADR-008 已 Accepted，当前实现和测试覆盖同进程协调、WriterPending 排空、Busy、
generation/过期、关闭异常、Unverified fresh fallback，以及 GDAL 写后完整重开矩阵。
未知外部 Writer、跨平台、多 GDAL 版本、sanitizer、压力和性能仍需独立证据；未采用
Adaptive 的调用方仍遵守 ADR-007 的“关闭 Reader → GDAL 写 → GDALClose → 重开 Reader”合同。

安装 consumer 当前覆盖 `linear`、`hybrid` 和可选 `adaptive`；其中 `linear` 的
安装验收必须在无 GDAL 构建中执行。

## 变更规则

1. 不允许在安装面重新增加 Writer target、Writer 头文件或 FileGDB 编辑 ABI；
2. 新增 `.gdbtable/.gdbtablx/.spx/.atx` 写入代码必须先通过新 ADR；
3. 外部写入与 Reader 生命周期边界发生变化时必须同步 ADR-007 和 ADR-008；
4. 同目录并发读写的观测结果不得写成支持合同；
5. Adaptive Reader 检测到活动 Writer 时必须 fail closed，不能立即以 GDAL 并发读取替代；
6. 无协调外部 Writer 检测必须明确标记 best-effort；
7. 在线副本发布能力若未来进入项目，必须作为独立可选组件重新决策，而不能隐式混入 Reader 核心。
8. ADR-009 的本地统一入口和 `FastFileGDB` 已实现；S3 在真实 AWS 验收前只能标记
   Experimental / Unverified。
