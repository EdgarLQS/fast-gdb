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
- 真正的 SequentialScan 流式模式；
- 正常 EOF、Failed 和 moved-from 区分；
- 单 engine 单活动游标守卫；
- generation 防迟到析构；
- engine reopen generation 防旧计划复用；
- 完整字段、NULL、Binary 和 ISO WKB 输出；
- 无几何、NULL/Empty 几何和零长度合法行处理；
- GDAL 完整对象对照与 100K full-feature benchmark runner。

## 2. `move_to` 语义

公开接口：

```cpp
bool FeatureCursor::move_to(uint32_t fid);
```

固定语义：

- 参数是 fast-gdb 零基 FID，不是结果序号；
- 调用成功后，下一次 `next()` 返回第一个满足当前查询且 `FID >= fid` 的对象；
- 支持向前、向后和任意跳跃；
- `move_to(0)` 等价于 rewind；
- 候选模式使用 `lower_bound`；
- 顺序模式从指定物理槽开始，并跳过删除槽；
- 目标之后无结果时返回 `false`，游标进入正常 Exhausted；
- Exhausted 游标可在 engine 未改变且无其他活动游标时重新定位；
- Failed 或 moved-from 游标不能重新定位。

## 3. 第一轮：生命周期与状态机

### P1：EOF 后旧游标可能在 engine reopen 后复用旧计划

问题：

- 游标正常耗尽后释放 engine 占用；
- 调用方可再次执行 `engine.open()`；
- 旧游标仍保存旧 FID 计划和 feature limit；
- 若允许旧游标 `move_to()`，可能把旧计划用于重建后的 parser。

修复：

- `QueryEngine::open()` 每次允许的尝试推进 `open_generation_`；
- cursor 保存规划时 generation；
- `next()` 和 `move_to()` 校验 generation；
- engine 重开后旧游标锁定为 Failed；
- 错误固定为 `query engine was reopened while cursor existed`。

测试：`FeatureCursorReopenTest.ExhaustedCursorCannotReacquireAfterEngineReopen`。

### P1：engine reopen 保留旧 `.spx` 缓存

问题：既有 `QueryEngine::open()` 重建 parser 时没有清理：

- `spatial_index_`；
- `spatial_index_initialized_`；
- `spatial_index_present_`；
- `capabilities_`。

修复：每次允许的 open attempt 先重置 parser、空间索引缓存和能力报告，再打开表。

### P1：EOF 后 `move_to()` 在取得 lease 前读取 tablx

问题：顺序游标正常耗尽后，若另一个游标已活动，旧游标原实现会先调用
`has_feature()` 再尝试取得 lease，形成守卫外读取。

修复：`move_to()` 在任何候选或 tablx 访问前先验证 engine generation 并取得 lease。
取得失败后首次错误锁定为 `another feature cursor is active`。

测试：`FeatureCursorReopenTest.ExhaustedCursorCannotRepositionWhileAnotherCursorIsActive`。

### P1：QueryEngine 地址可移动

问题：cursor PImpl 保存 `QueryEngine*`。若 engine 被 move，游标持有地址失效。

修复：公开删除 QueryEngine copy/move 构造和赋值；静态测试锁定不可移动合同。

## 4. 第二轮：完整对象正确性

### P1：零长度合法行缺少完整字段

问题：既有 `read_record_by_fid()` 对 `blob_length == 0` 返回成功，但
`field_values` 为空。ObjectID-only 表会被游标误判为字段数量错误。

修复：游标在不改变旧 API 的前提下规范化零长度合法行：

- ObjectID 由 `fid + 1` 生成；
- nullable 字段物化为 NULL；
- 重建 nullable bitmap；
- 存在无法从零字节恢复的非空字段时仍失败。

测试：`FeatureCursorZeroLengthTest.ObjectIdOnlyRowProducesACompleteFeatureRecord`。

### 完整对象合同

`next()` 使用局部 `QueryFeature candidate`：

1. 读取完整 `FeatureRecord`；
2. 校验 `record.fid == fid`；
3. 校验或规范化完整字段数量；
4. 读取同一行 `GeometryValue`；
5. 全部成功后才 move 覆盖调用方对象。

结果：失败不会留下半更新输出。

覆盖：

- Int32；
- nullable String；
- Binary 原始字节；
- Geometry WKB；
- 无几何表；
- NULL geometry；
- ObjectID-only 零长度行；
- 删除槽跳过；
- SpatialWhere 与 GDAL `GetNextFeature()` 顺序对照。

## 5. 第三轮：兼容、构建表面与证据

### P0：添加空间守卫时意外覆盖自适应查询实现

问题：初次修改 `query_engine_geometry.cpp` 时整文件替换导致大量既有 profiling、
active feature count、自适应 sequential、batched candidate 和 fallback 逻辑丢失。

修复：从修改前提交恢复完整文件，只插入活动游标检查。对比结果为该文件仅增加
6 行、删除 1 行，不再改变空间 planner。

### P1：无意改变旧 WHERE execution path

问题：为了让 cursor 识别非法 WHERE，曾将旧 `where:sequential` 错误路径改为
`where:invalid`，会改变既有 `query()` 诊断合同。

修复：恢复旧 execution path；`open_cursor()` 根据 WhereClause 的非空错误单独判定
Failed，不要求旧查询迁移。

### P1：测试 helper 中使用 fatal assertion

问题：返回 `std::string` 的 fixture helper 使用 `ASSERT_*`，GTest 会生成无返回值的
提前返回，导致编译失败。

修复：helper 改为显式错误检查和空字符串返回；fatal assertion 只保留在 TEST body。

### P1：GDAL 字段 API 参数错误

问题：`IsFieldSetAndNotNull()` 使用了字段名，但当前 GDAL API 要求字段下标。

修复：先取得字段下标，再进行 NULL 和值对照。本机 GDAL 3.10.3 相关 API 已通过
独立 `-fsyntax-only` 检查。

### 安装与构建面

- `feature_cursor.cpp` 由 Reader `CONFIGURE_DEPENDS` glob 自动加入 GDAL ON/OFF；
- GDAL tests 由 `tests/usegdal/*.cpp` glob 自动加入 GDAL ON；
- `query_engine.h` 是既有公开安装头；
- PImpl 没有把 parser、FieldRef 或 WHERE 内部类型暴露到安装面；
- package consumer 编译 `open_cursor/next/move_to/done/error` 和 move-only 合同。

## 6. 测试代码

### GDAL OFF

- `test_feature_cursor.cpp`
  - move-only cursor；
  - non-movable engine；
  - 方法签名；
  - 默认对象合同。

### GDAL ON

- `test_feature_cursor_gdal.cpp`
  - 顺序流；
  - 所有候选 QueryKind 与旧 `query()` FID 等价；
  - 前后和跳跃 `move_to`；
  - 删除槽；
  - engine 入口守卫；
  - move 构造/赋值；
  - 非法请求和输出不半更新；
  - 无几何表；
  - GDAL 字段、Binary、ISO WKB 对照。
- `test_feature_cursor_empty_geometry.cpp`
  - NULL geometry 作为成功 Empty 对象。
- `test_feature_cursor_zero_length.cpp`
  - ObjectID-only 零长度行。
- `test_feature_cursor_reopen.cpp`
  - engine reopen generation；
  - 其他游标活动时拒绝旧游标 reacquire。
- `test_feature_cursor_benchmark.cpp`
  - 100K full-feature cursor / legacy / GDAL checksum 和耗时；
  - 默认跳过；
  - 证据写入外部目录。

## 7. 自检结果

| 级别 | 数量 | 状态 |
|---|---:|---|
| P0 | 1 | 已恢复既有空间 planner，未保留意外覆盖 |
| P1 | 8 | 已修复并增加静态或运行时回归测试代码 |
| P2 | 0 | 当前未发现需要阻断代码审核的新增项 |

## 8. 尚未形成的实际证据

当前环境无法检出私有仓库，GitHub Actions 历史运行又在创建 step 前结束。因此以下
项目仍未实际运行：

- GDAL OFF Release；
- GDAL ON Release；
- 完整 CTest；
- `ctest -j`；
- package consumer；
- FeatureCursor 运行时功能测试；
- GDAL 完整对象对等测试；
- 100K full-feature benchmark；
- peak RSS；
- 10M full-feature benchmark；
- current/main 交替采样和 5% 门禁；
- `git diff --check main...HEAD`。

因此当前结论是：

```text
FeatureCursor 对应开发完成
三轮静态代码自检完成
可进入独立代码审核
Formal acceptance blocked
```
