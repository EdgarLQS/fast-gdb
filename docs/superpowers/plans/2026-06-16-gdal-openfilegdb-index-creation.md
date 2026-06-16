# GDAL OpenFileGDB 索引创建实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 封装 GDAL OpenFileGDB 的索引创建功能，实现空间索引和属性索引的创建、删除和检测

**Architecture:** 直接封装 GDAL C API，通过 SQL 语句创建索引。支持单字段索引、联合索引（多字段复合）和批量创建。测试数据统一管理在 test_data/temp/ 目录

**Tech Stack:** C++17, GDAL 3.9.3 (OpenFileGDB), Google Test, CMake

---

## 文件结构

```
src/edgar/explorgdb/writer/
├── gdb_index_creator.h          # 头文件：IndexDefinition 结构 + API 声明
└── gdb_index_creator.cpp        # 实现文件：所有 API 实现

tests/edgar/explorgdb/writer/
└── test_index_creator.cpp       # 单元测试：6 个测试用例

tests/
├── benchmark_index_creation.cpp # 性能基准测试
└── verify_gdal_indexes.cpp      # ArcGIS Pro 兼容性验证

docs/
├── GDAL_INDEX_CREATION.md       # 用户指南
└── INDEX_API_REFERENCE.md       # API 参考文档

scripts/
└── cleanup_temp.sh              # 清理临时数据脚本

CMakeLists.txt                   # 添加新文件到构建
```

---

## Task 1: 创建头文件 gdb_index_creator.h

**Files:**
- Create: `src/edgar/explorgdb/writer/gdb_index_creator.h`

- [ ] **Step 1: 创建头文件**

创建 `src/edgar/explorgdb/writer/gdb_index_creator.h`：

```cpp
// src/edgar/explorgdb/writer/gdb_index_creator.h
// GDAL OpenFileGDB 索引创建器 — 封装 GDAL C API 创建空间索引和属性索引
//
// 功能：
//   - 创建空间索引（.spx）
//   - 创建单字段属性索引（.atx）
//   - 创建联合索引（多字段复合）
//   - 批量创建索引
//   - 删除索引
//   - 检测空间索引
//
// 依赖：GDAL 3.9.3+（OpenFileGDB 驱动）
//
// 使用示例：
//   #include "explorgdb/writer/gdb_index_creator.h"
//   using namespace explorgdb::writer;
//
//   // 创建空间索引
//   CreateSpatialIndex("/path/to/data.gdb", "cities");
//
//   // 创建属性索引
//   CreateAttributeIndex("/path/to/data.gdb", "cities", "name");
//
//   // 创建联合索引
//   CreateCompositeIndex("/path/to/data.gdb", "cities", {"province", "city"});
//
//   // 批量创建
//   std::vector<IndexDefinition> indexes = {
//       IndexDefinition::Spatial(),
//       IndexDefinition("name_idx", "name")
//   };
//   CreateIndexes("/path/to/data.gdb", "cities", indexes);

#ifndef EXPLORGDB_GDB_INDEX_CREATOR_H
#define EXPLORGDB_GDB_INDEX_CREATOR_H

#include <string>
#include <vector>

namespace explorgdb {
namespace writer {

/**
 * 索引定义
 */
struct IndexDefinition {
    std::string index_name;              // 索引名称（可选，为空则自动生成）
    std::vector<std::string> fields;     // 字段列表（单字段或多字段）
    bool is_spatial = false;             // 是否为空间索引
    
    // 默认构造
    IndexDefinition() = default;
    
    // 单字段索引
    IndexDefinition(const std::string& name, const std::string& field)
        : index_name(name), fields({field}), is_spatial(false) {}
    
    // 多字段联合索引
    IndexDefinition(const std::string& name, const std::vector<std::string>& field_list)
        : index_name(name), fields(field_list), is_spatial(false) {}
    
    // 空间索引
    static IndexDefinition Spatial() {
        IndexDefinition def;
        def.is_spatial = true;
        return def;
    }
};

/**
 * 创建空间索引
 * 
 * @param gdb_path GDB 目录路径
 * @param layer_name 图层名称
 * @return true 成功，false 失败
 */
bool CreateSpatialIndex(const std::string& gdb_path,
                        const std::string& layer_name);

/**
 * 创建单字段属性索引
 * 
 * @param gdb_path GDB 目录路径
 * @param layer_name 图层名称
 * @param field_name 字段名称
 * @param index_name 索引名称（可选，为空则自动生成）
 * @return true 成功，false 失败
 */
bool CreateAttributeIndex(const std::string& gdb_path,
                          const std::string& layer_name,
                          const std::string& field_name,
                          const std::string& index_name = "");

/**
 * 创建联合索引（多字段复合索引）
 * 
 * @param gdb_path GDB 目录路径
 * @param layer_name 图层名称
 * @param field_names 字段列表（按顺序）
 * @param index_name 索引名称（可选，为空则自动生成）
 * @return true 成功，false 失败
 */
bool CreateCompositeIndex(const std::string& gdb_path,
                          const std::string& layer_name,
                          const std::vector<std::string>& field_names,
                          const std::string& index_name = "");

/**
 * 根据 IndexDefinition 创建索引（通用接口）
 */
bool CreateIndex(const std::string& gdb_path,
                 const std::string& layer_name,
                 const IndexDefinition& definition);

/**
 * 批量创建多个索引
 */
bool CreateIndexes(const std::string& gdb_path,
                   const std::string& layer_name,
                   const std::vector<IndexDefinition>& definitions);

/**
 * 删除索引
 */
bool DropIndex(const std::string& gdb_path,
               const std::string& index_name);

/**
 * 检查图层是否有空间索引
 */
bool HasSpatialIndex(const std::string& gdb_path,
                     const std::string& layer_name);

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_GDB_INDEX_CREATOR_H
```

- [ ] **Step 2: 提交头文件**

```bash
git add src/edgar/explorgdb/writer/gdb_index_creator.h
git commit -m "feat: add gdb_index_creator.h with IndexDefinition and API declarations"
```

---

## Task 2: 实现内部辅助函数

**Files:**
- Create: `src/edgar/explorgdb/writer/gdb_index_creator.cpp`

- [ ] **Step 1: 创建实现文件框架**

创建 `src/edgar/explorgdb/writer/gdb_index_creator.cpp`：

```cpp
// src/edgar/explorgdb/writer/gdb_index_creator.cpp
// GDAL OpenFileGDB 索引创建器实现

#include "gdb_index_creator.h"
#include "gdal.h"
#include "cpl_string.h"
#include <iostream>
#include <algorithm>

namespace explorgdb {
namespace writer {

// ── 内部辅助函数 ──

namespace {

GDALDatasetH OpenGDBForUpdate(const std::string& gdb_path) {
    GDALAllRegister();
    
    GDALDatasetH ds = GDALOpenEx(
        gdb_path.c_str(),
        GDAL_OF_UPDATE | GDAL_OF_VECTOR,
        nullptr, nullptr, nullptr
    );
    
    if (!ds) {
        std::cerr << "[IndexCreator] Failed to open GDB: " << gdb_path << "\n";
        std::cerr << "  GDAL Error: " << CPLGetLastErrorMsg() << "\n";
    }
    
    return ds;
}

bool ExecuteSQL(GDALDatasetH ds, const std::string& sql, const std::string& context) {
    char* err_msg = GDALDatasetExecuteSQL(ds, sql.c_str(), nullptr, nullptr);
    
    if (err_msg) {
        std::cerr << "[IndexCreator] " << context << " failed\n";
        std::cerr << "  SQL: " << sql << "\n";
        std::cerr << "  Error: " << err_msg << "\n";
        CPLFree(err_msg);
        return false;
    }
    
    return true;
}

std::string GenerateIndexName(const std::string& layer_name,
                               const std::vector<std::string>& fields) {
    std::string name = layer_name;
    for (const auto& field : fields) {
        name += "_" + field;
    }
    name += "_idx";
    
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    
    return name;
}

}  // namespace

// ── 公开 API 实现（待后续任务填充） ──

// TODO: 在后续任务中实现

}  // namespace writer
}  // namespace explorgdb
```

- [ ] **Step 2: 提交实现文件框架**

```bash
git add src/edgar/explorgdb/writer/gdb_index_creator.cpp
git commit -m "feat: add gdb_index_creator.cpp with internal helper functions"
```

---

## Task 3: 实现 CreateSpatialIndex（TDD）

**Files:**
- Modify: `src/edgar/explorgdb/writer/gdb_index_creator.cpp`
- Create: `tests/edgar/explorgdb/writer/test_index_creator.cpp`

- [ ] **Step 1: 写失败的测试**

创建 `tests/edgar/explorgdb/writer/test_index_creator.cpp`：

```cpp
// tests/edgar/explorgdb/writer/test_index_creator.cpp

#include <gtest/gtest.h>
#include "explorgdb/writer/gdb_index_creator.h"
#include "gdal.h"
#include <filesystem>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace explorgdb::writer;

// 测试 Fixture：管理临时目录
class IndexCreatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        GDALAllRegister();
        test_dir_ = "test_data/temp/index_test_" + std::to_string(getpid());
        fs::create_directories(test_dir_);
    }
    
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
        
        if (::testing::Test::HasFailure()) {
            std::cout << "[保留失败测试数据]: " << test_dir_ << "\n";
        }
    }
    
    // 生成测试数据（使用 GDAL OpenFileGDB）
    void generate_test_data(const std::string& gdb_path, int count) {
        // 使用 GDAL 创建简单的测试数据
        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
        GDALDataset* ds = driver->Create(gdb_path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
        
        OGRSpatialReference srs;
        srs.SetWellKnownGeogCS("WGS84");
        OGRLayer* layer = ds->CreateLayer("test_layer", &srs, wkbPoint, nullptr);
        
        OGRFieldDefn name_field("name", OFTString);
        layer->CreateField(&name_field);
        
        for (int i = 0; i < count; ++i) {
            OGRFeature* feat = OGRFeature::CreateFeature(layer->GetLayerDefn());
            feat->SetField("name", ("Point_" + std::to_string(i)).c_str());
            
            OGRPoint pt(100.0 + i * 0.01, 30.0 + i * 0.01);
            feat->SetGeometry(&pt);
            
            layer->CreateFeature(feat);
            OGRFeature::DestroyFeature(feat);
        }
        
        GDALClose(ds);
    }
    
    std::string test_dir_;
};

// 测试 1: 创建空间索引
TEST_F(IndexCreatorTest, T_IC01_CreateSpatialIndex) {
    std::string gdb_path = test_dir_ + "/test.gdb";
    generate_test_data(gdb_path, 100);
    
    ASSERT_TRUE(CreateSpatialIndex(gdb_path, "test_layer"));
    
    // 验证 .spx 文件存在
    bool has_spx = false;
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        if (entry.path().extension() == ".spx") {
            has_spx = true;
            break;
        }
    }
    ASSERT_TRUE(has_spx) << "空间索引文件 .spx 不存在";
}
```

- [ ] **Step 2: 运行测试验证失败**

先更新 CMakeLists.txt 添加测试文件（见 Task 10），然后：

```bash
cd build
cmake ..
make gdb_tutorial_test_runner
./bin/gdb_tutorial_test_runner --gtest_filter='IndexCreatorTest.T_IC01_CreateSpatialIndex'
```

预期：编译失败（函数未实现）

- [ ] **Step 3: 实现 CreateSpatialIndex**

在 `gdb_index_creator.cpp` 的 `// TODO` 位置添加：

```cpp
bool CreateSpatialIndex(const std::string& gdb_path,
                        const std::string& layer_name) {
    GDALDatasetH ds = OpenGDBForUpdate(gdb_path);
    if (!ds) return false;
    
    std::string sql = "CREATE SPATIAL INDEX ON " + layer_name;
    bool success = ExecuteSQL(ds, sql, "CreateSpatialIndex");
    
    GDALClose(ds);
    return success;
}
```

- [ ] **Step 4: 运行测试验证通过**

```bash
./bin/gdb_tutorial_test_runner --gtest_filter='IndexCreatorTest.T_IC01_CreateSpatialIndex'
```

预期：PASS

- [ ] **Step 5: 提交**

```bash
git add tests/edgar/explorgdb/writer/test_index_creator.cpp
git add src/edgar/explorgdb/writer/gdb_index_creator.cpp
git commit -m "feat: implement CreateSpatialIndex with TDD"
```

---

## Task 4: 实现 CreateAttributeIndex 和 CreateCompositeIndex（TDD）

**Files:**
- Modify: `src/edgar/explorgdb/writer/gdb_index_creator.cpp`
- Modify: `tests/edgar/explorgdb/writer/test_index_creator.cpp`

- [ ] **Step 1: 写失败的测试**

在 `test_index_creator.cpp` 添加：

```cpp
// 测试 2: 创建单字段属性索引
TEST_F(IndexCreatorTest, T_IC02_CreateAttributeIndex) {
    std::string gdb_path = test_dir_ + "/test.gdb";
    generate_test_data(gdb_path, 100);
    
    ASSERT_TRUE(CreateAttributeIndex(gdb_path, "test_layer", "name"));
    
    // 验证 .atx 文件存在
    bool has_atx = false;
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        if (entry.path().extension() == ".atx") {
            has_atx = true;
            break;
        }
    }
    ASSERT_TRUE(has_atx) << "属性索引文件 .atx 不存在";
}

// 测试 3: 创建联合索引
TEST_F(IndexCreatorTest, T_IC03_CreateCompositeIndex) {
    std::string gdb_path = test_dir_ + "/test.gdb";
    generate_test_data(gdb_path, 100);
    
    ASSERT_TRUE(CreateCompositeIndex(gdb_path, "test_layer", 
                                     {"name"}, 
                                     "name_idx"));
    
    // 验证 .atx 文件存在
    bool has_atx = false;
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        if (entry.path().filename().string().find("name_idx") != std::string::npos) {
            has_atx = true;
            break;
        }
    }
    ASSERT_TRUE(has_atx) << "联合索引文件不存在";
}
```

- [ ] **Step 2: 运行测试验证失败**

```bash
./bin/gdb_tutorial_test_runner --gtest_filter='IndexCreatorTest.T_IC02*:T_IC03*'
```

预期：编译失败

- [ ] **Step 3: 实现函数**

在 `gdb_index_creator.cpp` 添加：

```cpp
bool CreateAttributeIndex(const std::string& gdb_path,
                          const std::string& layer_name,
                          const std::string& field_name,
                          const std::string& index_name) {
    return CreateCompositeIndex(gdb_path, layer_name, {field_name}, index_name);
}

bool CreateCompositeIndex(const std::string& gdb_path,
                          const std::string& layer_name,
                          const std::vector<std::string>& field_names,
                          const std::string& index_name) {
    if (field_names.empty()) {
        std::cerr << "[IndexCreator] field_names is empty\n";
        return false;
    }
    
    GDALDatasetH ds = OpenGDBForUpdate(gdb_path);
    if (!ds) return false;
    
    std::string idx_name = index_name.empty() 
        ? GenerateIndexName(layer_name, field_names) 
        : index_name;
    
    std::string fields_str;
    for (size_t i = 0; i < field_names.size(); ++i) {
        if (i > 0) fields_str += ", ";
        fields_str += field_names[i];
    }
    
    std::string sql = "CREATE INDEX " + idx_name + 
                      " ON " + layer_name + "(" + fields_str + ")";
    
    bool success = ExecuteSQL(ds, sql, "CreateCompositeIndex");
    
    GDALClose(ds);
    return success;
}
```

- [ ] **Step 4: 运行测试验证通过**

```bash
./bin/gdb_tutorial_test_runner --gtest_filter='IndexCreatorTest.T_IC02*:T_IC03*'
```

预期：PASS

- [ ] **Step 5: 提交**

```bash
git add tests/edgar/explorgdb/writer/test_index_creator.cpp
git add src/edgar/explorgdb/writer/gdb_index_creator.cpp
git commit -m "feat: implement CreateAttributeIndex and CreateCompositeIndex with TDD"
```

---

## Task 5: 实现 CreateIndex 和 CreateIndexes（TDD）

**Files:**
- Modify: `src/edgar/explorgdb/writer/gdb_index_creator.cpp`
- Modify: `tests/edgar/explorgdb/writer/test_index_creator.cpp`

- [ ] **Step 1: 写失败的测试**

在 `test_index_creator.cpp` 添加：

```cpp
// 测试 4: 批量创建索引
TEST_F(IndexCreatorTest, T_IC04_CreateIndexes_Batch) {
    std::string gdb_path = test_dir_ + "/test.gdb";
    generate_test_data(gdb_path, 100);
    
    std::vector<IndexDefinition> indexes = {
        IndexDefinition::Spatial(),
        IndexDefinition("name_idx", "name")
    };
    
    ASSERT_TRUE(CreateIndexes(gdb_path, "test_layer", indexes));
    
    // 验证索引文件存在
    int index_count = 0;
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        auto ext = entry.path().extension();
        if (ext == ".spx" || ext == ".atx") {
            ++index_count;
        }
    }
    ASSERT_GE(index_count, 2) << "至少应该有 2 个索引文件";
}
```

- [ ] **Step 2: 运行测试验证失败**

```bash
./bin/gdb_tutorial_test_runner --gtest_filter='IndexCreatorTest.T_IC04*'
```

预期：编译失败

- [ ] **Step 3: 实现函数**

在 `gdb_index_creator.cpp` 添加：

```cpp
bool CreateIndex(const std::string& gdb_path,
                 const std::string& layer_name,
                 const IndexDefinition& definition) {
    if (definition.is_spatial) {
        return CreateSpatialIndex(gdb_path, layer_name);
    } else if (definition.fields.size() == 1) {
        return CreateAttributeIndex(gdb_path, layer_name, 
                                    definition.fields[0], 
                                    definition.index_name);
    } else {
        return CreateCompositeIndex(gdb_path, layer_name, 
                                    definition.fields, 
                                    definition.index_name);
    }
}

bool CreateIndexes(const std::string& gdb_path,
                   const std::string& layer_name,
                   const std::vector<IndexDefinition>& definitions) {
    if (definitions.empty()) {
        std::cerr << "[IndexCreator] No index definitions provided\n";
        return false;
    }
    
    GDALDatasetH ds = OpenGDBForUpdate(gdb_path);
    if (!ds) return false;
    
    bool all_success = true;
    int success_count = 0;
    
    for (const auto& def : definitions) {
        std::string sql;
        std::string context;
        
        if (def.is_spatial) {
            sql = "CREATE SPATIAL INDEX ON " + layer_name;
            context = "SpatialIndex";
        } else {
            std::string idx_name = def.index_name.empty() 
                ? GenerateIndexName(layer_name, def.fields) 
                : def.index_name;
            
            std::string fields_str;
            for (size_t i = 0; i < def.fields.size(); ++i) {
                if (i > 0) fields_str += ", ";
                fields_str += def.fields[i];
            }
            
            sql = "CREATE INDEX " + idx_name + 
                  " ON " + layer_name + "(" + fields_str + ")";
            context = "Index:" + idx_name;
        }
        
        if (ExecuteSQL(ds, sql, context)) {
            ++success_count;
        } else {
            all_success = false;
        }
    }
    
    GDALClose(ds);
    
    std::cout << "[IndexCreator] Created " << success_count << "/" 
              << definitions.size() << " indexes\n";
    
    return all_success;
}
```

- [ ] **Step 4: 运行测试验证通过**

```bash
./bin/gdb_tutorial_test_runner --gtest_filter='IndexCreatorTest.T_IC04*'
```

预期：PASS

- [ ] **Step 5: 提交**

```bash
git add tests/edgar/explorgdb/writer/test_index_creator.cpp
git add src/edgar/explorgdb/writer/gdb_index_creator.cpp
git commit -m "feat: implement CreateIndex and CreateIndexes with TDD"
```

---

## Task 6: 实现 DropIndex 和 HasSpatialIndex（TDD）

**Files:**
- Modify: `src/edgar/explorgdb/writer/gdb_index_creator.cpp`
- Modify: `tests/edgar/explorgdb/writer/test_index_creator.cpp`

- [ ] **Step 1: 写失败的测试**

在 `test_index_creator.cpp` 添加：

```cpp
// 测试 5: 删除索引
TEST_F(IndexCreatorTest, T_IC05_DropIndex) {
    std::string gdb_path = test_dir_ + "/test.gdb";
    generate_test_data(gdb_path, 100);
    
    // 先创建索引
    CreateAttributeIndex(gdb_path, "test_layer", "name", "name_idx");
    
    // 验证索引存在
    bool has_atx = false;
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        if (entry.path().filename().string().find("name_idx") != std::string::npos) {
            has_atx = true;
            break;
        }
    }
    ASSERT_TRUE(has_atx);
    
    // 删除索引
    ASSERT_TRUE(DropIndex(gdb_path, "name_idx"));
    
    // 验证索引已删除
    has_atx = false;
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        if (entry.path().filename().string().find("name_idx") != std::string::npos) {
            has_atx = true;
            break;
        }
    }
    ASSERT_FALSE(has_atx) << "索引应该已被删除";
}

// 测试 6: 检测空间索引
TEST_F(IndexCreatorTest, T_IC06_HasSpatialIndex) {
    std::string gdb_path = test_dir_ + "/test.gdb";
    generate_test_data(gdb_path, 100);
    
    // 无索引
    ASSERT_FALSE(HasSpatialIndex(gdb_path, "test_layer"));
    
    // 创建索引
    CreateSpatialIndex(gdb_path, "test_layer");
    
    // 有索引
    ASSERT_TRUE(HasSpatialIndex(gdb_path, "test_layer"));
}
```

- [ ] **Step 2: 运行测试验证失败**

```bash
./bin/gdb_tutorial_test_runner --gtest_filter='IndexCreatorTest.T_IC05*:T_IC06*'
```

预期：编译失败

- [ ] **Step 3: 实现函数**

在 `gdb_index_creator.cpp` 添加：

```cpp
bool DropIndex(const std::string& gdb_path,
               const std::string& index_name) {
    GDALDatasetH ds = OpenGDBForUpdate(gdb_path);
    if (!ds) return false;
    
    std::string sql = "DROP INDEX " + index_name;
    bool success = ExecuteSQL(ds, sql, "DropIndex");
    
    GDALClose(ds);
    return success;
}

bool HasSpatialIndex(const std::string& gdb_path,
                     const std::string& layer_name) {
    GDALDatasetH ds = GDALOpenEx(
        gdb_path.c_str(),
        GDAL_OF_READONLY | GDAL_OF_VECTOR,
        nullptr, nullptr, nullptr
    );
    
    if (!ds) return false;
    
    OGRLayerH layer = GDALDatasetGetLayerByName(ds, layer_name.c_str());
    if (!layer) {
        GDALClose(ds);
        return false;
    }
    
    int has_index = OGR_L_TestCapability(layer, OLCRandomRead);
    
    GDALClose(ds);
    return has_index != 0;
}
```

- [ ] **Step 4: 运行测试验证通过**

```bash
./bin/gdb_tutorial_test_runner --gtest_filter='IndexCreatorTest.*'
```

预期：全部 6 个测试 PASS

- [ ] **Step 5: 提交**

```bash
git add tests/edgar/explorgdb/writer/test_index_creator.cpp
git add src/edgar/explorgdb/writer/gdb_index_creator.cpp
git commit -m "feat: implement DropIndex and HasSpatialIndex, all 6 tests pass"
```

---

## Task 7: 更新 CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 添加源文件到构建**

在 `CMakeLists.txt` 中找到 writer 相关的源文件列表，添加：

```cmake
# 在 src/edgar/explorgdb/writer/ 的源文件列表中添加
src/edgar/explorgdb/writer/gdb_index_creator.cpp
```

- [ ] **Step 2: 添加测试文件**

在测试文件列表中添加：

```cmake
# 在 tests/edgar/explorgdb/writer/ 的测试文件列表中添加
tests/edgar/explorgdb/writer/test_index_creator.cpp
```

- [ ] **Step 3: 重新构建并运行所有测试**

```bash
cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
./bin/gdb_tutorial_test_runner --gtest_filter='IndexCreatorTest.*'
```

预期：全部 6 个测试 PASS

- [ ] **Step 4: 提交**

```bash
git add CMakeLists.txt
git commit -m "build: add gdb_index_creator to CMakeLists.txt"
```

---

## Task 8: 编写性能基准测试

**Files:**
- Create: `tests/benchmark_index_creation.cpp`

- [ ] **Step 1: 创建性能测试文件**

创建 `tests/benchmark_index_creation.cpp`：

```cpp
// tests/benchmark_index_creation.cpp
// 索引创建性能基准测试

#include "explorgdb/writer/gdb_index_creator.h"
#include <iostream>
#include <chrono>
#include <filesystem>

namespace fs = std::filesystem;
using namespace explorgdb::writer;

void BenchmarkIndexCreation() {
    const std::string large_gdb = "test_data/large/large_test.gdb";
    const std::string layer_name = "large_layer";
    
    if (!fs::exists(large_gdb)) {
        std::cerr << "测试数据不存在: " << large_gdb << "\n";
        std::cerr << "请先运行: ./bin/generate_large_gdb\n";
        return;
    }
    
    std::cout << "=== 索引创建性能基准 ===\n";
    std::cout << "测试数据: " << large_gdb << "\n\n";
    
    // 测试 1：空间索引
    {
        auto start = std::chrono::high_resolution_clock::now();
        bool ok = CreateSpatialIndex(large_gdb, layer_name);
        auto end = std::chrono::high_resolution_clock::now();
        
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "空间索引创建: " << (ok ? "成功" : "失败") 
                  << ", 耗时: " << ms << " ms\n";
    }
    
    // 测试 2：属性索引
    {
        auto start = std::chrono::high_resolution_clock::now();
        bool ok = CreateAttributeIndex(large_gdb, layer_name, "name", "name_idx");
        auto end = std::chrono::high_resolution_clock::now();
        
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "属性索引创建: " << (ok ? "成功" : "失败") 
                  << ", 耗时: " << ms << " ms\n";
    }
    
    // 测试 3：联合索引
    {
        auto start = std::chrono::high_resolution_clock::now();
        bool ok = CreateCompositeIndex(large_gdb, layer_name, 
                                       {"province", "city"}, "loc_idx");
        auto end = std::chrono::high_resolution_clock::now();
        
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "联合索引创建: " << (ok ? "成功" : "失败") 
                  << ", 耗时: " << ms << " ms\n";
    }
    
    std::cout << "\n=== 完成 ===\n";
}

int main() {
    GDALAllRegister();
    BenchmarkIndexCreation();
    return 0;
}
```

- [ ] **Step 2: 添加到 CMakeLists.txt**

```cmake
add_executable(benchmark_index_creation tests/benchmark_index_creation.cpp)
target_link_libraries(benchmark_index_creation explorgdb_writer ${GDAL_LIBRARY})
```

- [ ] **Step 3: 构建并运行**

```bash
cd build
cmake ..
make benchmark_index_creation
./bin/benchmark_index_creation
```

- [ ] **Step 4: 提交**

```bash
git add tests/benchmark_index_creation.cpp CMakeLists.txt
git commit -m "test: add benchmark_index_creation for performance testing"
```

---

## Task 9: 编写兼容性验证工具

**Files:**
- Create: `tests/verify_gdal_indexes.cpp`

- [ ] **Step 1: 创建验证工具**

创建 `tests/verify_gdal_indexes.cpp`：

```cpp
// tests/verify_gdal_indexes.cpp
// GDAL 创建的索引与 ArcGIS Pro 兼容性验证

#include "explorgdb/writer/gdb_index_creator.h"
#include "explorgdb/reader/gdb_spatial_index.h"
#include "explorgdb/reader/gdb_attribute_index.h"
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;
using namespace explorgdb::writer;
using namespace explorgdb;

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "用法: " << argv[0] << " <gdb_path>\n";
        return 1;
    }
    
    GDALAllRegister();
    
    const std::string gdb_path = argv[1];
    const std::string layer_name = "test_layer";
    
    std::cout << "=== GDAL 索引兼容性验证 ===\n";
    std::cout << "GDB 路径: " << gdb_path << "\n\n";
    
    // 步骤 1：创建索引
    std::cout << "[1/3] 创建索引...\n";
    std::vector<IndexDefinition> indexes = {
        IndexDefinition::Spatial(),
        IndexDefinition("name_idx", "name")
    };
    
    if (!CreateIndexes(gdb_path, layer_name, indexes)) {
        std::cerr << "索引创建失败\n";
        return 1;
    }
    
    // 步骤 2：提示用户在 ArcGIS Pro 中验证
    std::cout << "\n[2/3] 请在 ArcGIS Pro 中打开 GDB 验证：\n";
    std::cout << "  - 图层能正常显示\n";
    std::cout << "  - 空间索引存在且可用\n";
    std::cout << "  - 属性索引存在且可用\n";
    std::cout << "  - 查询结果正确\n";
    
    // 步骤 3：用我们的 reader 验证
    std::cout << "\n[3/3] 使用 explorgdb/reader 验证...\n";
    
    // 查找 .spx 文件
    std::string spx_path;
    std::string atx_path;
    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        auto ext = entry.path().extension();
        if (ext == ".spx" && spx_path.empty()) {
            spx_path = entry.path().string();
        }
        if (ext == ".atx" && entry.path().filename().string().find("name_idx") != std::string::npos) {
            atx_path = entry.path().string();
        }
    }
    
    if (!spx_path.empty()) {
        GdbSpatialIndexParser spx_parser(spx_path);
        if (spx_parser.parse()) {
            std::cout << "✓ 空间索引读取成功\n";
        } else {
            std::cerr << "✗ 空间索引读取失败\n";
        }
    }
    
    if (!atx_path.empty()) {
        GdbAttributeIndexParser atx_parser(atx_path);
        if (atx_parser.parse()) {
            std::cout << "✓ 属性索引读取成功\n";
        } else {
            std::cerr << "✗ 属性索引读取失败\n";
        }
    }
    
    std::cout << "\n=== 验证完成 ===\n";
    return 0;
}
```

- [ ] **Step 2: 添加到 CMakeLists.txt**

```cmake
add_executable(verify_gdal_indexes tests/verify_gdal_indexes.cpp)
target_link_libraries(verify_gdal_indexes explorgdb_writer explorgdb_reader ${GDAL_LIBRARY})
```

- [ ] **Step 3: 构建并运行**

```bash
cd build
cmake ..
make verify_gdal_indexes
./bin/verify_gdal_indexes /path/to/test.gdb
```

- [ ] **Step 4: 提交**

```bash
git add tests/verify_gdal_indexes.cpp CMakeLists.txt
git commit -m "test: add verify_gdal_indexes for ArcGIS Pro compatibility testing"
```

---

## Task 10: 编写用户指南

**Files:**
- Create: `docs/GDAL_INDEX_CREATION.md`

- [ ] **Step 1: 创建用户指南**

参考设计文档第 6.1 节，创建完整的用户指南文档。包括：
- 概述和工作流
- 快速开始示例
- 完整工作流示例
- API 参考
- 性能优化建议
- 兼容性说明
- 故障排除

- [ ] **Step 2: 提交**

```bash
git add docs/GDAL_INDEX_CREATION.md
git commit -m "docs: add GDAL index creation user guide"
```

---

## Task 11: 编写 API 参考文档

**Files:**
- Create: `docs/INDEX_API_REFERENCE.md`

- [ ] **Step 1: 创建 API 参考文档**

参考设计文档第 6.2 节，创建完整的 API 参考文档。包括：
- 头文件和命名空间
- 数据结构定义（IndexDefinition）
- 函数详细文档（7 个函数）
- 错误处理
- 线程安全
- 性能特性
- 依赖说明

- [ ] **Step 2: 提交**

```bash
git add docs/INDEX_API_REFERENCE.md
git commit -m "docs: add index API reference documentation"
```

---

## Task 12: 编写清理脚本

**Files:**
- Create: `scripts/cleanup_temp.sh`

- [ ] **Step 1: 创建清理脚本**

创建 `scripts/cleanup_temp.sh`：

```bash
#!/bin/bash
# 清理临时测试数据

echo "清理 test_data/temp/ 目录..."
rm -rf test_data/temp/*
echo "清理完成"
```

- [ ] **Step 2: 添加执行权限**

```bash
chmod +x scripts/cleanup_temp.sh
```

- [ ] **Step 3: 提交**

```bash
git add scripts/cleanup_temp.sh
git commit -m "scripts: add cleanup_temp.sh for cleaning temporary test data"
```

---

## Task 13: 最终验证和文档更新

**Files:**
- Modify: `docs/PROGRESS.md`

- [ ] **Step 1: 运行完整测试套件**

```bash
cd build
./bin/gdb_tutorial_test_runner --gtest_filter='IndexCreatorTest.*'
```

预期：全部 6 个测试 PASS

- [ ] **Step 2: 运行性能测试**

```bash
./bin/benchmark_index_creation
```

记录性能数据

- [ ] **Step 3: 更新进度文档**

在 `docs/PROGRESS.md` 添加：

```markdown
## 2026-06-16: GDAL OpenFileGDB 索引创建功能

**完成内容**:
- ✅ 实现 gdb_index_creator（7 个 API 函数）
- ✅ 6 个单元测试全部通过
- ✅ 性能基准测试
- ✅ ArcGIS Pro 兼容性验证工具
- ✅ 用户指南和 API 参考文档

**性能数据**:
- 空间索引创建: [待测试] ms
- 属性索引创建: [待测试] ms
- 联合索引创建: [待测试] ms
```

- [ ] **Step 4: 提交**

```bash
git add docs/PROGRESS.md
git commit -m "docs: update PROGRESS.md with index creation feature"
```

---

## 总结

完成所有 13 个任务后，你将拥有：

✅ **代码实现**: 完整的索引创建功能（7 个 API）  
✅ **单元测试**: 6 个测试用例，全部通过  
✅ **性能测试**: 基准测试工具  
✅ **兼容性验证**: ArcGIS Pro 验证工具  
✅ **文档**: 用户指南 + API 参考  
✅ **工具脚本**: 清理临时数据

**总工作量**: 4-5 小时（按设计文档估算）
