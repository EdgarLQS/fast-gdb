# fast-gdb 合并前检查单

## 已完成的代码收敛

- [x] `gdb_table.cpp` 已删除历史 `peek_geometry_blob` 实现。
- [x] CMake 已删除 `EXPLORGDB_RENAME_LEGACY_PEEK` 符号重命名兼容层。
- [x] `read_record_by_fid`、全量记录读取和 `sequential_scan` 通过 `fixed_physical_width()` 共享固定字段物理宽度语义。
- [x] 公开 `peek_geometry_blob` 通过 `skip_field_value()` 跳过非几何字段。
- [x] `DateTimeWithOffset` 在全部读取路径均消费 10 字节。
- [x] `QueryEngine` 测试覆盖生成 GDB 后的 `open/read_by_fid/scan/query_bbox` 主路径。
- [x] 无 `.atx` 时返回空结果，且 capability 状态和 reason 明确。
- [x] 临时 GitHub Actions 工作流已删除，不纳入最终 PR。

## 合并前仍需本地执行

- [ ] `cmake -S . -B build`
- [ ] `cmake --build build --target gdb_tutorial_test_runner`
- [ ] 新增专项测试通过。
- [ ] 计划 smoke 通过。
- [ ] 元数据/系统表测试通过。
- [ ] `git status --short --branch` 仅包含计划内改动。
- [ ] 同步当前 `main` 后再次构建和执行专项测试。

## 非阻塞记录

- 既有小 `.spx` fixture 问题。
- 缺失 `test_spatial_gdb.gdb` 测试数据路径。
- 仓库外层本地日志不纳入本分支。

## 范围外

- GDAL 运行时 fallback。
- 写入生产化。
- MultiPatch 标准 WKT。
- 完整 GDB_Items XML 生产级解析。
- 坐标重投影。
