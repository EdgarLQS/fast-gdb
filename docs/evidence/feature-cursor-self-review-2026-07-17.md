# FeatureCursor 完整 Feature 流式迭代自检记录

- 日期：2026-07-17
- 分支：`codex/spatial-attribute-query`
- 基线：`main@d8784e7`
- 参考计划提交：`4425a8485b4ee28265e2e6c25996c6322090cfce`
- 状态：**实现与三轮静态自检完成，Formal acceptance blocked**

## 1. 实现范围

本轮在既有空间属性联合查询基础上新增：

- 公开 `QueryFeature`；
- move-only `FeatureCursor`；
- `QueryEngine::open_cursor()`；
- `FeatureCursor::next()`；
- `FeatureCursor::move_to(fid)`；
- 候选 FID 模式；
- 真正 SequentialScan 流式模式；
- 正常 EOF、Failed 和 moved-from 区分；
- 单 engine 单活动 cursor；
- cursor generation 和 engine open generation；
- 完整字段、NULL、Binary 和 ISO WKB；
- 无几何、NULL/Empty geometry 和零长度合法行；
- GDAL 完整对象对照和 100K full-feature runner。

## 2. `move_to` 语义

```cpp
bool FeatureCursor::move_to(uint32_t fid);
```

- 参数是 fast-gdb 零基 FID，不是结果序号；
- 下一次 `next()` 返回第一个满足当前查询且 `FID >= fid` 的对象；
- 支持向前、向后、rewind 和任意跳跃；
- 候选模式使用 `lower_bound`；
- 顺序模式从指定物理槽开始并跳过删除槽；
- target 后无结果时进入正常 Exhausted；
- Exhausted cursor 可在 engine 未改变且无其他活动 cursor 时重新定位；
- Failed 或 moved-from 不能重新定位。

## 3. 第一轮：生命周期与状态机

### P1：EOF 后旧 cursor 可能在 engine reopen 后复用旧计划

修复：

- `QueryEngine::open()` 推进 `open_generation_`；
- cursor 保存规划 generation；
- `next()` 和 `move_to()` 校验；
- engine 重开后旧 cursor 锁定为 Failed；
- 错误为 `query engine was reopened while cursor existed`。

测试：`FeatureCursorReopenTest.ExhaustedCursorCannotReacquireAfterEngineReopen`。

### P1：engine reopen 保留旧 `.spx` 缓存

修复：每次允许的 open attempt 重置 parser、`spatial_index_`、初始化标志和能力报告。

### P1：EOF 后 `move_to()` 在取得 lease 前读取 tablx

修复：任何候选或 tablx 访问前先验证 open generation 并取得 lease。其他 cursor 活动时
首次错误锁定为 `another feature cursor is active`。

测试：`FeatureCursorReopenTest.ExhaustedCursorCannotRepositionWhileAnotherCursorIsActive`。

### P1：QueryEngine 地址可移动

初始修复删除了 QueryEngine copy/move 构造和赋值。后续以稳定堆控制块恢复 noexcept 移动构造：活动 cursor 在 engine 移动后继续有效，moved-from engine 安全不可用；静态与运行时测试共同锁定该合同。

### P1：capability 打开失败仍可能创建 cursor

问题：parser 可能成功加载，但 `CapabilityReport::can_read_layer()` 返回 false。只检查
`parser_` 会把失败的 `open()` 当作可用。

修复：

- 增加 `opened_`；
- 只有 capability 判定成功才设置 true；
- 判定失败时保留能力报告但释放 parser；
- `open_cursor()` 因 parser 为空返回 `table not open` Failed cursor。

## 4. 第二轮：完整对象正确性

### P1：零长度合法行缺少完整字段

问题：既有 `read_record_by_fid()` 对 `blob_length == 0` 返回成功，但 field_values 为空。
ObjectID-only 表会被 cursor 误判为字段数量错误。

修复：cursor 在不改变旧 API 的前提下规范化零长度合法行：

- ObjectID 由 `fid + 1` 生成；
- nullable 字段物化为 NULL；
- 重建 nullable bitmap；
- 无法从零字节恢复的非空字段仍失败。

测试：`FeatureCursorZeroLengthTest.ObjectIdOnlyRowProducesACompleteFeatureRecord`。

### 完整对象合同

`next()` 使用局部 `QueryFeature candidate`：

1. 读取完整 FeatureRecord；
2. 校验 `record.fid == fid`；
3. 校验或规范化字段数量；
4. 读取同一行 GeometryValue；
5. 全部成功后才 move 覆盖调用方输出。

覆盖 Int32、nullable String、Binary、WKB、无几何、NULL geometry、ObjectID-only、
删除槽和 SpatialWhere/GDAL 顺序对照。

## 5. 第三轮：兼容、构建表面与证据

### P0：添加空间守卫时意外覆盖自适应查询实现

问题：初次替换 `query_engine_geometry.cpp` 导致既有 profiling、active feature count、
adaptive sequential、batched candidate 和 fallback 逻辑丢失。

修复：恢复修改前完整文件，只增加活动 cursor 检查。最终该文件相对原实现仅增加
6 行、删除 1 行。

### P1：无意改变旧 WHERE execution path

修复：恢复旧 `where:sequential`；`open_cursor()` 根据 WhereClause 的非空错误单独判定
Failed。

### P1：测试 helper 中使用 fatal assertion

修复：返回值 helper 使用显式检查和空字符串失败；`ASSERT_*` 只保留在 TEST body。

### P1：GDAL 字段 API 参数错误

修复：`IsFieldSetAndNotNull()` 使用字段下标。本机 GDAL 3.10.3 相关调用已通过独立
`-fsyntax-only` 检查。

### P1：full-feature benchmark 未包含打开阶段

问题：首版只测 cursor/legacy/GDAL 迭代，未包含 QueryEngine 或 GDAL dataset 打开，
不符合参考计划的端到端计时范围。

修复：每个样本分别新建并打开 cursor engine、legacy engine 和 GDAL dataset；计时从
engine/dataset open 开始，到最后一个完整 Feature 和 checksum 结束。JSON 增加：

```text
timing_scope = engine-or-dataset-open-through-last-feature
```

### 安装与构建面

- `feature_cursor.cpp` 由 Reader glob 自动进入 GDAL ON/OFF；
- GDAL tests 由 `tests/usegdal/*.cpp` 自动进入 GDAL ON；
- PImpl 不暴露 parser、FieldRef 或 WHERE 内部类型；
- package consumer 编译 `open_cursor/next/move_to/done/error`；
- benchmark 默认跳过，结果只写外部 evidence 目录。

## 6. 测试代码

### GDAL OFF

- `test_feature_cursor.cpp`
  - move-only cursor；
  - engine 可移动构造但不可复制/移动赋值，并覆盖 moved-from 安全边界；
  - 方法签名；
  - 默认对象合同。

### GDAL ON

- `test_feature_cursor_gdal.cpp`
  - 顺序流和所有候选 QueryKind；
  - 前后和跳跃 `move_to`；
  - 删除槽、engine guard、move ownership；
  - 非法请求和输出不半更新；
  - 无几何表；
  - GDAL 字段、Binary、ISO WKB。
- `test_feature_cursor_empty_geometry.cpp`
  - NULL geometry 成功 Empty。
- `test_feature_cursor_zero_length.cpp`
  - ObjectID-only 零长度行。
- `test_feature_cursor_reopen.cpp`
  - engine reopen generation；
  - 其他 cursor 活动时拒绝旧 cursor reacquire。
- `test_feature_cursor_benchmark.cpp`
  - 100K cursor/legacy/GDAL 端到端 checksum 和耗时；
  - 默认跳过；
  - 外部 evidence 输出。

## 7. 自检结果

| 级别 | 数量 | 状态 |
|---|---:|---|
| P0 | 1 | 已恢复既有空间 planner，未保留意外覆盖 |
| P1 | 10 | 已修复并增加静态或运行时回归测试代码 |
| P2 | 0 | 当前未发现需要阻断独立代码审核的新增项 |

## 8. 尚未形成的实际证据

- GDAL OFF Release；
- GDAL ON Release；
- 完整 CTest；
- `ctest -j`；
- package consumer；
- FeatureCursor 运行时功能测试；
- GDAL 完整对象对等；
- 100K full-feature benchmark；
- peak RSS；
- 10M full-feature benchmark；
- current/main 交替采样和 5% 门禁；
- `git diff --check main...HEAD`。

当前结论：

```text
FeatureCursor 对应开发完成
三轮静态代码自检完成
可进入独立代码审核
Formal acceptance blocked
```
