# Writer 非空追加（M18.4.1）

非空追加通过 GDAL 构建下的 `WriterAppendSession` 提供。它不会改变 `WriterSession` 的空 schema 契约，也不开放 Update、Delete 或事务嵌套。

## 1. 能力边界

- 仅 `FAST_GDB_WITH_GDAL=ON` 的安装包包含 `writer_append.h`；
- 目标图层必须已经存在且至少包含一条记录；
- 只允许顺序追加，新 FID 必须严格大于打开时最大 FID；
- 不复用删除形成的 FID 空洞；
- 不修改原记录、原 FID、原属性或原几何；
- 单 Writer、业务层独占写入；commit 窗口内不支持并发 Reader；
- schema 创建、Update、Delete、崩溃自动恢复和嵌套事务不在本阶段。

## 2. 基本用法

```cpp
#include <writer_append.h>

using namespace explorgdb::writer;

WriterAppendSession append;
if (!append.open("cities.gdb", "cities")) {
    // append.error() 包含阶段、图层、路径和系统原因
    return false;
}

if (!append.begin_row() ||
    !append.set_string(0, "Chengdu") ||
    !append.set_f64(1, 21400000.0) ||
    !append.set_point({104.0665, 30.5728, 0.0, 0.0}) ||
    !append.end_row()) {
    append.abort();
    return false;
}

if (!append.commit()) {
    append.abort();
    return false;
}
```

字段索引使用 OGR 图层字段顺序，不包含 FID/ObjectID 和几何字段。几何通过独立 setter 提供。

## 3. 严格验证

公共 setter 不允许 GDAL 隐式类型转换：

- `set_i32` 仅接受 `OFTInteger`；
- `set_i64` 仅接受 `OFTInteger64`；
- `set_f64` 仅接受有限值 `OFTReal`；
- `set_string` 仅接受 `OFTString`；
- `set_binary` 仅接受 `OFTBinary`；
- `set_null` 仅接受 nullable 字段。

Point、Polyline 和 Polygon 必须与目标图层的几何 family、Z/M 维度一致。Polyline 每个 part 至少两个点；Polygon 环至少四点并显式闭合；所有使用的坐标必须有限。

`end_row()` 在 staging 中写入后立即按新 FID 回读，逐字段和完整几何比较。任何不一致都会锁定会话，只允许 `abort()` 或析构清理。

## 4. staging 和发布

`open()`：

1. 记录源 GDB 文件指纹；
2. 在同级目录创建唯一 `.append-staging-*` 副本；
3. 只在副本上追加。

`commit()`：

1. 关闭并重开 staging；
2. 验证总数量、全部原 FID、新 FID 和新增几何；
3. 再次确认源 GDB 指纹未变化；
4. 将源目录重命名为 `.append-backup-*`；
5. 将 staging 重命名为源路径；
6. 删除 backup；第二步发布失败时恢复 backup。

backup 删除失败会返回错误并保留 backup，但已发布源目录有效。调用方必须记录该错误并人工清理，不得把它当作未写入。

## 5. 错误后的状态

会话是 one-shot：首次 open、字段、几何、行写入、验证或发布错误会锁定对象。同一对象不得重试 `open()` 或 `commit()`；调用 `abort()` 后创建新会话。

未提交会话析构时删除 staging。源 GDB 在 staging、字段、几何、验证失败以及发布前源变更时保持不变。

## 6. 索引

第一版由 GDAL OpenFileGDB 在 staging 副本中维护现有 `.spx`/`.atx`。合同测试同时检查：

- 空间索引仍存在；
- 属性索引文件数量不减少；
- 属性过滤可以命中新记录；
- 空间过滤可以命中新记录。

本阶段不新增索引定义，也不自动为无索引图层创建索引。

## 7. 验收入口

- ADR：`docs/adr/ADR-002-non-empty-append.md`
- manifest：`tests/contracts/writer-append-macos-v1.json`
- workflow：`.github/workflows/writer-append-macos.yml`
- Google Test：`WriterAppendSessionTest.*`、`WriterAppendValidationTest.*`、`WriterAppendIndexTest.*`

只有 required 场景连续三次通过、安装消费成功并保留 schema-v2 JSON/CSV 后，M18.4.1 才能标记验收通过。当前 GitHub Actions 在任何 step 前失败的问题仍由 Issue #12 跟踪。