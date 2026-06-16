# GDAL 索引创建 API 参考

## IndexDefinition 结构体

```cpp
struct IndexDefinition {
    std::string index_name;              // 索引名称（可选，为空则自动生成）
    std::vector<std::string> fields;     // 字段列表（单字段或多字段）
    bool is_spatial = false;             // 是否为空间索引
};
```

### 构造方式

| 构造方式 | 说明 | 示例 |
|---------|------|------|
| `IndexDefinition()` | 默认构造（需手动设置字段） | — |
| `IndexDefinition(name, field)` | 单字段属性索引 | `IndexDefinition("name_idx", "name")` |
| `IndexDefinition(name, {f1, f2})` | 多字段复合索引 | `IndexDefinition("loc_idx", {"prov", "city"})` |
| `IndexDefinition::Spatial()` | 空间索引 | `IndexDefinition::Spatial()` |

---

## API 函数清单

### 1. CreateSpatialIndex

**功能**：为指定图层创建空间索引（生成 `.spx` 文件）。

```cpp
bool CreateSpatialIndex(const std::string& gdb_path,
                        const std::string& layer_name);
```

**参数**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `gdb_path` | `const std::string&` | GDB 目录路径（如 `/data/cities.gdb`） |
| `layer_name` | `const std::string&` | 图层名称（必须与 GDB 中实际图层名一致） |

**返回值**：

| 值 | 说明 |
|----|------|
| `true` | 索引创建成功或已存在 |
| `false` | 创建失败（GDB 无法打开或其他错误） |

**示例**：

```cpp
#include "explorgdb/writer/gdb_index_creator.h"
using namespace explorgdb::writer;

bool ok = CreateSpatialIndex("/data/buildings.gdb", "buildings");
if (ok) {
    std::cout << "Spatial index created successfully\n";
}
```

**注意事项**：
- OpenFileGDB 驱动可能在写入要素时自动创建空间索引
- SQL 执行失败但 `.spx` 文件存在时仍返回 `true`

---

### 2. CreateAttributeIndex

**功能**：为指定字段创建单字段属性索引（生成 `.atx` 文件）。

```cpp
bool CreateAttributeIndex(const std::string& gdb_path,
                          const std::string& layer_name,
                          const std::string& field_name,
                          const std::string& index_name = "");
```

**参数**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `gdb_path` | `const std::string&` | GDB 目录路径 |
| `layer_name` | `const std::string&` | 图层名称 |
| `field_name` | `const std::string&` | 字段名称（必须存在于图层中） |
| `index_name` | `const std::string&` | 索引名称（可选，为空则自动生成 `{layer}_{field}_idx`） |

**返回值**：

| 值 | 说明 |
|----|------|
| `true` | 索引创建成功或已存在 |
| `false` | 创建失败 |

**示例**：

```cpp
// 自动生成索引名称 "cities_name_idx"
CreateAttributeIndex("/data/cities.gdb", "cities", "name");

// 自定义索引名称
CreateAttributeIndex("/data/cities.gdb", "cities", "population", "pop_idx");
```

---

### 3. CreateCompositeIndex

**功能**：创建多字段联合索引（适用于多条件查询）。

```cpp
bool CreateCompositeIndex(const std::string& gdb_path,
                          const std::string& layer_name,
                          const std::vector<std::string>& field_names,
                          const std::string& index_name = "");
```

**参数**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `gdb_path` | `const std::string&` | GDB 目录路径 |
| `layer_name` | `const std::string&` | 图层名称 |
| `field_names` | `const std::vector<std::string>&` | 字段列表（按查询频率排序） |
| `index_name` | `const std::string&` | 索引名称（可选，为空则自动生成） |

**返回值**：

| 值 | 说明 |
|----|------|
| `true` | 索引创建成功或已存在 |
| `false` | 创建失败（如字段列表为空） |

**示例**：

```cpp
// 创建省市复合索引
CreateCompositeIndex("/data/cities.gdb", "cities",
                     {"province", "city"});

// 自定义索引名称
CreateCompositeIndex("/data/cities.gdb", "cities",
                     {"province", "city"}, "prov_city_idx");
```

**注意事项**：
- 字段顺序影响查询性能，将区分度高的字段放前面
- OpenFileGDB 对索引名称有长度限制（约 16 字符）

---

### 4. CreateIndex

**功能**：根据 `IndexDefinition` 类型自动 dispatch 到对应的创建函数。

```cpp
bool CreateIndex(const std::string& gdb_path,
                 const std::string& layer_name,
                 const IndexDefinition& definition);
```

**参数**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `gdb_path` | `const std::string&` | GDB 目录路径 |
| `layer_name` | `const std::string&` | 图层名称 |
| `definition` | `const IndexDefinition&` | 索引定义 |

**返回值**：

| 值 | 说明 |
|----|------|
| `true` | 索引创建成功 |
| `false` | 创建失败或定义无效 |

**示例**：

```cpp
// 空间索引
IndexDefinition spatial_def = IndexDefinition::Spatial();
CreateIndex("/data/cities.gdb", "cities", spatial_def);

// 单字段属性索引
IndexDefinition attr_def("name_idx", "name");
CreateIndex("/data/cities.gdb", "cities", attr_def);

// 多字段复合索引
IndexDefinition comp_def("loc_idx", {"province", "city"});
CreateIndex("/data/cities.gdb", "cities", comp_def);
```

---

### 5. CreateIndexes

**功能**：批量创建多个索引（内部复用数据集连接）。

```cpp
bool CreateIndexes(const std::string& gdb_path,
                   const std::string& layer_name,
                   const std::vector<IndexDefinition>& definitions);
```

**参数**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `gdb_path` | `const std::string&` | GDB 目录路径 |
| `layer_name` | `const std::string&` | 图层名称 |
| `definitions` | `const std::vector<IndexDefinition>&` | 索引定义列表 |

**返回值**：

| 值 | 说明 |
|----|------|
| `true` | 所有索引都创建成功 |
| `false` | 至少有一个索引创建失败 |

**示例**：

```cpp
std::vector<IndexDefinition> indexes = {
    IndexDefinition::Spatial(),                    // 空间索引
    IndexDefinition("name_idx", "name"),           // 名称索引
    IndexDefinition("time_idx", "timestamp"),      // 时间索引
    IndexDefinition("loc_idx", {"province", "city"}) // 复合索引
};

bool all_ok = CreateIndexes("/data/cities.gdb", "cities", indexes);
if (all_ok) {
    std::cout << "All indexes created successfully\n";
}
```

**注意事项**：
- 函数会在第一个失败的索引处停止，后续索引不再尝试
- 比逐个调用 `CreateIndex` 效率更高（只打开/关闭一次数据集）

---

### 6. DropIndex

**功能**：删除指定索引（通过 SQL `DROP INDEX`）。

```cpp
bool DropIndex(const std::string& gdb_path,
               const std::string& index_name);
```

**参数**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `gdb_path` | `const std::string&` | GDB 目录路径 |
| `index_name` | `const std::string&` | 要删除的索引名称 |

**返回值**：

| 值 | 说明 |
|----|------|
| `true` | 删除成功 |
| `false` | 删除失败（可能不支持该操作） |

**示例**：

```cpp
bool dropped = DropIndex("/data/cities.gdb", "cities_name_idx");
if (!dropped) {
    std::cerr << "Drop index may not be supported by OpenFileGDB\n";
}
```

**注意事项**：
- OpenFileGDB 驱动可能不完全支持 `DROP INDEX` 操作
- 即使 SQL 失败，也可手动删除 `.atx` 文件

---

### 7. HasSpatialIndex

**功能**：检测 GDB 是否包含空间索引（检查 `.spx` 文件存在性）。

```cpp
bool HasSpatialIndex(const std::string& gdb_path,
                     const std::string& layer_name);
```

**参数**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `gdb_path` | `const std::string&` | GDB 目录路径 |
| `layer_name` | `const std::string&` | 图层名称（用于兼容接口，实际未使用） |

**返回值**：

| 值 | 说明 |
|----|------|
| `true` | GDB 目录下存在 `.spx` 文件 |
| `false` | 不存在 `.spx` 文件或 GDB 路径无效 |

**示例**：

```cpp
if (HasSpatialIndex("/data/cities.gdb", "cities")) {
    std::cout << "Spatial index exists\n";
} else {
    std::cout << "No spatial index found\n";
}
```

**注意事项**：
- OpenFileGDB 创建一个 `.spx` 文件 per GDB，不是 per layer
- `layer_name` 参数保留用于未来扩展

---

## 完整工作流示例

```cpp
#include "explorgdb/writer/gdb_index_creator.h"
#include <iostream>
#include <filesystem>

using namespace explorgdb::writer;
namespace fs = std::filesystem;

int main() {
    std::string gdb_path = "/data/output.gdb";
    std::string layer = "cities";

    // Step 1: 创建空间索引
    std::cout << "Creating spatial index...\n";
    if (!CreateSpatialIndex(gdb_path, layer)) {
        std::cerr << "Warning: Spatial index creation may have failed\n";
    }

    // Step 2: 创建属性索引
    std::cout << "Creating attribute indexes...\n";
    CreateAttributeIndex(gdb_path, layer, "name", "name_idx");
    CreateAttributeIndex(gdb_path, layer, "population", "pop_idx");

    // Step 3: 创建复合索引
    std::cout << "Creating composite index...\n";
    CreateCompositeIndex(gdb_path, layer, {"province", "city"}, "loc_idx");

    // Step 4: 验证索引
    std::cout << "\nVerification:\n";
    std::cout << "  Has spatial index: "
              << (HasSpatialIndex(gdb_path, layer) ? "yes" : "no") << "\n";

    // 列出所有索引文件
    std::cout << "  Index files:\n";
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        if (entry.path().extension() == ".spx" ||
            entry.path().extension() == ".atx") {
            std::cout << "    - " << entry.path().filename().string() << "\n";
        }
    }

    return 0;
}
```

---

## 相关文档

- [`docs/GDAL_INDEX_CREATION.md`](GDAL_INDEX_CREATION.md) — 用户指南
- [`src/edgar/explorgdb/writer/gdb_index_creator.h`](../src/edgar/explorgdb/writer/gdb_index_creator.h) — 头文件源码
- [`src/edgar/explorgdb/writer/gdb_index_creator.cpp`](../src/edgar/explorgdb/writer/gdb_index_creator.cpp) — 实现源码
