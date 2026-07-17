# 空间属性联合查询三轮自我代码审核

- 日期：2026-07-17
- 分支：`codex/spatial-attribute-query`
- 基线：`main@d8784e7`
- 状态：**三轮静态审核与问题修复完成，Formal acceptance blocked**
- 范围：仅审核和修改当前开发分支，不修改 `main`

## 审核方法

三轮审核采用不同关注点，避免只重复阅读同一执行链：

1. 编译、公开接口与安装面；
2. 查询正确性、索引损坏和回退完备性；
3. 测试并发、性能证据与可维护性。

严重级别：

- `P0`：可能产生错误结果、假空集、无限循环或无法构建；
- `P1`：可能造成不稳定测试、错误证据或跨平台问题；
- `P2`：可维护性、诊断质量或文档一致性问题。

## 第一轮：编译、接口与安装面

### 检查内容

- `SpatialWhere` 枚举分派和公开请求/结果结构；
- 新增 `.cpp` 是否进入 GDAL ON/OFF Reader 构建；
- GDAL 测试是否只进入 GDAL ON target；
- 公共头是否可独立包含；
- 内部 WHERE 头是否被安装；
- package consumer 是否只使用公开接口。

### 发现与修复

#### P1：`query_engine.h` 依赖传递包含

公共头直接使用 `size_t`，但未显式包含 `<cstddef>`。不同标准库、包含顺序或安装后 consumer 可能编译失败。

修复：

- 增加 `<cstddef>`；
- 保持 `SpatialWhere` 追加在枚举末尾；
- 未改变公开结构布局之外的语义。

提交：`161fbd5e20faca260bc01c2f44d96670985ba18c`

#### P1：`gdb_indexes.h` 依赖传递包含

公共 Reader 头直接使用 `size_t`、`uint8_t`，但没有直接声明对应标准头。

修复：增加 `<cstddef>` 和 `<cstdint>`。

提交：`8c13058f1fdfadf731acd1fdb5a732d757b4ca59`

### 第一轮结论

- Reader 源文件通过 `CONFIGURE_DEPENDS` glob 进入两种构建；
- `tests/usegdal/*.cpp` 仅在 `FAST_GDB_WITH_GDAL=ON` 时加入；
- `query_where_internal.h` 已从安装目录排除；
- package consumer 只包含 `<query_engine.h>` 并使用公开 `SpatialWhere` 契约；
- 本轮发现问题已修复。

## 第二轮：正确性、索引损坏与回退

### 检查内容

- `.spx + .atx` 候选交集是否可能漏结果；
- 合法零命中与索引解析失败是否可区分；
- NULL、NaN、函数索引、Unicode 和字符串截断；
- `.atx` trailer、页容量、页号和叶页链；
- 候选扫描失败是否回退到 canonical row read。

### 发现与修复

#### P0：不完整 `.atx` 可能被误判为合法零命中

旧解析器只验证 trailer magic。若 trailer 合法但页面越界、叶页链中断或实际条目数少于 `total_value_count`，`parse()` 仍可能返回成功。联合查询会把不完整候选集当作可信索引结果，产生假空集或漏 FID。

修复：

- 读取文件时校验 `streamoff`、`size_t` 和 `streamsize` 边界；
- 要求页面区长度严格为 4096 字节整数倍；
- 校验 `value_size`、tree depth 和物理页数；
- 叶页和非叶页条目数不得超过页容量；
- 实际解析条目数必须等于 trailer `total_value_count`；
- FID 0 视为结构损坏；
- 任一异常均清空条目并返回失败，使联合查询回退到空间候选完整 WHERE 求值。

提交：

- `df4632abf3227516816b966ef9282b13fcbb40a8`
- `73cf01978e7532b53c9c98513f14de5290aa378c`

#### P0：叶页自环可能无限循环

旧逻辑无访问页预算，`next_page_id` 指回当前页时会无限遍历。

修复：

- 按物理页数建立访问预算；
- 页号乘法使用 64 位中间值，防止 32 位 `size_t` 溢出；
- 页链耗尽预算或越界立即失败。

提交：`abc92c3cc1cc11844a010e5d50624c26b9d92474`

#### P1：损坏索引回归覆盖不足

修复：新增现场 FileGDB 测试：

- trailer 条目计数不一致；
- 叶页链自环；
- 两者均要求执行路径回退、`used_attribute_index=false`，且完整 FID 仍为 `{1,2}`。

提交：`448c90f30a5b21e4bc0923d8ea88bb3e7b6cf633`

另新增 GDAL OFF 纯 C++ 合成 `.atx` 门禁：

- 合法单叶页仍可解析；
- trailer count mismatch；
- cyclic leaf chain；
- zero FID；
- 三类损坏均 fail closed。

提交：`74a987bf68d0c577a4f8438a9753f640abde8200`

### 第二轮结论

索引状态现在明确分为：

- 结构完整且合法零命中：允许返回空候选；
- 文件缺失或任何结构损坏：禁止使用 `.atx`，回退到精确空间命中行上的完整 WHERE；
- 不会因损坏索引返回假空集，也不会因页链循环挂死。

## 第三轮：测试并发、性能证据与可维护性

### 检查内容

- `gtest_discover_tests` 下的 `ctest -j` 行为；
- 多个 TEST 是否共享固定临时 `.gdb`；
- benchmark JSON 是否记录观测值；
- 测试辅助代码是否依赖特定 GTest 版本；
- 安装包是否暴露内部实现；
- 分支差异是否包含数据、构建目录或 benchmark 输出。

### 发现与修复

#### P1：并行 CTest 会竞争同一个临时 GDB

集成、Unicode、索引故障文件内存在多个 TEST，但各 TEST 使用同一个固定临时目录。`gtest_discover_tests` 会将每个 TEST 注册为独立 CTest；在 `ctest -j` 下，测试可能同时删除和重建同一目录，导致随机失败或交叉污染。

修复：

- 新增 `spatial_where_test_utils.h`；
- 使用当前 test name 生成隔离路径；
- 每个测试文件使用独立前缀，路径组件做字符清理；
- 集成、Unicode、索引故障测试全部改为每 TEST 独立目录。

提交：

- `5e3d5994cbe070c254f8c1116aa1352d21466644`
- `c440b765cd69681b06030428e3111d65d7b643f0`
- `9cb5f7588ce389565d32d6338c42163d3d499831`
- `cf7908964d3d9e94811a782b33c63f7bfa0ae79b`

#### P1：测试辅助头依赖较新 GTest API

初版隔离辅助函数使用 `TestInfo::test_suite_name()`。项目会优先采用系统 GTest，旧版本可能缺少该接口，造成测试目标编译失败。

修复：

- 移除 suite-name API；
- 使用所有支持版本均存在的 `TestInfo::name()`；
- 依靠每个测试文件的独立前缀保持全局路径唯一。

提交：`1412a2775a3c376ecb1ad151573ae27709641907`

#### P1：benchmark 证据硬编码执行路径和正确性

JSON 原先固定写入 `spatial-where:spx+atx` 和 `correct: true`。即使测试内有断言，证据文件仍不应记录假定值。

修复：

- 保存每轮实际 `combined.execution_path`；
- 要求五次采样执行路径一致；
- 完整 FID 同时等于旧式求交和 GDAL 后才设置 `correct=true`；
- JSON 写入实际观测路径和 correctness。

提交：`e4b73193fb1aaf29310cd42268876321025735cd`

### 第三轮结论

- 新增测试不再因并行 CTest 共享目录而互相破坏；
- 测试辅助代码不再要求较新 GTest suite API；
- benchmark evidence 不再硬编码关键结论；
- 内部 WHERE 头仍未进入安装面；
- `main...HEAD` 差异未包含 fixture、构建目录或 benchmark 原始输出。

## 三轮最终问题清单

| 级别 | 数量 | 状态 |
|---|---:|---|
| P0 | 2 | 已修复并增加回归测试代码 |
| P1 | 6 | 已修复并增加契约/隔离/证据校验 |
| P2 | 0 | 未发现需要阻断本轮的新增问题 |

## 尚未形成的实际证据

本次审核是代码与差异审核。由于当前 GitHub Actions 在创建 step 前被环境阻断，且当前环境无法检出私有仓库，以下项目仍未实际运行：

- GDAL ON Release；
- GDAL OFF Release；
- 完整 CTest，包括新增合成索引安全测试；
- `ctest -j` 并行测试；
- package consumer；
- 100K benchmark；
- 10M benchmark 和 5% 回退门禁；
- `git diff --check main...HEAD`。

因此审核结论是：

```text
三轮自我代码审核完成
发现的问题均已在分支修复
代码验收仍为 Formal acceptance blocked
```
