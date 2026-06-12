# GDB 组件库 Phase 1A 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 GDB 组件库 Phase 1A 只读薄封装——GdbDatasource/GdbDatasets/GdbDataset/GdbRecordset，完成打开/关闭、枚举图层、读字段、读几何、顺序遍历、错误信息，并通过 5 个测试。

**Architecture:** 薄封装 GDAL 原生指针。GdbDatasource 唯一拥有 GDALDataset*，所有子对象为视图（持有 GdbErrorContext*）。API 风格参考 SuperMap iObjects Datasource→Datasets→Dataset→Recordset 三层模式。

**Tech Stack:** C++17, GDAL 3.9.3, Google Test, CMake

---

## 文件清单

| 操作 | 文件路径 | 职责 |
|------|---------|------|
| 创建 | `fast_gdb/src/error_context.h` | 共享错误上下文（纯头文件） |
| 创建 | `fast_gdb/src/connection_info.h` | GdbConnectionInfo 声明 |
| 创建 | `fast_gdb/src/connection_info.cpp` | GdbConnectionInfo 实现 |
| 创建 | `fast_gdb/src/datasource.h` | GdbDatasource 声明 |
| 创建 | `fast_gdb/src/datasource.cpp` | GdbDatasource 实现 |
| 创建 | `fast_gdb/src/datasets.h` | GdbDatasets + GdbDataset 声明（放在一起避免前向引用） |
| 创建 | `fast_gdb/src/datasets.cpp` | GdbDatasets 实现 |
| 创建 | `fast_gdb/src/dataset.cpp` | GdbDataset 实现 |
| 创建 | `fast_gdb/src/recordset.h` | GdbRecordset 声明 |
| 创建 | `fast_gdb/src/recordset.cpp` | GdbRecordset 实现 |
| 创建 | `fast_gdb/tests/test_011_component_api.cpp` | Phase 1A 测试（5 个用例） |
| 修改 | `fast_gdb/CMakeLists.txt` | 新增组件库 target 和测试链接 |

---

### Task 1: 创建 src/ 目录和 error_context.h

**Files:**
- Create: `fast_gdb/src/error_context.h`

- [ ] **Step 1: 创建目录和文件**

```bash
mkdir -p "fast_gdb/src"
```

- [ ] **Step 2: 编写 error_context.h**

```cpp
// src/error_context.h — 纯头文件错误上下文

#ifndef GDB_ERROR_CONTEXT_H
#define GDB_ERROR_CONTEXT_H

#include <string>

class GdbErrorContext {
public:
    virtual ~GdbErrorContext() = default;

    void setError(const std::string& msg) { m_lastError = msg; }
    std::string getLastError() const { return m_lastError; }
    void clearError() { m_lastError.clear(); }

protected:
    std::string m_lastError;
};

#endif // GDB_ERROR_CONTEXT_H
```

- [ ] **Step 3: 验证文件创建成功**

```bash
ls "fast_gdb/src/error_context.h"
```

---

### Task 2: 编写 connection_info.h 和 connection_info.cpp

**Files:**
- Create: `fast_gdb/src/connection_info.h`
- Create: `fast_gdb/src/connection_info.cpp`

- [ ] **Step 1: 编写 connection_info.h**

```cpp
// src/connection_info.h

#ifndef GDB_CONNECTION_INFO_H
#define GDB_CONNECTION_INFO_H

#include <string>
#include <map>

class GdbConnectionInfo {
public:
    void setServer(const std::string& path);
    std::string getServer() const;

    void setAlias(const std::string& alias);
    std::string getAlias() const;

    void setReadOnly(bool ro);
    bool isReadOnly() const;

    void setOpenOption(const std::string& key, const std::string& value);
    const std::map<std::string, std::string>& getOpenOptions() const;

    // 转换为 GDALOpenEx 所需的 char**，调用方需用 freeOpenOptions 释放
    char** toOpenOptions() const;
    void freeOpenOptions(char** papsz) const;

private:
    std::string m_server;
    std::string m_alias;
    bool m_readOnly = false;
    std::map<std::string, std::string> m_openOptions;
};

#endif // GDB_CONNECTION_INFO_H
```

- [ ] **Step 2: 编写 connection_info.cpp**

```cpp
// src/connection_info.cpp

#include "connection_info.h"
#include "cpl_string.h"

void GdbConnectionInfo::setServer(const std::string& path) { m_server = path; }
std::string GdbConnectionInfo::getServer() const { return m_server; }

void GdbConnectionInfo::setAlias(const std::string& alias) { m_alias = alias; }
std::string GdbConnectionInfo::getAlias() const { return m_alias; }

void GdbConnectionInfo::setReadOnly(bool ro) { m_readOnly = ro; }
bool GdbConnectionInfo::isReadOnly() const { return m_readOnly; }

void GdbConnectionInfo::setOpenOption(const std::string& key, const std::string& value) {
    m_openOptions[key] = value;
}
const std::map<std::string, std::string>& GdbConnectionInfo::getOpenOptions() const {
    return m_openOptions;
}

char** GdbConnectionInfo::toOpenOptions() const {
    char** papsz = nullptr;
    for (const auto& [key, value] : m_openOptions) {
        papsz = CSLSetNameValue(papsz, key.c_str(), value.c_str());
    }
    return papsz;
}

void GdbConnectionInfo::freeOpenOptions(char** papsz) const {
    CSLDestroy(papsz);
}
```

- [ ] **Step 3: 验证文件创建成功**

```bash
ls "fast_gdb/src/connection_info.h" "fast_gdb/src/connection_info.cpp"
```

---

### Task 3: 编写 datasource.h

**Files:**
- Create: `fast_gdb/src/datasource.h`

- [ ] **Step 1: 编写 datasource.h**

> 注意：需要前向声明 GdbDatasets，因为循环依赖。GdbDatasets/GdbDataset 声明放在 datasets.h 中。

```cpp
// src/datasource.h

#ifndef GDB_DATASOURCE_H
#define GDB_DATASOURCE_H

#include "error_context.h"
#include "connection_info.h"
#include "gdal_priv.h"

class GdbDatasets;
class GdbDataset;
class GdbRecordset;

class GdbDatasource : public GdbErrorContext {
public:
    GdbDatasource();
    ~GdbDatasource();

    // 不可拷贝、不可移动
    GdbDatasource(const GdbDatasource&) = delete;
    GdbDatasource& operator=(const GdbDatasource&) = delete;
    GdbDatasource(GdbDatasource&&) = delete;
    GdbDatasource& operator=(GdbDatasource&&) = delete;

    // 打开/关闭
    bool open(const GdbConnectionInfo& info);
    bool openExisting(const std::string& path);
    void close();
    bool isOpen() const;

    // 数据集访问 — 每次调用返回临时视图
    GdbDatasets getDatasets() const;

    // 信息
    std::string getAlias() const;
    std::string getServer() const;
    int getDatasetCount() const;

    // Phase 1B：事务能力检测
    bool supportsTransactions() const;
    bool supportsEmulatedTransactions() const;

    // Phase 1B：事务状态
    bool isInTransaction() const;

    // 底层访问
    GDALDataset* getNative() const;

private:
    friend class GdbDatasets;
    friend class GdbDataset;
    friend class GdbRecordset;

    GDALDataset* m_ds = nullptr;
    std::string m_alias;
    std::string m_server;
    bool m_inTransaction = false;
};

#endif // GDB_DATASOURCE_H
```

---

### Task 4: 编写 datasource.cpp

**Files:**
- Create: `fast_gdb/src/datasource.cpp`

- [ ] **Step 1: 编写 datasource.cpp**

```cpp
// src/datasource.cpp

#include "datasource.h"
#include "datasets.h"

GdbDatasource::GdbDatasource() = default;

GdbDatasource::~GdbDatasource() {
    close();
}

bool GdbDatasource::open(const GdbConnectionInfo& info) {
    close();

    m_alias = info.getAlias();
    m_server = info.getServer();

    unsigned int flags = GDAL_OF_VECTOR;
    if (info.isReadOnly()) {
        flags |= GDAL_OF_READONLY;
    } else {
        flags |= GDAL_OF_UPDATE;
    }

    // RAII 包装 char**
    char** papszOptions = info.toOpenOptions();
    auto optionsGuard = [&info, papszOptions]() { info.freeOpenOptions(papszOptions); };

    m_ds = (GDALDataset*)GDALOpenEx(m_server.c_str(), flags, nullptr, papszOptions, nullptr);

    info.freeOpenOptions(papszOptions);

    if (!m_ds) {
        setError("Failed to open: " + m_server);
        return false;
    }

    clearError();
    return true;
}

bool GdbDatasource::openExisting(const std::string& path) {
    GdbConnectionInfo info;
    info.setServer(path);
    info.setReadOnly(true);
    return open(info);
}

void GdbDatasource::close() {
    if (m_ds) {
        GDALClose(m_ds);
        m_ds = nullptr;
    }
    m_inTransaction = false;
    clearError();
}

bool GdbDatasource::isOpen() const { return m_ds != nullptr; }

GdbDatasets GdbDatasource::getDatasets() const {
    return GdbDatasets(m_ds, const_cast<GdbDatasource*>(this));
}

std::string GdbDatasource::getAlias() const { return m_alias; }
std::string GdbDatasource::getServer() const { return m_server; }

int GdbDatasource::getDatasetCount() const {
    return m_ds ? m_ds->GetLayerCount() : 0;
}

bool GdbDatasource::supportsTransactions() const {
    return m_ds && m_ds->TestCapability(ODsCTransactions);
}

bool GdbDatasource::supportsEmulatedTransactions() const {
    return m_ds && m_ds->TestCapability(ODsCEmulatedTransactions);
}

bool GdbDatasource::isInTransaction() const { return m_inTransaction; }

GDALDataset* GdbDatasource::getNative() const { return m_ds; }
```

---

### Task 5: 编写 datasets.h（GdbDatasets + GdbDataset 声明）

**Files:**
- Create: `fast_gdb/src/datasets.h`

- [ ] **Step 1: 编写 datasets.h**

```cpp
// src/datasets.h — GdbDatasets 和 GdbDataset 声明

#ifndef GDB_DATASETS_H
#define GDB_DATASETS_H

#include "error_context.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include <string>

class GdbRecordset;

class GdbDataset {
public:
    GdbDataset();

    std::string getName() const;
    GDALwkbGeometryType getGeometryType() const;
    int getFeatureCount();
    int getFieldCount() const;
    std::string getFieldName(int index) const;
    OGRFieldType getFieldType(int index) const;

    // 记录集
    GdbRecordset getRecordset() const;
    GdbRecordset getRecordsetFiltered(const std::string& attributeFilter) const;

    // 字段信息
    std::string getFieldTypeName(OGRFieldType type) const;

    // 底层访问
    OGRLayer* getNative() const;
    bool isValid() const;

    std::string getLastError() const { return m_errorCtx ? m_errorCtx->getLastError() : ""; }

private:
    friend class GdbDatasets;
    friend class GdbRecordset;
    GdbDataset(OGRLayer* layer, GdbErrorContext* errCtx);
    OGRLayer* m_layer = nullptr;
    GdbErrorContext* m_errorCtx = nullptr;
};

class GdbDatasets {
public:
    GdbDatasets();

    int getCount() const;
    GdbDataset get(int index) const;
    GdbDataset get(const std::string& name) const;

    std::string getLastError() const { return m_errorCtx ? m_errorCtx->getLastError() : ""; }

private:
    friend class GdbDatasource;
    GdbDatasets(GDALDataset* ds, GdbErrorContext* errCtx);
    GDALDataset* m_ds = nullptr;
    GdbErrorContext* m_errorCtx = nullptr;
};

#endif // GDB_DATASETS_H
```

---

### Task 6: 编写 datasets.cpp

**Files:**
- Create: `fast_gdb/src/datasets.cpp`

- [ ] **Step 1: 编写 datasets.cpp**

```cpp
// src/datasets.cpp

#include "datasets.h"
#include "recordset.h"

// ========== GdbDataset ==========

GdbDataset::GdbDataset() = default;

GdbDataset::GdbDataset(OGRLayer* layer, GdbErrorContext* errCtx)
    : m_layer(layer), m_errorCtx(errCtx) {}

std::string GdbDataset::getName() const {
    return m_layer ? m_layer->GetName() : "";
}

GDALwkbGeometryType GdbDataset::getGeometryType() const {
    return m_layer ? m_layer->GetGeomType() : wkbUnknown;
}

int GdbDataset::getFeatureCount() {
    return m_layer ? m_layer->GetFeatureCount() : -1;
}

int GdbDataset::getFieldCount() const {
    return m_layer ? m_layer->GetLayerDefn()->GetFieldCount() : 0;
}

std::string GdbDataset::getFieldName(int index) const {
    if (!m_layer || index < 0 || index >= getFieldCount()) return "";
    return m_layer->GetLayerDefn()->GetFieldDefn(index)->GetNameRef();
}

OGRFieldType GdbDataset::getFieldType(int index) const {
    if (!m_layer || index < 0 || index >= getFieldCount()) return OFTInteger;
    return m_layer->GetLayerDefn()->GetFieldDefn(index)->GetType();
}

GdbRecordset GdbDataset::getRecordset() const {
    if (!m_layer) return GdbRecordset(nullptr, m_errorCtx);
    return GdbRecordset(m_layer, m_errorCtx);
}

GdbRecordset GdbDataset::getRecordsetFiltered(const std::string& attributeFilter) const {
    GdbRecordset rs = getRecordset();
    if (rs.m_layer) {
        rs.m_layer->SetAttributeFilter(attributeFilter.c_str());
    }
    return rs;
}

std::string GdbDataset::getFieldTypeName(OGRFieldType type) const {
    switch (type) {
        case OFTInteger:      return "Integer";
        case OFTInteger64:    return "Integer64";
        case OFTReal:         return "Real";
        case OFTString:       return "String";
        case OFTBinary:       return "Binary";
        case OFTDate:         return "Date";
        case OFTTime:         return "Time";
        case OFTDateTime:     return "DateTime";
        default:              return "Unknown";
    }
}

OGRLayer* GdbDataset::getNative() const { return m_layer; }
bool GdbDataset::isValid() const { return m_layer != nullptr; }

// ========== GdbDatasets ==========

GdbDatasets::GdbDatasets() = default;

GdbDatasets::GdbDatasets(GDALDataset* ds, GdbErrorContext* errCtx)
    : m_ds(ds), m_errorCtx(errCtx) {}

int GdbDatasets::getCount() const {
    return m_ds ? m_ds->GetLayerCount() : 0;
}

GdbDataset GdbDatasets::get(int index) const {
    if (!m_ds || index < 0 || index >= m_ds->GetLayerCount()) {
        return GdbDataset();
    }
    return GdbDataset(m_ds->GetLayer(index), m_errorCtx);
}

GdbDataset GdbDatasets::get(const std::string& name) const {
    if (!m_ds) return GdbDataset();
    OGRLayer* layer = m_ds->GetLayerByName(name.c_str());
    return GdbDataset(layer, m_errorCtx);
}
```

---

### Task 7: 编写 recordset.h

**Files:**
- Create: `fast_gdb/src/recordset.h`

- [ ] **Step 1: 编写 recordset.h**

```cpp
// src/recordset.h

#ifndef GDB_RECORDSET_H
#define GDB_RECORDSET_H

#include "error_context.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include <memory>
#include <string>

class GdbRecordset {
public:
    GdbRecordset();
    ~GdbRecordset();

    // 不可拷贝
    GdbRecordset(const GdbRecordset&) = delete;
    GdbRecordset& operator=(const GdbRecordset&) = delete;

    // 可移动
    GdbRecordset(GdbRecordset&& other) noexcept;
    GdbRecordset& operator=(GdbRecordset&& other) noexcept;

    // 顺序游标
    bool moveFirst();
    bool moveNext();
    bool isEOF() const;
    bool isValid() const;

    // 当前要素
    int64_t getFid() const;

    // 字段访问
    int getFieldCount() const;
    std::string getFieldName(int index) const;
    int getFieldIndex(const std::string& name) const;

    // 类型化读取
    int32_t getFieldAsInteger(const std::string& name) const;
    int32_t getFieldAsInteger(int index) const;
    int64_t getFieldAsInteger64(const std::string& name) const;
    int64_t getFieldAsInteger64(int index) const;
    double getFieldAsDouble(const std::string& name) const;
    double getFieldAsDouble(int index) const;
    std::string getFieldAsString(const std::string& name) const;
    std::string getFieldAsString(int index) const;

    // 几何访问 — const 指针，调用方不应销毁
    const OGRGeometry* getGeometry() const;
    std::unique_ptr<OGRGeometry> cloneGeometry() const;

    std::string getLastError() const { return m_errorCtx ? m_errorCtx->getLastError() : ""; }

    void close();

private:
    friend class GdbDataset;
    GdbRecordset(OGRLayer* layer, GdbErrorContext* errCtx);
    OGRLayer* m_layer = nullptr;
    OGRFeature* m_currentFeature = nullptr;
    GdbErrorContext* m_errorCtx = nullptr;
    bool m_eof = false;
};

#endif // GDB_RECORDSET_H
```

---

### Task 8: 编写 recordset.cpp

**Files:**
- Create: `fast_gdb/src/recordset.cpp`

- [ ] **Step 1: 编写 recordset.cpp**

```cpp
// src/recordset.cpp

#include "recordset.h"

GdbRecordset::GdbRecordset() = default;

GdbRecordset::GdbRecordset(OGRLayer* layer, GdbErrorContext* errCtx)
    : m_layer(layer), m_errorCtx(errCtx) {}

GdbRecordset::~GdbRecordset() { close(); }

GdbRecordset::GdbRecordset(GdbRecordset&& other) noexcept
    : m_layer(other.m_layer),
      m_currentFeature(other.m_currentFeature),
      m_errorCtx(other.m_errorCtx),
      m_eof(other.m_eof) {
    other.m_layer = nullptr;
    other.m_currentFeature = nullptr;
    other.m_errorCtx = nullptr;
    other.m_eof = false;
}

GdbRecordset& GdbRecordset::operator=(GdbRecordset&& other) noexcept {
    if (this != &other) {
        close();
        m_layer = other.m_layer;
        m_currentFeature = other.m_currentFeature;
        m_errorCtx = other.m_errorCtx;
        m_eof = other.m_eof;
        other.m_layer = nullptr;
        other.m_currentFeature = nullptr;
        other.m_errorCtx = nullptr;
        other.m_eof = false;
    }
    return *this;
}

bool GdbRecordset::moveFirst() {
    if (!m_layer) {
        m_eof = true;
        return false;
    }
    close();
    m_layer->ResetReading();
    return moveNext();
}

bool GdbRecordset::moveNext() {
    if (!m_layer) {
        m_eof = true;
        return false;
    }
    close();
    m_currentFeature = m_layer->GetNextFeature();
    m_eof = (m_currentFeature == nullptr);
    return !m_eof;
}

bool GdbRecordset::isEOF() const { return m_eof; }

bool GdbRecordset::isValid() const { return m_layer != nullptr; }

int64_t GdbRecordset::getFid() const {
    return m_currentFeature ? m_currentFeature->GetFID() : -1;
}

int GdbRecordset::getFieldCount() const {
    return m_layer ? m_layer->GetLayerDefn()->GetFieldCount() : 0;
}

std::string GdbRecordset::getFieldName(int index) const {
    if (!m_layer || index < 0 || index >= getFieldCount()) return "";
    return m_layer->GetLayerDefn()->GetFieldDefn(index)->GetNameRef();
}

int GdbRecordset::getFieldIndex(const std::string& name) const {
    if (!m_layer) return -1;
    return m_layer->GetLayerDefn()->GetFieldIndex(name.c_str());
}

int32_t GdbRecordset::getFieldAsInteger(const std::string& name) const {
    return getFieldAsInteger(getFieldIndex(name));
}

int32_t GdbRecordset::getFieldAsInteger(int index) const {
    if (!m_currentFeature || index < 0) return 0;
    return m_currentFeature->GetFieldAsInteger(index);
}

int64_t GdbRecordset::getFieldAsInteger64(const std::string& name) const {
    return getFieldAsInteger64(getFieldIndex(name));
}

int64_t GdbRecordset::getFieldAsInteger64(int index) const {
    if (!m_currentFeature || index < 0) return 0;
    return m_currentFeature->GetFieldAsInteger64(index);
}

double GdbRecordset::getFieldAsDouble(const std::string& name) const {
    return getFieldAsDouble(getFieldIndex(name));
}

double GdbRecordset::getFieldAsDouble(int index) const {
    if (!m_currentFeature || index < 0) return 0.0;
    return m_currentFeature->GetFieldAsDouble(index);
}

std::string GdbRecordset::getFieldAsString(const std::string& name) const {
    return getFieldAsString(getFieldIndex(name));
}

std::string GdbRecordset::getFieldAsString(int index) const {
    if (!m_currentFeature || index < 0) return "";
    return m_currentFeature->GetFieldAsString(index);
}

const OGRGeometry* GdbRecordset::getGeometry() const {
    if (!m_currentFeature) return nullptr;
    return m_currentFeature->GetGeometryRef();
}

std::unique_ptr<OGRGeometry> GdbRecordset::cloneGeometry() const {
    auto geom = getGeometry();
    return geom ? std::unique_ptr<OGRGeometry>(geom->clone()) : nullptr;
}

void GdbRecordset::close() {
    if (m_currentFeature) {
        OGRFeature::DestroyFeature(m_currentFeature);
        m_currentFeature = nullptr;
    }
    m_eof = false;
}
```

---

### Task 9: 修改 CMakeLists.txt 添加组件库

**Files:**
- Modify: `fast_gdb/CMakeLists.txt`

- [ ] **Step 1: 在 gtest_discover_tests 之前添加组件库配置**

在 CMakeLists.txt 的 `# ===== Google Test =====` 注释之后、`find_package(GTest REQUIRED)` 之前插入：

```cmake
# ===== GDB Component Library (Phase 1A) =====
set(GDB_COMPONENT_SOURCES
    src/connection_info.cpp
    src/datasource.cpp
    src/datasets.cpp
    src/dataset.cpp
    src/recordset.cpp
)

add_library(gdb_component STATIC ${GDB_COMPONENT_SOURCES})
target_include_directories(gdb_component PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GDAL_INCLUDE_DIR}
)
target_link_libraries(gdb_component PRIVATE ${GDAL_LIBRARY})
target_compile_features(gdb_component PUBLIC cxx_std_17)
target_compile_options(gdb_component PRIVATE -Wall -Wno-unused-result)
```

- [ ] **Step 2: 修改测试 target 链接组件库**

将现有的：
```cmake
add_executable(gdb_tutorial_test_runner ${TEST_SOURCES})
target_include_directories(gdb_tutorial_test_runner PRIVATE ${GDAL_INCLUDE_DIR} tests)
target_link_libraries(gdb_tutorial_test_runner PRIVATE ${GDAL_LIBRARY} GTest::gtest GTest::gtest_main)
```

改为：
```cmake
add_executable(gdb_tutorial_test_runner ${TEST_SOURCES})
target_include_directories(gdb_tutorial_test_runner PRIVATE ${GDAL_INCLUDE_DIR} tests)
target_link_libraries(gdb_tutorial_test_runner PRIVATE ${GDAL_LIBRARY} GTest::gtest GTest::gtest_main gdb_component)
```

- [ ] **Step 3: 更新 build info 输出**

在最后一条 message 之前添加：

```cmake
message(STATUS "Component: gdb_component (Phase 1A 只读封装)")
```

---

### Task 10: 编写 test_011_component_api.cpp

**Files:**
- Create: `fast_gdb/tests/test_011_component_api.cpp`

- [ ] **Step 1: 编写测试文件**

```cpp
/**
 * 组件库 Phase 1A 测试 — 只读薄封装
 *
 * 对应教程 011：组件 API 基础
 * 5 个测试用例：打开/关闭、枚举图层、读字段、读几何、错误处理
 */

#include "test_fixture.h"
#include "datasource.h"
#include <filesystem>

/**
 * T011_OpenCloseGdb: 验证 GdbDatasource 打开和关闭。
 */
TEST_F(GdbTutorialFixture, T011_OpenCloseGdb) {
    const char* path = "/tmp/tutorial_011_open.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    // 创建图层
    ds->CreateLayer("points", nullptr, wkbPoint, nullptr);
    GDALClose(ds);

    // 用组件打开
    GdbDatasource gdb;
    EXPECT_TRUE(gdb.openExisting(path));
    EXPECT_TRUE(gdb.isOpen());
    EXPECT_EQ(gdb.getDatasetCount(), 1);

    gdb.close();
    EXPECT_FALSE(gdb.isOpen());
}

/**
 * T011_EnumerateDatasets: 验证枚举所有图层。
 */
TEST_F(GdbTutorialFixture, T011_EnumerateDatasets) {
    const char* path = "/tmp/tutorial_011_enum.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    ds->CreateLayer("layer_a", nullptr, wkbPoint, nullptr);
    ds->CreateLayer("layer_b", nullptr, wkbLineString, nullptr);
    ds->CreateLayer("layer_c", nullptr, wkbPolygon, nullptr);
    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDatasets datasets = gdb.getDatasets();
    EXPECT_EQ(datasets.getCount(), 3);

    EXPECT_STREQ(datasets.get(0).getName().c_str(), "layer_a");
    EXPECT_STREQ(datasets.get(1).getName().c_str(), "layer_b");
    EXPECT_STREQ(datasets.get(2).getName().c_str(), "layer_c");

    // 按名称查找
    GdbDataset byName = datasets.get("layer_b");
    EXPECT_TRUE(byName.isValid());
    EXPECT_STREQ(byName.getName().c_str(), "layer_b");

    // 不存在的图层
    GdbDataset notFound = datasets.get("nonexistent");
    EXPECT_FALSE(notFound.isValid());
}

/**
 * T011_ReadFields: 验证读取要素字段值。
 */
TEST_F(GdbTutorialFixture, T011_ReadFields) {
    const char* path = "/tmp/tutorial_011_fields.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("people", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFieldDefn nameField("name", OFTString);
    nameField.SetWidth(20);
    layer->CreateField(&nameField);

    OGRFieldDefn ageField("age", OFTInteger);
    layer->CreateField(&ageField);

    OGRFieldDefn heightField("height", OFTReal);
    layer->CreateField(&heightField);

    // 写入 2 个要素
    OGRFeature feat1(layer->GetLayerDefn());
    feat1.SetField("name", "Alice");
    feat1.SetField("age", 30);
    feat1.SetField("height", 165.5);
    OGRPoint pt1(1.0, 2.0);
    feat1.SetGeometry(&pt1);
    layer->CreateFeature(&feat1);

    OGRFeature feat2(layer->GetLayerDefn());
    feat2.SetField("name", "Bob");
    feat2.SetField("age", 25);
    feat2.SetField("height", 180.0);
    OGRPoint pt2(3.0, 4.0);
    feat2.SetGeometry(&pt2);
    layer->CreateFeature(&feat2);

    GDALClose(ds);

    // 用组件读取
    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDatasets datasets = gdb.getDatasets();
    GdbDataset people = datasets.get("people");
    ASSERT_TRUE(people.isValid());

    EXPECT_EQ(people.getFieldCount(), 3);
    EXPECT_STREQ(people.getFieldName(0), "name");
    EXPECT_STREQ(people.getFieldName(1), "age");
    EXPECT_STREQ(people.getFieldName(2), "height");

    GdbRecordset rs = people.getRecordset();

    // 第一条记录
    ASSERT_TRUE(rs.moveFirst());
    EXPECT_STREQ(rs.getFieldAsString("name").c_str(), "Alice");
    EXPECT_EQ(rs.getFieldAsInteger("age"), 30);
    EXPECT_NEAR(rs.getFieldAsDouble("height"), 165.5, 0.001);

    // 第二条记录
    ASSERT_TRUE(rs.moveNext());
    EXPECT_STREQ(rs.getFieldAsString("name").c_str(), "Bob");
    EXPECT_EQ(rs.getFieldAsInteger("age"), 25);
    EXPECT_NEAR(rs.getFieldAsDouble("height"), 180.0, 0.001);

    // 结束
    EXPECT_FALSE(rs.moveNext());
    EXPECT_TRUE(rs.isEOF());
}

/**
 * T011_ReadGeometry: 验证读取几何对象。
 */
TEST_F(GdbTutorialFixture, T011_ReadGeometry) {
    const char* path = "/tmp/tutorial_011_geom.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("points", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    OGRFeature feat(layer->GetLayerDefn());
    OGRPoint pt(42.5, -12.3);
    feat.SetGeometry(&pt);
    layer->CreateFeature(&feat);

    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    GdbDataset points = gdb.getDatasets().get("points");
    ASSERT_TRUE(points.isValid());
    EXPECT_EQ(points.getGeometryType(), wkbPoint);

    GdbRecordset rs = points.getRecordset();
    ASSERT_TRUE(rs.moveFirst());

    // 获取几何（const 指针）
    const OGRGeometry* geom = rs.getGeometry();
    ASSERT_NE(geom, nullptr);
    EXPECT_EQ(wkbFlatten(geom->getGeometryType()), wkbPoint);

    // 克隆几何（长期持有）
    auto cloned = rs.cloneGeometry();
    ASSERT_NE(cloned, nullptr);
    auto* clonedPt = dynamic_cast<OGRPoint*>(cloned.get());
    ASSERT_NE(clonedPt, nullptr);
    EXPECT_DOUBLE_EQ(clonedPt->getX(), 42.5);
    EXPECT_DOUBLE_EQ(clonedPt->getY(), -12.3);
}

/**
 * T011_ErrorHandling: 验证错误处理。
 */
TEST_F(GdbTutorialFixture, T011_ErrorHandling) {
    GdbDatasource gdb;

    // 打开不存在的路径
    EXPECT_FALSE(gdb.openExisting("/tmp/tutorial_011_noexist.gdb"));
    EXPECT_FALSE(gdb.isOpen());
    EXPECT_FALSE(gdb.getLastError().empty());

    // 空状态
    GdbDatasource emptyGdb;
    EXPECT_EQ(emptyGdb.getDatasetCount(), 0);
    EXPECT_FALSE(emptyGdb.isOpen());

    GdbDatasets emptyDatasets = emptyGdb.getDatasets();
    EXPECT_EQ(emptyDatasets.getCount(), 0);
    EXPECT_FALSE(emptyDatasets.get(0).isValid());
}
```

---

### Task 11: 构建并运行测试

**Files:**
- 无新建，验证已有代码

- [ ] **Step 1: 配置和构建**

```bash
cd "fast_gdb" && mkdir -p build && cd build && cmake .. 2>&1 && make 2>&1
```

Expected: 编译通过，无错误。输出包含 `Component: gdb_component (Phase 1A 只读封装)`。

- [ ] **Step 2: 运行 Phase 1A 测试**

```bash
./bin/gdb_tutorial_test_runner --gtest_filter='T011_*' 2>&1
```

Expected: 5 个测试全部 PASS。

- [ ] **Step 3: 运行全部测试（回归验证）**

```bash
./bin/gdb_tutorial_test_runner 2>&1
```

Expected: 32 个测试全部 PASS（原有 27 + 新增 5）。

- [ ] **Step 4: 确认 GDB问题修改/ 无变更**

```bash
cd .. && git status "../GDB问题修改/" 2>&1
```

Expected: 无任何变更。

---

### Task 12: 提交

- [ ] **Step 1: 提交 Phase 1A**

```bash
cd "fast_gdb"
git add src/ tests/test_011_component_api.cpp CMakeLists.txt docs/011_GDB_组件库设计文档.md
git commit -m "$(cat <<'EOF'
feat: GDB 组件库 Phase 1A — 只读薄封装

基于 SuperMap iObjects Datasource→Dataset→Recordset 模式：
- GdbDatasource: GDALDataset* 唯一所有者
- GdbDatasets/GdbDataset/GdbRecordset: 非拥有视图
- 共享错误上下文 GdbErrorContext
- 5 个测试用例：打开/关闭、枚举、字段读取、几何、错误处理
EOF
)"
```
