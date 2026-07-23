> **历史归档**：本文记录已废弃的 Writer 方案或阶段性证据，不代表当前产品能力。fast-gdb 当前仅提供 Reader；现行边界见 [ADR-007](../../../adr/ADR-007-reader-only-gdal-edit-boundary.md)，归档说明见 [Writer 历史索引](../README.md)。

# Writer 稳定 API 与旧接口迁移

本文对应 M18.2。新代码应链接 `fast_gdb::writer` 并包含 `<writer_session.h>`；旧实验接口仅通过 deprecated 的 `fast_gdb::writer_legacy` 目标保留。

## 1. 支持边界

稳定 Writer 仅接受调用方预先创建的 pristine empty schema：图层必须存在，且没有写入或删除历史。当前不支持 schema 创建、非空追加、Update、Delete、默认值自动应用、嵌套事务、原生曲线或 MultiPatch 写入。

## 2. CMake

### 稳定入口

```cmake
find_package(fast_gdb 0.1 CONFIG REQUIRED)
add_executable(example main.cpp)
target_link_libraries(example PRIVATE fast_gdb::writer)
```

无 GDAL 构建也可以消费该目标。schema 可由业务环境、ArcGIS 或单独的数据准备步骤创建。

### GDAL 索引助手

仅当 fast-gdb 以 `FAST_GDB_WITH_GDAL=ON` 构建时：

```cpp
#include <writer_index.h>

explorgdb::writer::CreateSpatialIndex(gdb_path, layer_name);
explorgdb::writer::CreateAttributeIndex(
    gdb_path, layer_name, "name", "name_idx");
```

### 旧实验目标

```cmake
find_package(fast_gdb 0.1 CONFIG REQUIRED)
target_link_libraries(old_consumer PRIVATE fast_gdb::writer_legacy)
```

该目标会产生 CMake deprecation 提示，只用于迁移期兼容，不应被新项目采用。

## 3. 稳定写入流程

```cpp
#include <writer_session.h>

using namespace explorgdb::writer;

WriterSession session;
if (!session.open("/tmp/cities.staging.gdb", "cities")) {
    log(session.error().message);
    return false;
}

if (!session.begin_row() ||
    !session.append_string(0, "chengdu") ||
    !session.set_point({104.0665, 30.5728, 0.0, 0.0}) ||
    !session.append_geometry(1) ||
    !session.end_row()) {
    log(session.error().message);
    session.abort();
    return false;
}

if (!session.commit("/data/cities.gdb")) {
    const WriterError& error = session.error();
    log(error.message);
    session.abort();
    return false;
}
```

`commit()` 要求 staging 与 final 位于同一父目录，且 final 不存在。目标已存在时不会覆盖。

## 4. 错误处理

不要解析 `message` 判断失败阶段。使用结构化字段：

```cpp
const WriterError& error = session.error();
switch (error.stage) {
    case WriterStage::Open:
    case WriterStage::Row:
    case WriterStage::Geometry:
    case WriterStage::Flush:
    case WriterStage::Close:
    case WriterStage::Publish:
    case WriterStage::Abort:
        break;
    case WriterStage::None:
        break;
}

log(error.layer);
log(error.path);
log(error.system_reason);
```

除 abort 文件清理失败外，当前会话错误均不允许在同一对象上重试。应 `abort()` 清理 staging，重新创建空 schema 和新的 `WriterSession`。

## 5. 几何迁移

旧接口需要直接操作 `GeometrySerializer`：

```cpp
GdbTableWriter writer;
writer.open_existing(staging, layer);
writer.geometry_serializer().set_point({104.0, 30.0});
writer.geometry_serializer().serialize(GeomType::Point);
writer.begin_row();
writer.append_geometry(geometry_field);
writer.end_row();
writer.close();
```

稳定接口不暴露内部 serializer：

```cpp
WriterSession session;
session.open(staging, layer);
session.begin_row();
session.set_point({104.0, 30.0, 0.0, 0.0},
                  WriterGeometryType::Point);
session.append_geometry(geometry_field);
session.end_row();
session.commit(final_path);
```

Polyline 和 Polygon 使用多 part/ring 坐标数组；Z/M/ZM 由 `WriterGeometryType` 决定，并从 `WriterCoordinate::z`、`WriterCoordinate::m` 读取。

## 6. 生命周期差异

| 行为 | 旧实验 API | 稳定 API |
|---|---|---|
| staging 所有权 | 调用方自行管理 | `open()` 成功后由会话管理 |
| 关闭验证 | 调用方直接 `close()` | `commit()` 内部执行 |
| 无覆盖发布 | 需组合 `AtomicGdbWriteSession` | 默认内置 |
| 未提交清理 | 调用方负责 | `abort()`/析构自动清理 |
| 错误信息 | 字符串 | 阶段、图层、路径、系统原因、重试性 |
| 内部头文件 | 直接可见 | 稳定目标不可见 |

## 7. 默认值

即使 schema 定义了默认值，也必须显式设置非 nullable 字段。M18.2 不将 schema 默认值解释或应用逻辑加入 Writer，会在 `end_row()` 缺字段时失败。
