# fast-gdb — FileGDB C++ Reader 与版本化 Writer Store

ESRI FileGDB 格式研究和 C++17 组件库。项目以 ISO WKB-first Reader、索引查询和不可变 generation 发布为核心。

当前正式版本：**v0.1.0**。`VersionedGdbStore` 属于 Unreleased 开发能力，已完成本地自检，跨平台正式验收尚未闭环。

## 产品形态

| 安装目标 | 用途 | GDAL 依赖 |
|---|---|---:|
| `fast_gdb::linear` | 纯 C++ Reader、几何与查询 | 无 |
| `fast_gdb::hybrid` | fast-gdb 主路径 + GDAL 复杂拓扑回退 | 有 |
| `fast_gdb::writer` | 不可变 generation、Reader 快照、单 Writer 和原子发布 | 无 |

`fast_gdb::writer` 只公开 VersionedGdbStore。旧 Writer、legacy target 和直接 source 替换接口已经从安装面删除，不提供兼容层。

## Reader 概览

读取链路：

```text
FileGDB geometry blob
    → GeometryModel（整数网格 XY + Z/M）
    → PolygonTopologyBuilder / built-in curve linearizer
    → ISO WKB（正式输出）
       WKT（按需调试输出）
       SpatialPredicate（精确空间判断）
```

主要能力：

- Point、MultiPoint、Polyline、Polygon 及 Z/M/ZM；
- multipart、洞、岛中岛和环方向无关拓扑；
- CircularArc、CubicBezier、EllipticArc 的内置折线化；
- `.spx`、`.atx`、WHERE、bbox 与组合查询；
- FID-only 查询和完整 `FeatureCursor`；
- ISO WKB-first，WKT 仅按需转换；
- 可选 GDAL Hybrid 回退。

MultiPatch 仍只提供 Hybrid degraded support，不承诺完整表面拓扑。

## VersionedGdbStore

### 解决的问题

Writer 不再修改 Reader 正在 mmap 的目录，也不再通过 `source → backup → source` 替换业务路径。每次发布创建一个不可变 generation，并通过 `CURRENT` 原子切换：

```text
<store-root>/
├── CURRENT
├── generations/
│   ├── gen-<id>.gdb/
│   └── gen-<id>.gdb/
└── work/
    └── work-gen-<id>.gdb/
```

语义：

- 已有 Reader 固定旧 generation；
- Writer 只修改私有 `working_path()`；
- 新 Reader 在发布后获取新 generation；
- 空闲 Reader 可显式 `refresh()`；
- 同一进程、同一仓库最多一个 Writer。

### 消费

```cmake
find_package(fast_gdb 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE fast_gdb::writer)
```

```cpp
#include <versioned_gdb_store.h>
#include <versioned_gdb_validator.h>

using namespace explorgdb::writer;

VersionedGdbStore store("/data/cities-store");
if (!store.open()) return false;

auto reader = store.acquire_reader();
if (!reader.valid()) return false;
// GdbCatalog、QueryEngine、cursor 和 mmap 全部从 reader.path() 打开。

auto write = store.begin_write();
if (!write.valid()) return false;

// 业务编辑器、GDAL 或其他完整 GDB 生成器只能修改 working_path()。
if (!business_editor_rewrites_gdb(write.working_path())) {
    write.abort();
    return false;
}

close_all_handles_for_working_gdb();
if (!write.publish(validator)) {
    if (write.published()) {
        // CURRENT 已切换，但最终持久化屏障失败。
        // 停止新 Writer，释放 Reader 后调用 store.recover()。
    } else {
        write.abort();
    }
}
```

完整用法见 [VersionedGdbStore 并发读写与版本发布](docs/usage/11_VersionedGdbStore并发读写与版本发布.md)。

### 发布状态

| 返回值 | 状态 | 含义 |
|---:|---|---|
| `true` | `PublishedDurable` | 新 generation 已发布并持久化 |
| `false` | `NotPublished` | CURRENT 未切换，可修复或 abort |
| `false` | `PublishedDurabilityUncertain` | CURRENT 已切换但最终目录同步失败，必须 recover |

不能只根据 `publish()` 的 bool 判断是否已经切换版本。

### 克隆和校验

working GDB 创建优先级：

- macOS `clonefile`；
- Linux `FICLONE`；
- Windows 或不支持 CoW 时完整复制。

发布前 validator 使用新的 Reader 对象重开候选，可检查目录 magic、系统目录、记录数、全表扫描、FID、WKB-first 几何、`.spx` 和 `.atx`。

## 构建

### 纯 C++

```bash
cmake -S . -B build-linear \
  -DFAST_GDB_WITH_GDAL=OFF \
  -DFAST_GDB_CURVE_BACKEND=BUILTIN \
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB \
  -DBUILD_TESTING=ON
cmake --build build-linear --parallel
ctest --test-dir build-linear --output-on-failure
```

### GDAL Hybrid

```bash
cmake -S . -B build-hybrid \
  -DFAST_GDB_WITH_GDAL=ON \
  -DFAST_GDB_CURVE_BACKEND=BUILTIN \
  -DFAST_GDB_GEOMETRY_OUTPUT=STANDARD_WKB \
  -DBUILD_TESTING=ON
cmake --build build-hybrid --parallel
ctest --test-dir build-hybrid --output-on-failure
```

CMake 使用 `find_package(GDAL)`，不绑定本机安装路径。

## WKB-first API

```cpp
explorgdb::GdbTableParser table(table_path);
table.open();
table.load_tablx(tablx_path);

explorgdb::GeometryValue geometry;
if (table.read_geometry_value(fid, geometry) && geometry.valid()) {
    consume(geometry.wkb);
    const auto debug_wkt = geometry.to_wkt();
}
```

## 项目目录

| 目录 | 内容 |
|---|---|
| `src/edgar/explorgdb/common` | 二进制与共享基础设施 |
| `src/edgar/explorgdb/reader` | Reader、索引、几何和查询 |
| `src/edgar/explorgdb/curve_gdal` | 可选 GDAL Hybrid Bridge |
| `src/edgar/explorgdb/writer` | VersionedGdbStore 私有实现及未导出的内部代码 |
| `include/fast_gdb/writer` | 唯一 Writer 公共 API |

## 关键文档

- [VersionedGdbStore 使用指南](docs/usage/11_VersionedGdbStore并发读写与版本发布.md)
- [ADR-007](docs/adr/ADR-007-versioned-gdb-store.md)
- [Writer 生命周期](docs/architecture/writer-lifecycle.md)
- [Writer Known Limitations](docs/architecture/writer-known-limitations.md)
- [Writer Roadmap](docs/roadmap/writer-roadmap.md)
- [三轮代码自检](docs/evidence/versioned-gdb-store-three-round-self-review-2026-07-21.md)
- [测试数据准备与跨平台验证](docs/usage/03_测试数据准备与跨平台验证.md)

## 明确边界

- 所有仓库访问必须经 VersionedGdbStore；
- 只支持同一进程多个 Reader + 单 Writer；
- 不提供旧 Writer API、legacy target 或兼容头；
- 不内建字段级 Append/Update/Delete 公共 API；
- 不支持 schema migration、原生曲线/MultiPatch 写入和 FID 空洞复用；
- 不支持跨进程锁、跨主机、分布式或跨 GDB 事务；
- S3、对象存储和不可靠网络文件系统不在范围内；
- 调用方必须为完整复制准备足够空间；
- 当前仍缺 macOS/Linux/Windows 正式矩阵、ENOSPC/崩溃故障注入和真实 FileGDB 发布证据。
