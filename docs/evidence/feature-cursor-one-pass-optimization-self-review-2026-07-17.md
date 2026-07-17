# FeatureCursor one-pass 完整对象读取优化自检

- 日期：2026-07-17
- 分支：`codex/spatial-attribute-query`
- 优化前基线：`721f1860599ceea4f7c7592d89ed1febb787e14b`
- 参考：`docs/technical/01_性能基准与优化.md` 第 1.10 节
- 状态：**实现与三轮静态自检完成；性能复测和正式验收阻塞**

## 1. 优化目标

优化前 `FeatureCursor::next()` 对每个最终 FID 执行：

1. `read_record_by_fid()`：定位记录、物化字段，并为 Geometry 字段生成兼容 WKT；
2. `read_geometry_value()`：再次定位同一几何 blob、再次生成 `GeometryModel`，再写 ISO WKB。

本轮新增 parser 深层入口：

```cpp
bool GdbTableParser::read_feature_by_fid(
    uint32_t fid,
    FeatureRecord& record,
    GeometryValue& geometry,
    FeatureReadMetrics* metrics = nullptr);
```

目标是同一 FID：

- 一次记录定位；
- 一次字段物化；
- 一次 GeometryModel 解码；
- 从同一模型生成兼容 WKT 和 ISO WKB；
- 失败时不向调用方发布半对象；
- 保留旧 `read_record_by_fid()` 与 `read_geometry_value()`，作为兼容 API 和 benchmark control。

## 2. 实现范围

### Reader

- 新增 `gdb_table_feature.cpp`；
- `FeatureCursor::next()` 改用 one-pass 入口；
- no-geometry、NULL geometry、empty geometry 和 ObjectID-only 零长度记录保持既有 cursor 语义；
- 普通读取不调用 `steady_clock`；只有显式 profile 请求才收集阶段耗时。

### Profile

`QueryRequest` 追加：

```cpp
bool profile_feature_reads = false;
```

`QueryResult::feature_cursor_metrics` 聚合：

- row lookup；
- field materialization；
- GeometryModel decode；
- WKT write；
- WKB write；
- feature count。

查询规划阶段继续使用既有 `CombinedQueryMetrics`。Benchmark profile 样本另行记录 checksum sink，且不进入 median/p95。

### Benchmark 方法学

100K Point full-feature runner 从 schema v1 升为 schema v2：

- 五个样本不再固定 Cursor、legacy、GDAL 顺序；
- 使用确定性轮换顺序降低固定位置偏置；
- 每个样本仍从 engine/dataset open 计时到最后一个完整对象和 checksum；
- profile 样本独立，不计入性能统计；
- 输出声明 `fresh-open-not-strict-cold`；
- 未配置输出目录时写系统临时目录，不在仓库生成 benchmark evidence；
- 每个样本必须满足 Cursor、legacy、GDAL 的 FID、字段、Binary、WKB、结果数和 checksum 一致。

## 3. 第一轮：编译与公开契约

### P1：进程级 profile 环境变量会形成并行测试竞争

首版使用 `FAST_GDB_FEATURE_CURSOR_PROFILE` 控制 profile。`gtest_discover_tests` 会把测试注册成独立 CTest；并行执行时，进程环境状态虽然不跨进程共享，但同一测试进程内的辅助执行和未来并发调用仍会形成隐式全局配置，且调用方无法把 profile 与具体 cursor 请求绑定。

修复：

- 删除 FeatureCursor 对 profile 环境变量的依赖；
- 改为 `QueryRequest::profile_feature_reads`；
- `open_cursor()` 在创建 Impl 时固定 profile 开关；
- 默认 `false`，普通路径无 clock 调用；
- 增加 profile-on/profile-off 回归。

### P1：安装 consumer 未锁定新增 profile 契约

修复：package consumer 编译：

- `QueryRequest::profile_feature_reads`；
- `FeatureCursorMetrics`；
- `QueryResult::feature_cursor_metrics`。

### 构建面检查

- Reader 使用 `CONFIGURE_DEPENDS` glob，新 `gdb_table_feature.cpp` 自动进入 GDAL ON/OFF；
- 新 GDAL 测试由 `tests/usegdal/*.cpp` 自动收录；
- one-pass 实现没有引入 GDAL 头或符号；
- parser/FieldRef/WHERE 内部类型未新增到 `query_engine.h`。

## 4. 第二轮：字段与几何正确性

### P0：缺失 geometry slice 可能被误报为合法 NULL geometry

首版在 schema 有 Geometry 字段、非零记录却没有定位到 geometry slice 时，与真正 NULL geometry 共用 Empty 分支。损坏布局可能因此伪装成成功对象。

修复：

- `present == false` 直接失败；
- 只有 nullable bitmap 明确标记 Geometry 为 NULL 时返回 `GeometryStatus::Empty`；
- geometry offset/size 再次做边界校验。

### P1：记录偏移和长度加法存在溢出风险

首版 fd 路径使用 `raw_offset + 4` 参与边界判断。修复为：

- 先确认 `offset <= file_size`；
- 使用 `file_size - offset >= sizeof(length)`；
- 再计算安全的 `payload_offset`；
- blob 长度使用减法式边界检查；
- `best_padding` 使用 `size_t::max()`，避免 32 位平台 `blob_length + 1` 溢出。

### 正确性对照

新增旧/新路径对照：

- Point：Int32、String、Binary、nullable bitmap、兼容 WKT、ISO WKB；
- NULL geometry：完整属性对象 + Empty；
- MultiPoint；
- Polyline；
- Polygon 含洞；
- profile request scope。

Point、MultiPoint、Polyline、Polygon 和内置曲线的有效模型都通过 `GeometryModel -> WktWriter/WkbWriter`，one-pass 从同一模型生成两种输出。MultiPatch 在当前 `read_geometry_value()` 中本就不属于成功完整对象，本轮不扩大支持边界。

## 5. 第三轮：Benchmark 与证据边界

### P1：固定执行顺序偏置

基线固定 Cursor、legacy、GDAL，GDAL 总在最后。修复为五样本轮换：

1. Cursor / legacy / GDAL；
2. legacy / GDAL / Cursor；
3. GDAL / Cursor / legacy；
4. Cursor / GDAL / legacy；
5. legacy / Cursor / GDAL。

这降低固定位置偏置，但仍不是 strict-cold，也不是统计随机化。JSON 必须保留相应声明。

### P1：profile 缺少查询规划和 checksum sink

修复：

- 查询规划使用 `CombinedQueryMetrics`；
- 完整对象阶段使用 `FeatureCursorMetrics`；
- checksum sink 在独立 profile 样本中计时；
- profile 样本不进入正式五样本 median/p95。

### P1：默认 evidence 路径可能污染仓库

修复：未设置 `FAST_GDB_BENCHMARK_OUTPUT_DIR` 时写入系统临时目录下的 `fast-gdb-benchmark-results`。

### 性能结论边界

本轮没有实际执行新的 Release benchmark。因此：

- 第 1.10 节的 3.869 ms / 3.899 ms / 1.366 ms 仍是 `721f186` 的优化前基线；
- 不能宣称 one-pass 已提速；
- 不能宣称与 GDAL 的差距已缩小；
- profile 阶段占比必须以新 schema-v2 evidence 为准；
- strict-cold、1%/30%/100%、MultiPoint/Polyline 和 peak RSS 仍待补齐。

## 6. 自检结果

| 级别 | 数量 | 状态 |
|---|---:|---|
| P0 | 1 | 已修复：损坏行不得伪装为 NULL geometry |
| P1 | 5 | 已修复：profile scope、安装契约、边界溢出、执行顺序、profile/evidence 完整性 |
| P2 | 1 | 记录物化逻辑在 canonical 与 one-pass 路径间仍有重复；当前以逐字段/逐几何 parity 测试约束，后续应收敛为单一布局实现 |

## 7. 尚未完成的实际证据

- [ ] GDAL OFF Release；
- [ ] GDAL ON Release；
- [ ] 完整 CTest；
- [ ] `ctest -j`；
- [ ] package consumer；
- [ ] one-pass parity tests 实际通过；
- [ ] schema-v2 100K benchmark；
- [ ] one-pass 相对 legacy/current 的 5% 门禁；
- [ ] strict-cold 对照；
- [ ] 1%/30%/100% 选择性；
- [ ] Point/MultiPoint/Polyline 矩阵；
- [ ] peak RSS 和候选 FID 内存；
- [ ] `git diff --check main...HEAD`。

当前结论：

```text
one-pass 优化代码完成
三轮静态自检完成
性能提升待实际复测
Formal acceptance blocked
```
