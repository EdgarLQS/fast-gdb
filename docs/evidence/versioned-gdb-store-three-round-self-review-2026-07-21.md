# VersionedGdbStore 三轮代码自检查记录

- **日期**：2026-07-21
- **分支**：`agent/versioned-gdb-store`
- **PR**：#16
- **范围**：ADR-007 不可变 generation、Reader 快照租约、单 Writer 门禁、CoW/复制、CURRENT 发布、恢复和 QueryEngine 校验

## 结论

完成三轮独立视角复查，并将发现的问题直接修复到功能分支。复查后未保留已知的同进程并发可见性或切换前破坏 CURRENT 的缺陷；跨平台真实运行、故障注入和真实 FileGDB 验收仍按本文末尾的残余项执行。

## 第一轮：并发、对象生命周期与进程内门禁

### 检查重点

- Reader 租约增加、移动、refresh、释放和旧 generation 回收；
- 多个 `VersionedGdbStore` 实例是否共享 Writer 门禁；
- 路径别名是否可绕过门禁；
- Writer 析构、abort、发布成功和发布失败后的终态；
- 共享错误状态是否残留过期错误。

### 发现与修复

1. **Windows 路径大小写可形成两个注册键**
   - 风险：同一仓库可能获得两个进程内状态并同时开始两个 Writer。
   - 修复：注册键基于原生路径，并在 Windows 对 ASCII 路径字符进行大小写折叠；保留 `weakly_canonical` 处理符号链接别名。

2. **Windows 文件持久化句柄权限不正确**
   - 风险：以只读权限打开文件后调用 `FlushFileBuffers` 可能失败。
   - 修复：Windows 文件刷新使用 `GENERIC_WRITE`，并保持共享读、写、删除标志。

3. **发布不确定状态无法由调用方区分**
   - 风险：`publish()==false` 既可能表示未发布，也可能表示 CURRENT 已切换但最终目录同步失败。
   - 修复：新增 `GdbPublishState`、`publish_state()` 和 `published()`；不确定状态为终态，不允许同事务重试。

4. **成功操作可能保留旧 `last_error`**
   - 修复：成功 acquire、begin_write、refresh 和 abort 后清除过期错误；后台清理失败仍保留可诊断错误。

### 新增回归测试

- 多实例单 Writer；
- 符号链接仓库别名不能绕过门禁；
- Windows 大小写别名不能绕过门禁；
- 发布状态和错误清理；
- Writer 析构自动 abort 并释放门禁。

## 第二轮：崩溃一致性、持久化与文件系统边界

### 检查重点

- 候选同步、generation 提升、CURRENT 临时文件、原子替换和目录同步的顺序；
- 每个失败点能否保持 CURRENT 可恢复；
- 空间不足与清理失败是否会破坏当前版本；
- generation 是否真正不可变；
- 恢复是否 fail closed。

### 发现与修复

1. **CURRENT 已替换但根目录同步失败时可能过早删除旧 generation**
   - 风险：崩溃后文件系统若恢复旧 CURRENT，清单可能指向已经删除的旧 generation。
   - 修复：引入 `current_durability_uncertain`。该状态下保留全部旧代次、Reader 释放不执行回收、阻止新 Writer，并要求无活动租约时显式 `recover()` 完成目录持久化屏障后再清理。

2. **managed GDB 中的符号链接破坏不可变性**
   - 风险：generation 内文件可指向外部可变对象；相对路径计算还可能逃逸 working 根目录。
   - 修复：clone 和同步阶段拒绝符号链接及特殊文件；使用词法相对路径并拒绝 `..`；working 目录不得位于 source GDB 内部。

3. **CoW/复制的 working 文件可能继承只读权限**
   - 风险：Writer 无法修改克隆出的文件。
   - 修复：working 常规文件显式增加 owner read/write 权限。

4. **清理错误被忽略**
   - 风险：残留候选、旧 generation 或临时清单无法进入诊断，空间和恢复状态不可判断。
   - 修复：所有关键删除和枚举错误均返回或记录；回滚失败附加到原始错误中。

5. **CURRENT 解析不够严格**
   - 修复：要求单行、唯一换行、有限长度、合法 generation 名称且目标目录存在；损坏清单 fail closed。

### 新增回归测试

- managed source 拒绝符号链接；
- working 文件获得 owner write 权限；
- 多行 CURRENT fail closed；
- stale work、临时清单和未发布 generation 恢复清理。

## 第三轮：API、Validator、跨平台构建与测试覆盖

### 检查重点

- 稳定公开头和安装面；
- Validator 是否验证结构而非仅验证存在性；
- `validate_sample_geometry=false` 是否仍产生不必要几何解码；
- GDAL 开关是否影响核心托管发布测试；
- 是否存在真实并发测试而非仅顺序模拟。

### 发现与修复

1. **关闭几何校验时仍走几何读取路径**
   - 修复：普通 FID 样本使用 `QueryEngine::read_by_fid()`；仅在明确要求时调用 WKB-first 几何读取。

2. **索引只检查非空文件，未验证 B+ 树结构**
   - 修复：要求的 `.spx` 使用 `GdbSpatialIndexParser::parse()`；要求的 `.atx` 同时检查元数据并使用 `GdbAttributeIndexParser::parse()` 完整解析。

3. **核心测试只在无 GDAL 构建中形成独立目标**
   - 修复：无论 GDAL 是否启用，始终创建 `fast_gdb_versioned_store_test_runner`，并显式链接 `Threads::Threads`。

4. **内部实现头可能进入 legacy 安装面**
   - 修复：`versioned_gdb_store_internal.h` 明确从 legacy 头文件安装中排除。

5. **缺少持续读取线程覆盖**
   - 修复：新增真实 `std::thread` 测试，在 Writer 发布期间循环读取旧 generation，同时验证新 Reader 获取新版。

## 本地验证

以下检查已在 Linux 环境完成：

- C++17 严格语法检查：`-Wall -Wextra -Wpedantic -fsyntax-only`；
- VersionedGdbStore 初始化、发布、旧/新 Reader、refresh 和回收 smoke；
- 多线程旧 Reader 连续可见性 smoke；
- 符号链接路径门禁 smoke；
- AddressSanitizer + UndefinedBehaviorSanitizer smoke；
- QueryEngine Validator 使用接口桩的严格语法检查；
- 新增 GTest 文件使用测试宏桩的语法检查。

上述本地检查均通过。

## 仍需平台与集成验收

以下内容不能由当前本地环境替代：

- 仓库完整 CMake 构建与 CTest；
- macOS/APFS `clonefile` 成功路径和强制复制回退；
- Linux reflink 与非 reflink 文件系统矩阵；
- Windows 编译、完整复制、大小写路径和 `MoveFileExW` 运行验证；
- ENOSPC、删除失败、目录 fsync 失败和原子替换边界的系统级故障注入；
- 真实 FileGDB 的记录数、FID、几何、`.spx` 和 `.atx` 验收。

PR 创建后 GitHub Actions 作业曾在任何 step 执行前终止且未产生可读取日志，因此不能将该基础设施结果解释为代码编译或测试失败；待 Actions 执行层恢复后必须重新运行完整矩阵。
