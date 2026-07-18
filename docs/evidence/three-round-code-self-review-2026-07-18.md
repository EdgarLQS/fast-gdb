# 三轮代码自检查报告

- 日期：2026-07-18
- 分支：`codex/spatial-attribute-query`
- 审查对象：计划 21、计划 22、四目录注释批次的当前分支头
- 方法：三轮使用不同关注点独立检查；本报告不把无法执行的 CI 当作通过证据。

## 结论

三轮静态自检查已完成，但当前不能判定为“代码审查通过”。共发现 3 个需要修复的问题：

1. `GeometryValue::to_wkt()` 没有进入 `fast_gdb_geometry_core`，最小几何产品存在符号归属不完整风险；
2. `FeatureCursor` 捕获裸 `CursorControl*`，游标晚于 `QueryEngine` 析构时可能悬空访问；
3. WKB→WKT 对普通几何中的 NaN/Inf 未统一 fail closed，可能输出不可消费 WKT。

在这 3 项修复并补测试前，PR 应保持 Draft。

---

## 第一轮：接口契约与功能正确性

### 检查范围

- `GeometryValue::to_wkt()` 的声明、定义、安装与链接归属；
- `read_record_by_fid()` 的 Geometry 跳过合同；
- `FeatureCursor` 的公开生命周期、移动语义和单游标租约；
- package consumer 对新 API 的引用。

### 已确认正确

- `read_record_by_fid()` 对 Geometry 字段只读取长度并跳过 blob，字段槽写入空字符串；
- `GeometryValue::to_wkt()` 不依赖 GDAL，也不重新读取 FileGDB；
- package consumer 已引用 `GeometryValue::to_wkt()` 和 `FeatureCursor` 新接口；
- `wkt_write_ms` 已从公开 cursor 指标移除。

### 发现 1：geometry core 的符号归属不完整

**严重度：高**

`GeometryValue` 定义在公开的 `geometry_model.h` 中，`fast_gdb_geometry_core` 也公开安装并导出 reader 几何头文件；但 `GeometryValue::to_wkt()` 的实现位于 `wkb_reader.cpp`，当前 `FAST_GDB_GEOMETRY_SOURCES` 没有包含该翻译单元。

结果：

- 链接 `explorgdb_reader_lib` 的消费者通常能得到该符号；
- 只链接 `fast_gdb_geometry_core` 并调用 `to_wkt()` 的消费者可能出现未定义符号；
- 最小 geometry 测试目标不能直接覆盖 `to_wkt()`。

### 修复要求

- 将 `wkb_reader.cpp` 纳入 `FAST_GDB_GEOMETRY_SOURCES`；
- 确保它不再重复编入 `explorgdb_reader_lib`；
- 将 `test_wkb_to_wkt.cpp` 和直接调用 `to_wkt()` 的 exact writer test 纳入最小 geometry 测试清单；
- 增加仅链接 `fast_gdb::geometry_core` 的 package consumer 场景。

---

## 第二轮：边界条件、资源所有权与跨平台安全

### 检查范围

- cursor 回调捕获和对象析构顺序；
- mmap/row buffer 指针生命周期；
- WKB 计数、字节序、嵌套类型和非有限坐标；
- Writer staging、backup、rename 和 rollback 路径；
- Windows/macOS/Linux exclusive rename 语义。

### 已确认正确

- WKB 读取检查尾随字节、嵌套几何类型、维度一致性和计数上限；
- record 读取对 blob 长度、文件范围和 nullable bitmap 变体采取 fail-closed；
- Writer 发布前检查源指纹，并在 staging 替换失败时尝试恢复 backup；
- Windows 发布没有使用 `exists()` 预检查，避免 TOCTOU；
- Writer 错误进入锁定状态，避免失败后继续修改 staging。

### 发现 2：FeatureCursor 控制块可能悬空

**严重度：高**

`FeatureCursor::Impl` 保存的 acquire/release/engine-valid 回调捕获 `QueryEngine::CursorControl*` 裸指针。公开 API 没有从类型系统上阻止游标晚于引擎析构。

潜在场景：

1. 创建 `QueryEngine`；
2. `open_cursor()` 返回游标；
3. 移动游标到更长生命周期对象；
4. `QueryEngine` 析构；
5. 游标调用 `next()/move_to()` 或析构释放租约；
6. 回调解引用已经释放的 `CursorControl`。

### 修复要求

- 将 `cursor_control_` 改为 `std::shared_ptr<CursorControl>`；
- 回调按值捕获 shared ownership；
- parser 生命周期仍需明确：可选择让 cursor 持有共享的 parser 状态，或在引擎析构时使共享 control 标记 owner 已失效，且 cursor 在接触 `table_` 前 fail closed；
- 增加“游标晚于引擎析构”的 ASan/普通单元测试。

### 发现 3：非有限坐标未统一拒绝

**严重度：中**

当前 WKB→WKT 仅对 Point 的 X/Y NaN 组合识别 Empty；LineString、Polygon、集合子几何以及非空 Point 的 Z/M/XY 没有统一拒绝 NaN/Inf。`write_number()` 还会显式输出 `NaN`。

风险：

- 生成不被常见 WKT 消费者接受的文本；
- 损坏输入没有保持 fail-closed；
- polygon 闭环比较对 NaN 的行为不直观。

### 修复要求

- 只允许 Point 的 X/Y 同时为 NaN 表示 Empty；
- 非 Empty 几何的全部 ordinate 必须 `std::isfinite()`；
- Empty Point 的附加 Z/M ordinate 应采用明确合同并测试；
- 增加 Point/LineString/Polygon/Multi* 的 NaN、+Inf、-Inf 拒绝测试。

---

## 第三轮：构建集成、测试覆盖与注释变更隔离

### 检查范围

- CMake 源文件归属；
- GDAL OFF/ON 的条件编译；
- geometry/full/package consumer 测试覆盖；
- 注释提交是否混入行为变化；
- 计划文档与代码现状是否一致。

### 已确认正确

- 四目录注释工作遵循“头文件说做什么、实现说为什么”的边界；
- 本轮抽查的注释提交未改变函数签名、分支或数据结构；
- package consumer 已在源码层引用新接口；
- WKB 测试覆盖大小端、Z/M/ZM、集合、Empty、截断、错误嵌套、错误计数和未闭环 polygon；
- GDAL OFF 路径对依赖 GDAL 的 Writer 操作返回明确 unavailable 错误。

### 集成缺口

- `wkb_reader.cpp` 不在最小 geometry core；
- `test_wkb_to_wkt.cpp` 不在 `GEOMETRY_TEST_SOURCES`，只在 full reader 测试 glob 中；
- 因 Actions 在 checkout 前失败，尚无编译、链接、CTest 或 sanitizer 证据验证上述风险是否已经触发。

---

## 修复优先级

1. **P0：FeatureCursor 生命周期所有权**；
2. **P0：to_wkt() 的 geometry core 链接归属和最小 consumer**；
3. **P1：非有限 ordinate fail-closed**；
4. 补齐对应单元测试、ASan 测试和最小链接测试；
5. runner 恢复后执行 GDAL OFF/ON、完整 CTest、package consumer、diff check。

## 自检查状态

- 第一轮：完成，发现 1 项高风险问题；
- 第二轮：完成，发现 1 项高风险和 1 项中风险问题；
- 第三轮：完成，确认构建/测试集成缺口；
- 最终状态：**三轮自检查完成，修复验收未完成**。
