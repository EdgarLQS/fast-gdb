# Adaptive Reader 独立端到端示例

## 范围

`examples/adaptive_reader` 是独立 CMake package consumer，依赖已安装的
`fast_gdb::adaptive`。它在唯一临时目录中由官方 OpenFileGDB driver 创建一条
Point 记录，依次验证：

1. 稳定状态由 FastGdb 以 `Verified` 读取初始值 `7`；
2. WriterActive 期间默认读取返回 `SourceBusy`，且不选择任何读取后端；
3. 调用方使用官方 GDAL 修改值为 `42`，关闭 update Dataset 并通知协调器；
4. 代次递增后重新加载绑定并构造新 session，FastGdb 以 `Verified` 读到 `42`。

示例退出时删除临时 GDB。fast-gdb API 和 Adaptive target 都没有写入职责；
示例中的写入仅展示调用方直接管理官方 GDAL update 生命周期。

## 构建环境与命令

- macOS arm64、AppleClang 21.0.0。
- GDAL/OpenFileGDB 3.13.3。
- 安装包由当前工作树以 `FAST_GDB_BUILD_ADAPTIVE_READER=ON` 构建。

```sh
cmake -S . -B /private/tmp/fast-gdb-gdal-3.13.3-build \
  -DFAST_GDB_BUILD_ADAPTIVE_READER=ON
cmake --build /private/tmp/fast-gdb-gdal-3.13.3-build \
  --target fast_gdb_adaptive -j4
cmake --install /private/tmp/fast-gdb-gdal-3.13.3-build \
  --prefix /private/tmp/fast-gdb-adaptive-example-install

cmake -S examples/adaptive_reader \
  -B /private/tmp/fast-gdb-adaptive-example-build \
  -DCMAKE_PREFIX_PATH=/private/tmp/fast-gdb-adaptive-example-install \
  -DGDAL_INCLUDE_DIR=/private/tmp/fast-gdb-gdal-3.13.3/include \
  -DGDAL_LIBRARY=/private/tmp/fast-gdb-gdal-3.13.3/lib/libgdal.dylib
cmake --build /private/tmp/fast-gdb-adaptive-example-build -j4
DYLD_LIBRARY_PATH=/private/tmp/fast-gdb-gdal-3.13.3/lib \
  ctest --test-dir /private/tmp/fast-gdb-adaptive-example-build \
  --output-on-failure
```

结果：Adaptive library 和安装包构建成功；示例构建成功；CTest **1/1**
通过。直接运行输出：

```text
before: value=7, backend=FastGdb, consistency=Verified
during update: status=SourceBusy, backend=None
after: value=42, generation=1, backend=FastGdb, consistency=Verified
```

The package's Adaptive CTest group was also rebuilt and run under the same GDAL
runtime path: **40 passed, 1 skipped, 0 failed**. The skipped case is the
pre-existing `OpenFileGDB DeleteIndex` capability gap and is not counted as a
pass.

```sh
DYLD_LIBRARY_PATH=/private/tmp/fast-gdb-gdal-3.13.3/lib \
  ctest --test-dir /private/tmp/fast-gdb-gdal-3.13.3-build \
  -R adaptive --output-on-failure -j4
```

这是单机成功路径证据，不涵盖 Windows DLL 生命周期、Linux/macOS 多进程压力、
异常断电恢复或未协调外部 Writer 的强一致性；这些范围仍按主计划验收。
