# GDAL 写入同一 FileGDB 时 fast-gdb Reader 可见性观测

- **日期**：2026-07-22
- **性质**：边界测试与观测性证据
- **正式产品结论**：fast-gdb Reader only；FileGDB 编辑交给 GDAL/OpenFileGDB
- **测试源码**：`tests/usegdal/test_gdal_write_fast_reader_visibility.cpp`
- **测试目标**：`fast_gdb_gdal_read_write_boundary_test_runner`
- **CTest 前缀**：`gdal-reader-boundary.`

## 1. 问题

当 GDAL 正在修改一个普通 `.gdb` 目录，而 fast-gdb Reader 已经打开同一个目录时，Reader 会看到：

- 旧数据；
- 新数据；
- 混合数据；
- 错误。

这个问题不能只凭设计推断，需要真实 OpenFileGDB 观测；但单次观测也不能被误写成并发支持合同。

## 2. 正式支持结论

唯一正式支持的时序是：

```text
关闭全部 fast-gdb Reader
    → GDAL 独占写入
    → GDALClose
    → 重新创建 fast-gdb Reader
```

重开的 Reader 必须读取新数据。

以下时序明确不支持：

```text
fast-gdb Reader 保持打开
    + GDAL update 同一个 .gdb
```

并发期间 old/new/mixed/error 均可能出现。

## 3. 测试数据

真实 OpenFileGDB driver 创建：

```text
boundary.gdb
└── reader_boundary
    ├── value: Integer
    ├── phase: String
    └── Shape: Point
```

写前值：

```text
value = 1
phase = old
```

GDAL 更新后：

```text
value = 2
phase = new
```

同时更新两个字段是为了识别混合状态：

- `1/old` → old；
- `2/new` → new；
- 其他组合 → mixed；
- 打开或读取失败 → error。

## 4. 正式门禁测试

测试名：

```text
SupportedQuiescedReaderWorkflowReopensWithNewData
```

步骤：

1. fast-gdb 打开并读取 `1/old`；
2. Reader 离开作用域，释放 parser、mmap 和句柄；
3. GDAL 以 `GDAL_OF_UPDATE` 打开同一 GDB；
4. `SetFeature(2/new)`；
5. `SyncToDisk()`、`FlushCache()`、`GDALClose()`；
6. fast-gdb 创建全新 Reader；
7. 必须读取 `2/new`。

该测试失败表示受支持的阶段式集成不成立，属于发布阻断问题。

## 5. 同目录重叠观测测试

测试名：

```text
SameDirectoryReadWhileGdalWriterIsOpenIsCharacterizationOnly
```

步骤：

1. fast-gdb Reader 打开并保持存活；
2. 独立线程通过 GDAL 修改同一 GDB；
3. GDAL 已 `SetFeature`、`SyncToDisk` 和 `FlushCache`，但 Dataset 保持打开；
4. 已有 fast-gdb Reader 再次读取；
5. 新 fast-gdb Reader 尝试打开同一目录；
6. 允许 GDALClose；
7. 已有 Reader 再次读取；
8. 输出三个阶段的 old/new/mixed/error 分类；
9. 销毁旧 Reader；
10. 全新 Reader 必须读取 `2/new`。

测试不会对步骤 4、5、7 的类别作固定断言。原因不是测试宽松，而是这些状态本身不属于产品支持范围。

## 6. 结果解释规则

### 观察到 old

只能说明当前平台、当前 GDAL 版本、当前操作和当前文件布局中，已有 Reader 保留了旧缓存或旧映射。不能推导并发读取安全。

### 观察到 new

只能说明当前读路径重新读取了某些已更新内容。不能证明表、tablx、spx、atx 和系统表始终一致。

### 观察到 mixed

直接证明同目录并发存在跨文件或跨缓存版本不一致风险。

### 观察到 error

证明文件锁、句柄、解析或格式一致性可能在重叠阶段失败。

## 7. 不允许得出的结论

禁止从测试输出得出：

- “GDAL 写时 fast-gdb 总能读旧数据”；
- “GDAL 写时 fast-gdb 总能读新数据”；
- “简单 SetFeature 安全，所以 DeleteFeature/REPACK 也安全”；
- “Linux 可用，所以 Windows/macOS 也可用”；
- “GDALClose 后旧 Reader 可以继续复用”；
- “没有出现 mixed 就等于不存在 mixed 风险”。

## 8. 后续测试矩阵

正式兼容性测试应继续覆盖“停读→写→重开”流程：

| GDAL 操作 | 重开后 fast-gdb 验证 |
|---|---|
| CreateFeature | 新 FID、字段、几何、记录数 |
| SetFeature | 字段值、几何、索引查询 |
| DeleteFeature | 删除槽、顺序扫描、FID 查询 |
| CreateField/DeleteField | Schema 和记录布局 |
| CreateIndex/DeleteIndex | `.gdbindexes/.atx` 和回退 |
| 空间索引重建 | `.spx` 候选和精确复核 |
| REPACK | tablx、物理 offset、FID 语义 |
| extent 重算 | 图层范围 |

同目录并发压力测试只作为风险观测，不作为支持门禁。

## 9. CI

工作流：

```text
.github/workflows/gdal-write-reader-boundary.yml
```

核心命令：

```bash
cmake -S . -B build-boundary \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_BUILD_FULL_TESTS=OFF \
  -DFAST_GDB_BUILD_TOOLS=OFF \
  -DBUILD_TESTING=ON
cmake --build build-boundary \
  --target fast_gdb_gdal_read_write_boundary_test_runner --parallel
ctest --test-dir build-boundary --output-on-failure \
  -R '^gdal-reader-boundary\.'
```

## 10. 当前证据边界

本文件定义测试合同和结果解释方式。只有实际 workflow step、日志和 artifact 可用后，才能记录具体平台观测。无日志的 runner 失败既不能证明测试通过，也不能证明代码失败。
