# fast-gdb 与 GDAL/OpenFileGDB 功能对比矩阵

## 定位

fast-gdb 负责高性能读取；GDAL/OpenFileGDB 负责 FileGDB 编辑，并作为正确性对照和可选 fallback。

| 能力 | fast-gdb | GDAL/OpenFileGDB | 结论 |
|---|---|---|---|
| 打开 FileGDB | 是 | 是 | fast-gdb 主读路径 |
| 顺序扫描 | 是 | 是 | 性能与校验对照 |
| FID 随机读取 | 是 | 是 | 需明确 FID 映射 |
| 属性 WHERE | 是 | 是 | GDAL parity |
| bbox/空间查询 | 是 | 是 | 候选后精确复核 |
| `.spx` 读取 | 是 | 是 | fast-gdb 自研读取 |
| `.atx` 读取 | 是 | 是 | fast-gdb 自研读取 |
| ISO WKB | 是 | 是 | fast-gdb 正式输出 |
| 曲线 | 内置线性化/Hybrid | 是 | 复杂场景 fallback |
| MultiPatch | degraded | 较完整 | 专项 profile |
| CreateFeature | 否 | 是 | 交给 GDAL |
| SetFeature | 否 | 是 | 交给 GDAL |
| DeleteFeature | 否 | 是 | 交给 GDAL |
| Create/Delete Field | 否 | 是 | 交给 GDAL |
| Create/Delete Index | 否 | 是 | 交给 GDAL |
| REPACK | 否 | 是 | 交给 GDAL；写后重开 |
| 事务 | 否 | 模拟/驱动能力 | 不进入 fast-gdb 产品 |
| 在线版本发布 | 否 | 否 | 业务层能力 |

## 读写阶段规则

### 支持

```text
fast-gdb close all Reader state
→ GDAL update
→ GDALClose
→ fast-gdb full reopen
```

### 不支持

```text
fast-gdb Reader open
+ GDAL update same .gdb
```

并发期间 old/new/mixed/error 均可能出现。

## GDAL 写后 Reader 兼容矩阵

| GDAL 操作 | fast-gdb 重开后验证 |
|---|---|
| CreateFeature | 记录数、FID、字段、几何 |
| SetFeature | 新字段值、几何、属性/空间索引 |
| DeleteFeature | 删除槽、扫描、FID lookup |
| CreateField | Schema、nullable、record layout |
| DeleteField | 字段偏移和记录解析 |
| CreateIndex | `.gdbindexes/.atx` 解析和查询 |
| DeleteIndex | 安全回退 |
| REPACK | tablx、row offset、FID 语义 |
| Recompute extent | bbox 和图层范围 |

## 测试解释

- GDAL parity 用于验证 Reader；
- GDAL 生成数据不表示 fast-gdb 提供写入；
- 同目录重叠测试只记录可见性类别；
- 任何单平台 old/new 结果都不是并发支持证明；
- 正式门禁只覆盖完整关闭和重开后的正确性。
