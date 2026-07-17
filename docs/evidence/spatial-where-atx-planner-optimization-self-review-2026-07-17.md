# SpatialWhere `.atx` 与联合规划优化自检

- 日期：2026-07-17
- 分支：`codex/spatial-attribute-query`
- 参考提交：`d15bedd3847560975516f8704090e1433d202bd3`
- 性能对照：`8f2300153840bce5b5be82ae8b19e5bd9f2b0197`
- 状态：实现与三轮静态自检完成；构建、CTest 和性能复测未执行

## 1. 优化依据

`8f23001` 的 100K Point、1% 最终命中率 full-feature profile 显示：

- 查询阶段约 3.16–3.44 ms；
- 属性候选阶段约 2.61–2.86 ms；
- 空间阶段约 0.52–0.56 ms；
- row lookup、字段物化、GeometryModel、WKT、WKB 和 checksum 合计约 0.37 ms。

因此本轮不继续优化 `FeatureCursor::next()`，而是处理 `.gdbindexes/.atx` 和联合查询成本规划。

## 2. 原路径问题

原 `SpatialWhere` 属性索引路径每次查询都会：

1. 解析 `.gdbindexes`；
2. 把整个 `.atx` 文件读入内存；
3. 沿叶链解码所有索引项为 `AttributeIndexEntry`；
4. 把所有条目保存在 `all_entries_`；
5. 再全量扫描 `all_entries_` 获取候选；
6. 对候选排序、去重并与空间 FID 交集；
7. 对交集执行完整 WHERE 复核。

100K、空间精确命中约 10K、属性最终命中 1K 的场景，为缩小 10K 空间候选，需要先物化约 100K 个索引对象，成本与选择性不匹配。

## 3. 实现

### 3.1 direct `.atx` 查询

新增：

```cpp
bool GdbAttributeIndexParser::query_double_direct(
    double value,
    AttrOp op,
    size_t max_fid_count,
    std::vector<uint32_t>& result,
    AttributeIndexQueryMetrics* metrics = nullptr);

bool GdbAttributeIndexParser::query_string_direct(
    const std::string& value,
    AttrOp op,
    size_t max_fid_count,
    std::vector<uint32_t>& result,
    AttributeIndexQueryMetrics* metrics = nullptr);
```

热路径：

- 读取并验证 trailer；
- 从根页第一子节点定位最左叶页；
- 验证并遍历完整叶链；
- 使用 visited bitmap 拒绝循环；
- 校验页面、容量、总条目数、零 FID 和 FID 上界；
- 数值索引直接比较叶页原始字节，不构造临时 `AttributeIndexEntry`；
- 只保存真正匹配的 FID；
- 完整结构验证成功后才排序、去重并发布结果。

旧 `parse()/all_entries()/query_double()/query_string()` 保留，作为兼容接口和 legacy benchmark control。

### 3.2 自适应成本规划

只有以下条件同时满足时，跳过 `.atx`，直接对精确空间候选执行零拷贝字段复核：

```text
spatial_matches <= 65,536
spatial_matches <= active_feature_count / 8
```

即候选不超过 65,536 且不超过活动对象数的 12.5%。

使用活动对象数而不是 `.gdbtablx` 物理槽位数，避免块对齐和删除槽导致错误绕过。

高覆盖、大候选场景继续使用 `.atx` direct 查询和线性交集。

### 3.3 详细指标

`CombinedQueryMetrics` 新增：

- `attribute_index_bypassed`；
- metadata parse；
- `.atx` file load；
- tree navigation；
- leaf scan；
- candidate sort/unique；
- final WHERE recheck；
- page count、pages visited、entries scanned。

## 4. 正确性与安全边界

- `.spx/.atx` 仍只提供候选，最终 WHERE 必须复核；
- direct `.atx` 失败时不覆盖调用方已有结果；
- trailer count 不一致、循环叶链、零 FID、越界 FID 均 fail closed；
- 非 BMP 字符串、`!=`、不安全字符串操作、函数索引和不支持数值编码继续安全回退；
- 绕过 `.atx` 时结果完全来自精确空间候选和 canonical WHERE，不依赖索引内容；
- 因为绕过路径不读取 `.atx` 数据页，未使用索引的物理损坏不会在该次查询中被诊断；这不影响结果正确性，索引健康检查仍由高覆盖查询、专项验证或显式索引检查完成。

## 5. 测试代码

### GDAL OFF

`test_gdb_attribute_index_safety.cpp` 新增：

- direct 数值查询与旧物化查询等价；
- page、entry、candidate 指标；
- trailer count 不一致失败且不发布半结果；
- 循环叶链失败且不发布半结果；
- 零 FID 失败且不发布半结果。

### GDAL ON

新增 `test_spatial_where_adaptive.cpp`：

- 10/100 精确空间候选走 `spatial-where:spatial-candidates`；
- `attribute_index_bypassed=true`；
- 100/100 空间覆盖继续走 `spatial-where:spx+atx`；
- direct `.atx` 指标可见；
- 两条路径的 FID 均与 GDAL 一致。

既有损坏 `.atx` 集成夹具为 100% 空间覆盖，不触发成本绕过，因此仍实际验证损坏索引回退。

### Benchmark

100K FID-only benchmark 已同步自适应路径和详细阶段指标。原始结果默认写系统临时目录或显式外部目录，不提交仓库。

## 6. 三轮静态自检

### 第一轮：规划与语义

P1：初版使用 `.gdbtablx` 物理槽位数作为比例分母，小表可能因块对齐错误绕过 `.atx`。

修复：比例使用 `active_feature_count()`；FID 边界仍使用物理 `feature_count()`。

P1：若在解析索引资格前直接按成本绕过，会掩盖缺失 metadata、函数索引或不安全操作的诊断。

修复：先完成字段、操作符、`.gdbindexes` 和 `.atx` 路径解析，再决定是否支付数据页读取成本。

### 第二轮：fail-closed 与测试

P1：direct 查询若直接向输出 vector 追加，后续发现循环或 count 不一致时可能发布半结果。

修复：使用局部 candidates；完整结构校验成功后才 move 到调用方输出，并增加预填充 vector 回归。

P1：原 benchmark 固定断言 `spatial-where:spx+atx`，会把新的自适应路径误报为失败。

修复：100K/10% 场景明确断言 bypass 路径，并输出新指标；高覆盖专项测试继续锁定 direct `.atx` 路径。

### 第三轮：范围与构建面

- Reader 使用 `CONFIGURE_DEPENDS` glob，新 `.cpp` 自动进入 GDAL OFF/ON；
- GDAL 测试使用 glob，新测试自动收录；
- 未修改空间精确判断、WHERE evaluator、FeatureCursor 状态机或旧 standalone attribute API；
- 没有提交 `.gdb`、构建目录或 benchmark JSON。

## 7. 自检结果

| 级别 | 数量 | 状态 |
|---|---:|---|
| P0 | 0 | 未发现 |
| P1 | 4 | 已修复 |
| P2 | 3 | 已记录：仍全量读取 `.atx`；高覆盖仍扫描完整叶链；`.gdbindexes` 尚未做 QueryEngine 级缓存 |

## 8. 尚未完成的证据

- [ ] GDAL OFF Release；
- [ ] GDAL ON Release；
- [ ] 完整 CTest；
- [ ] `ctest -j`；
- [ ] direct `.atx` safety tests 实际通过；
- [ ] adaptive planner tests 实际通过；
- [ ] 100K FID-only benchmark；
- [ ] 100K full-feature benchmark；
- [ ] 相对 `8f23001` 的同机 current/baseline/GDAL 交替采样；
- [ ] 不超过 5% 回退门禁；
- [ ] 1%/30%/100% 选择性矩阵；
- [ ] strict-cold；
- [ ] peak RSS 和候选 FID 内存；
- [ ] 10M Point/MultiPoint/Polyline；
- [ ] `git diff --check main...HEAD`。

当前结论：

```text
.atx direct 查询与自适应规划实现完成
三轮静态自检完成
性能改善待同机复测
Formal acceptance blocked
```
