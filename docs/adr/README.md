# ADR 索引

当前架构决策以 Reader 产品为中心。

| ADR | 状态 | 决策范围 |
|---|---|---|
| [ADR-007：Reader-only 与 GDAL 编辑边界](ADR-007-reader-only-gdal-edit-boundary.md) | Accepted | fast-gdb 只提供 Reader；FileGDB 编辑交给 GDAL/OpenFileGDB；同目录并发读写不支持 |

ADR-001～ADR-005 和旧版 ADR-007 对应的自研 Writer、字段级 Append/Update/Delete、事务、恢复和版本发布方案已被本决策取代并从当前文档删除。

## 当前决策关系

```text
fast-gdb
  ├─ fast_gdb::linear
  ├─ fast_gdb::hybrid
  ├─ FileGDB Reader / Query / Geometry / Index
  └─ no Writer product

GDAL/OpenFileGDB
  └─ all FileGDB creation and editing
```

## 变更规则

1. 不允许在安装面重新增加 Writer target、Writer 头文件或 FileGDB 编辑 ABI；
2. 新增 `.gdbtable/.gdbtablx/.spx/.atx` 写入代码必须先通过新 ADR；
3. 外部写入与 Reader 生命周期边界发生变化时必须更新 ADR-007；
4. 同目录并发读写的观测结果不得写成支持合同；
5. 在线副本发布能力若未来进入项目，必须作为独立可选组件重新决策，而不能隐式混入 Reader 核心。
