# VersionedGdbStore latest-only API 三轮自检查

- **日期**：2026-07-22
- **分支**：`agent/versioned-gdb-store`
- **目标**：不保留任何旧 Writer 公共接口、兼容 target、兼容头或迁移期
- **结论**：公共 API 已收敛；正式构建和跨平台运行证据仍受 CI 执行环境阻塞

## 1. 目标状态

安装后的 Writer 产品必须只有：

- CMake target：`fast_gdb::writer`；
- Header：`versioned_gdb_store.h`；
- Header：`versioned_gdb_validator.h`；
- Archive symbols：仅 `versioned_gdb_*` 实现。

以下内容必须不存在于安装面：

- `fast_gdb::writer_legacy`；
- `FAST_GDB_INSTALL_LEGACY_WRITER_API`；
- `writer_session.h`；
- `writer_recovery.h`；
- `writer_index.h`；
- `writer_append.h`；
- `writer_update.h`；
- `writer_delete.h`；
- `writer_transaction.h`；
- `gdb_table_writer.h`；
- 旧 API package consumer；
- 旧 API workflow 和使用指南。

## 2. 第一轮：构建、链接和符号边界

### 检查项

- 安装 target 是否仍指向包含全部旧 Writer 源文件的 `explorgdb_writer_lib`；
- 旧实现符号是否仍进入安装 archive；
- 公共 include path 是否仍暴露 `src/edgar/explorgdb/writer/`；
- VersionedGdbStore 实现是否能够独立链接 Reader 和 common；
- 测试是否链接真正安装的 Writer 实现；
- 删除源码目录重复公共头后，内部完整测试目标是否仍可找到新版头。

### 发现

1. 原安装目标仍复用 `explorgdb_writer_lib`。即使旧头不安装，静态 archive 仍包含旧 Writer 符号，不满足“旧接口全部删除”。
2. 删除 `src/.../writer/versioned_gdb_store.h` 和 validator 重复头后，内部 `explorgdb_writer_lib` 仍会通过 GLOB 编译 `versioned_gdb_*.cpp`，但缺少 `include/fast_gdb/writer`，完整构建会找不到新版公共头。

### 修复

- 新建 `fast_gdb_versioned_writer_lib`；
- 源文件严格限定为 `versioned_gdb_*.cpp`；
- public include 只指向 `include/fast_gdb/writer/`；
- Reader 和源码 Writer 目录均为 PRIVATE；
- 导出名设为 `writer`；
- `explorgdb_writer_lib` 降为不安装的内部实现/历史测试目标；
- 内部目标仅以 PRIVATE 方式增加 `include/fast_gdb/writer`，避免完整测试构建回归，同时不形成传递或安装接口；
- versioned store test runner 改为链接 `fast_gdb_versioned_writer_lib`。

### 结果

安装 archive、头文件和传递 include path 均不再包含旧 Writer 公共接口；内部历史测试目标仍可编译新版源文件，但不会被安装或导出。

## 3. 第二轮：安装面与负向兼容检查

### 检查项

- legacy CMake option/target 是否仍存在；
- GDAL ON 是否条件安装旧 Append/Update/Delete/Transaction 头；
- package consumer 是否仍包含 writer_legacy 或旧 Writer 宏；
- 安装后是否有旧头残留；
- CMake export 是否出现 `writer_legacy`。

### 发现

初版负向 workflow 使用 `grep -R writer_legacy <install-root>`。由于 CHANGELOG 必须记录删除项，该检查会错误匹配文档并导致假失败。

### 修复

- 删除 legacy option、target 和安装目录；
- 删除全部旧 Writer 头的 install 规则；
- package consumer 只保留 `linear`、`hybrid`、`writer`；
- writer consumer 只包含两个 versioned 头；
- workflow 显式断言旧头不存在；
- `writer_legacy` 检查仅扫描安装后的 CMake export 目录，不扫描 CHANGELOG。

### 结果

安装面的正向和负向契约均由单一 workflow 明确验证。

## 4. 第三轮：文档、ADR、CI 和范围一致性

### 检查项

- README 和当前状态文档是否仍给出旧 Writer 示例；
- ADR 索引是否仍把 ADR-001～005 标为当前决策；
- lifecycle/limitations/roadmap 是否仍描述 `source → backup → source`；
- 旧 workflow 是否仍会触发已删除 consumer 选项；
- 当前功能矩阵是否把 fast-gdb Writer 描述为字段级编辑器。

### 发现

- 旧 ADR、M18 计划、使用指南和 macOS workflow 仍构成可发现入口；
- GDAL 对比矩阵仍将 Writer 描述为空 schema/直接发布组件；
- PR 原描述仍是“新增发布层”，没有声明 breaking replacement。

### 修复

删除：

- ADR-001～ADR-005；
- 旧 Writer transaction design；
- 旧稳定 API/Append/CI/性能/本地验收指南；
- 旧 M18 Writer 计划和进度；
- 六个旧 macOS Writer workflow。

新增或重写：

- ADR-007：唯一公共 Writer 决策；
- VersionedGdbStore 使用指南；
- Writer lifecycle、limitations、roadmap；
- README、项目总览、规划状态、未完成清单；
- GDAL 功能对比矩阵；
- Changelog breaking changes；
- `versioned-gdb-store.yml` 三平台 workflow；
- PR 标题和描述改为明确的 breaking replacement。

### 结果

当前入口只指向 VersionedGdbStore。历史内部代码不构成安装、API、ABI、文档或 CI 兼容承诺。

## 5. 最新公共边界

### 提供

- immutable generation；
- Reader snapshot lease；
- single Writer gate；
- macOS clonefile / Linux FICLONE / full-copy fallback；
- reopen validator；
- atomic CURRENT；
- durable/not-published/uncertain 状态；
- recovery 和 generation garbage collection。

### 不提供

- 旧 Writer API 或兼容层；
- 字段级 Append/Update/Delete 公共 API；
- schema migration；
- 原生曲线/MultiPatch 写入；
- FID 空洞复用；
- 跨进程锁/租约；
- 多 Writer；
- savepoint、嵌套、跨 GDB 或分布式事务；
- S3、对象存储或不可靠网络文件系统。

## 6. 尚未取得的运行证据

本轮完成的是源码、构建定义、安装定义、consumer、workflow 和文档自检。仍必须通过实际环境取得：

- 完整 CMake configure/build；
- CTest；
- Windows/Linux/macOS package install；
- 导出符号和旧头负向检查；
- installed consumer；
- CoW/full-copy 实际矩阵；
- ENOSPC/crash fault injection；
- 真实 FileGDB validator。

GitHub Actions 当前仍在执行任何 step 前结束，因此不能把 workflow 的 failure 解释为代码编译失败，也不能把本轮静态自检解释为正式验收通过。
