# fast-gdb 项目综合汇报

## 一、结论

fast-gdb 采用纯 Reader 产品定位。继续自研 FileGDB Writer 的复杂度、兼容性和维护成本高于收益，字段级编辑统一交给 GDAL/OpenFileGDB。

`src/edgar/usegdal` 作为历史 GDAL/OGR 包装探索保留，但明确降级为 reference only：不构建、不安装、不导出、不进入正式测试门禁，也不构成 Writer 产品。

## 二、核心价值

- 无 GDAL 依赖的纯 C++ Reader；
- 可选 GDAL Hybrid fallback；
- FileGDB 表、tablx、spx、atx 解析；
- FID、属性、空间和联合查询；
- FeatureCursor；
- ISO WKB-first；
- mmap 和查询性能优化；
- GDAL correctness parity。

## 三、产品边界

fast-gdb 不提供：

- FileGDB 创建、追加、更新、删除产品 API；
- Schema 和索引写入产品 API；
- Writer transaction/recovery；
- 在线版本发布；
- Reader/Writer 跨进程锁；
- 写后局部刷新；
- `usegdal` 参考类型的兼容性或生产支持。

## 四、与 GDAL 的分工

| 能力 | fast-gdb | GDAL/OpenFileGDB |
|---|---|---|
| 高性能读取 | 主责 | 对照/回退 |
| WKB-first | 主责 | 兼容对照 |
| 属性/空间查询 | 主责 | 基准对照 |
| Create/Set/Delete Feature | 不提供 | 主责 |
| Schema 编辑 | 不提供 | 主责 |
| Index/REPACK | 不提供 | 主责 |
| 写后验证 | Reader 重开验证 | 完成编辑并关闭 |

`src/edgar/usegdal` 只用于参考 GDAL 包装设计，不承担上表中的正式职责。

## 五、读写阶段

```text
READING
  → drain all fast-gdb objects
QUIESCENT
  → GDAL edit
EDITING
  → close every GDAL object
CLOSED
  → rebuild catalog/table/query state
READING
```

任何 Reader 与 GDAL update 重叠的同目录访问都不属于支持范围。

## 六、测试策略

1. 受支持流程必须断言：写后完整重开读取新数据；
2. 重叠流程只记录 old/new/mixed/error；
3. GDAL CreateFeature/SetFeature/DeleteFeature/CreateField/Index/REPACK 逐项验证 Reader 重开兼容性；
4. 三平台、不同 GDAL 版本和真实大型 GDB 形成矩阵；
5. 正式边界测试直接调用官方 GDAL API，不依赖 `usegdal` 参考层。

## 七、工程收益

- 删除受支持 Writer 语义；
- 降低错误发布和格式破坏风险；
- 集中资源到 Reader 正确性与性能；
- 产品和安装面更清晰；
- 与 GDAL 形成明确互补；
- 保留早期 GDAL 包装经验，供后续独立研究复用。

## 八、参考代码治理

`src/edgar/usegdal`：

- 保留 datasource、dataset、recordset、query、transaction、batch-write 等代码；
- 不由根 CMake 构建；
- 不进入安装、导出、consumer 和 release gate；
- 不保证可构建、可兼容或可用于生产；
- 若未来提升为正式组件，必须另立 ADR、target、测试矩阵和版本策略。

## 九、后续重点

- 修复并闭环 CI runner；
- 扩展 GDAL 写后 Reader 重开矩阵；
- 长时间 Reader 性能与稳定性；
- `.spx/.atx` 大数据和损坏回退；
- 复杂几何和真实 FileGDB parity；
- 文档和示例持续保持 Reader-only 口径；
- `usegdal` 参考层与正式产品持续物理隔离。
