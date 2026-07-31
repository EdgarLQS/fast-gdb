// src/recordset.cpp — GdbRecordset 实现
//
// GdbRecordset 是 OGRLayer 的 RAII 封装，提供顺序游标 API。
// 核心关注点：
// 1. OGRFeature* 生命周期管理：GetNextFeature 获取 → DestroyFeature 释放
// 2. 空间关系后过滤：非 Intersects 时逐要素检查 OGRGeometry
// 3. 与 OGRLayer 的共享状态：游标位置、滤镜持久化

#include "recordset.h"
#include "feature.h"

// ========== 构造/析构 ==========

GdbRecordset::GdbRecordset() = default;

GdbRecordset::GdbRecordset(OGRLayer* layer, GdbErrorContext* errCtx)
    : m_layer(layer), m_errorCtx(errCtx) {}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
GdbRecordset::~GdbRecordset() { close(); }

// 移动构造：接管源对象的资源，源对象置为空状态
GdbRecordset::GdbRecordset(GdbRecordset&& other) noexcept
    : m_layer(other.m_layer),
      m_currentFeature(other.m_currentFeature),
      m_errorCtx(other.m_errorCtx),
      m_eof(other.m_eof),
      m_spatialRelation(other.m_spatialRelation),
      m_spatialFilter(other.m_spatialFilter) {
    other.m_layer = nullptr;
    other.m_currentFeature = nullptr;
    other.m_errorCtx = nullptr;
    other.m_eof = false;
    other.m_spatialRelation = GdbSpatialRelation::Intersects;
    other.m_spatialFilter = nullptr;
}

// 移动赋值：先释放自身资源，再接管源对象
GdbRecordset& GdbRecordset::operator=(GdbRecordset&& other) noexcept {
    if (this != &other) {
        close();
        m_layer = other.m_layer;
        m_currentFeature = other.m_currentFeature;
        m_errorCtx = other.m_errorCtx;
        m_eof = other.m_eof;
        m_spatialRelation = other.m_spatialRelation;
        m_spatialFilter = other.m_spatialFilter;
        other.m_layer = nullptr;
        other.m_currentFeature = nullptr;
        other.m_errorCtx = nullptr;
        other.m_eof = false;
        other.m_spatialRelation = GdbSpatialRelation::Intersects;
        other.m_spatialFilter = nullptr;
    }
    return *this;
}

// ========== 顺序游标操作 ==========

/**
 * 移动游标到第一条要素。
 *
 * 实现策略：
 * 1. close() 释放当前持有的要素指针（如果存在）
 * 2. ResetReading() 重置 OGRLayer 内部游标到开头
 *    注意：这是解决共享游标问题的关键——OGRLayer 的游标是持久化的，
 *    之前的 GetNextFeature 调用会推进游标位置。如果不调用 ResetReading，
 *    新的 Recordset 或重复遍历会从上次停止的位置继续，而不是从头开始。
 * 3. moveNext() 获取第一条要素
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbRecordset::moveFirst() {
    if (!m_layer) {
        m_eof = true;
        return false;
    }
    close();
    m_layer->ResetReading();
    return moveNext();
}

/**
 * 移动游标到下一条要素。
 *
 * 实现策略分两种情况：
 *
 * 【情况 1：Intersects（默认）】
 * OGRLayer 的 SetSpatialFilter() 已经在 query() 中设置了 bbox 预过滤。
 * GDAL 的 GetNextFeature() 内部会自动应用空间过滤，所以这里直接获取即可。
 * 无需额外的后过滤步骤。
 *
 * 【情况 2：Contains/Within/Disjoint】
 * SetSpatialFilter 只能做 Intersects 预过滤，无法满足其他关系。
 * 解决方案：两层过滤
 *   - 第一层：bbox 预过滤（Disjoint 除外，见 query() 中的特殊处理）
 *     在 query() 中设置，GetNextFeature 自动应用
 *   - 第二层：逐要素几何关系验证，在这里的 while 循环中执行
 *
 *   循环逻辑：
 *   1. GetNextFeature() 获取一个候选要素（可能已被 bbox 预过滤）
 *   2. 获取要素的几何对象
 *   3. 无几何时：
 *      - Disjoint：视为匹配（没有几何 = 不与 filter 相交），返回该要素
 *      - 其他关系：跳过（没有几何无法判断包含/在...内）
 *   4. 有几何时：调用 OGRGeometry 的对应关系方法
 *      - Contains：filterGeom->Contains(featureGeom)
 *        含义：过滤几何完全包含要素几何
 *      - Within：featureGeom->Within(filterGeom)
 *        含义：要素几何完全在过滤几何内
 *      - Disjoint：filterGeom->Disjoint(featureGeom)
 *        含义：两几何不相交（要素不在过滤几何内）
 *   5. 匹配则停止循环，返回此要素
 *   6. 不匹配则销毁要素继续下一个
 *
 *   内存管理注意事项：
 *   - GetNextFeature() 返回的 OGRFeature* 必须由调用方负责释放
 *   - 不匹配的要素必须手动 DestroyFeature() 才能继续循环
 *   - 匹配的要素保留在 m_currentFeature 中，供后续读取使用
 *
 * @return true 成功移动到下一条要素，false 已到达末尾（EOF）
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbRecordset::moveNext() {
    if (!m_layer) {
        m_eof = true;
        return false;
    }

    // 释放上一次 moveNext() 获取的要素指针，防止内存泄漏
    close();

    // 【Intersects 快速路径】
    // SetSpatialFilter 已在 query() 中设置，GetNextFeature 内部自动应用
    // bbox 预过滤 + 几何相交验证（文件驱动由基类 OGRLayer 处理）
    if (m_spatialRelation == GdbSpatialRelation::Intersects) {
        m_currentFeature = m_layer->GetNextFeature();
        m_eof = (m_currentFeature == nullptr);
        return !m_eof;
    }

    // 【非 Intersects：bbox 预过滤 + 几何关系后过滤】
    // 遍历候选要素，逐一到检查空间关系，直到找到匹配的或遍历完
    while ((m_currentFeature = m_layer->GetNextFeature()) != nullptr) {
        const OGRGeometry* geom = m_currentFeature->GetGeometryRef();

        if (!geom) {
            // 要素没有几何对象
            // Disjoint：没有几何 = 不与 filter 相交 = 匹配
            // 其他（Contains/Within）：无法判断，跳过
            if (m_spatialRelation == GdbSpatialRelation::Disjoint) break;
            OGRFeature::DestroyFeature(m_currentFeature);
            m_currentFeature = nullptr;
            continue;
        }

        // 检查空间关系
        bool match = false;
        switch (m_spatialRelation) {
            case GdbSpatialRelation::Contains:
                // filterGeom->Contains(featureGeom)：过滤几何包含要素几何
                match = m_spatialFilter->Contains(geom);
                break;
            case GdbSpatialRelation::Within:
                // featureGeom->Within(filterGeom)：要素几何在过滤几何内
                // 对点 + 多边形场景：点在多边形内 = Within = true
                match = geom->Within(m_spatialFilter);
                break;
            case GdbSpatialRelation::Disjoint:
                // filterGeom->Disjoint(featureGeom)：两几何不相交
                match = m_spatialFilter->Disjoint(geom);
                break;
            default:
                match = true;
                break;
        }

        if (match) {
            // 找到匹配的要素，保留 m_currentFeature 供后续读取
            break;
        }

        // 不匹配：必须手动释放要素，否则内存泄漏
        OGRFeature::DestroyFeature(m_currentFeature);
        m_currentFeature = nullptr;
    }

    m_eof = (m_currentFeature == nullptr);
    return !m_eof;
}

bool GdbRecordset::isEOF() const { return m_eof; }

bool GdbRecordset::isValid() const { return m_layer != nullptr; }

// ========== 当前要素读取 ==========

int64_t GdbRecordset::getFid() const {
    return m_currentFeature ? m_currentFeature->GetFID() : -1;
}

// ========== 字段访问 ==========

/**
 * 字段总数。基于 OGRFeatureDefn（图层定义），不依赖当前要素。
 * 即使没有当前要素（未调用 moveNext），也能获取字段数。
 */
int GdbRecordset::getFieldCount() const {
    return m_layer ? m_layer->GetLayerDefn()->GetFieldCount() : 0;
}

std::string GdbRecordset::getFieldName(int index) const {
    if (!m_layer || index < 0 || index >= getFieldCount()) return "";
    return m_layer->GetLayerDefn()->GetFieldDefn(index)->GetNameRef();
}

/**
 * 按名称查找字段索引。OGRLayer::GetFieldIndex 内部遍历字段定义表。
 * 不存在时返回 -1，不会报错。
 */
int GdbRecordset::getFieldIndex(const std::string& name) const {
    if (!m_layer) return -1;
    return m_layer->GetLayerDefn()->GetFieldIndex(name.c_str());
}

// ========== 类型化读取 ==========

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

// ========== 几何访问 ==========

/**
 * 获取当前要素的几何对象指针。
 *
 * 重要：返回的指针由 OGRFeature 管理，不是独立拥有的。
 * 以下情况会导致指针失效：
 * - 调用下一次 moveNext()（会 DestroyFeature 释放当前要素）
 * - 调用 close()
 * - Recordset 被销毁
 * 如需长期持有几何，使用 cloneGeometry() 获取独立副本。
 */
const OGRGeometry* GdbRecordset::getGeometry() const {
    if (!m_currentFeature) return nullptr;
    return m_currentFeature->GetGeometryRef();
}

/**
 * 克隆当前要素的几何对象。
 *
 * 通过 OGRGeometry::clone() 创建深拷贝，返回独立拥有的 unique_ptr。
 * 调用方负责管理生命周期，不受 Recordset 影响。
 * 适用场景：需要保存几何对象供后续使用，或传递给其他 Recordset 操作。
 */
std::unique_ptr<OGRGeometry> GdbRecordset::cloneGeometry() const {
    auto geom = getGeometry();
    return geom ? std::unique_ptr<OGRGeometry>(geom->clone()) : nullptr;
}

// ========== 要素抽象 ==========

/**
 * 将当前要素转为 GdbFeature 值对象。
 *
 * GdbFeature 是独立拥有的值类型（深拷贝几何、拷贝字段），
 * 可长期持有、跨 Recordset 传递、存入容器。
 * 与 getGeometry()/getFieldAsXxx() 的区别：
 * - getGeometry()：返回裸指针，受 Recordset 生命周期约束
 * - getFieldAsXxx()：只返回单个字段值
 * - getFeature()：返回完整的要素副本，包含所有字段和几何
 *
 * 典型用途：在 moveNext() 循环中收集要素到 vector，或传递给其他模块处理。
 */
GdbFeature GdbRecordset::getFeature() const {
    if (!m_currentFeature) return GdbFeature();
    return GdbFeature::fromNative(m_currentFeature);
}

// ========== 关闭操作 ==========

/**
 * 关闭记录集，释放当前要素指针。
 *
 * 析构函数自动调用此方法。调用后：
 * - m_currentFeature 被释放，getFid()/getFieldAsXxx() 返回默认值
 * - m_eof 被重置（不影响 layer 游标位置）
 * - Recordset 对象本身仍有效，可继续调用 moveNext() 遍历
 *
 * 注意：此方法不关闭 OGRLayer（layer 由 GdbDatasource 管理）。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
void GdbRecordset::close() {
    if (m_currentFeature) {
        OGRFeature::DestroyFeature(m_currentFeature);
        m_currentFeature = nullptr;
    }
    m_eof = false;
}

// ========== 写入操作 ==========

/**
 * 准备编辑当前要素。
 *
 * GDAL 行为说明：
 * GetNextFeature() 返回的 OGRFeature* 是一个独立对象，
 * 可以直接调用 SetField()/SetGeometry() 修改其内容，
 * 然后调用 OGRLayer::SetFeature() 将修改写回图层。
 *
 * 不需要显式 "begin edit" 操作，但需要确保：
 * 1. 当前有要素（已调用过 moveNext()）
 * 2. 要素属于该 OGRLayer
 *
 * 此方法仅做验证，实际编辑通过 setField()/setGeometry() 完成。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbRecordset::edit() {
    if (!m_currentFeature || !m_layer) {
        if (m_errorCtx) m_errorCtx->setError("No current feature to edit");
        return false;
    }
    return true;
}

/**
 * 修改当前要素的指定字段值（int32_t 重载）。
 *
 * 仅修改内存中的 OGRFeature 对象，不会立即写回图层。
 * 必须调用 update() 才能持久化修改。
 * 调用前提：当前有要素（edit() 或 addNew() 后）。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbRecordset::setField(const std::string& name, int32_t value) {
    if (!m_currentFeature) return false;
    int idx = getFieldIndex(name);
    if (idx < 0) return false;
    m_currentFeature->SetField(idx, value);
    return true;
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbRecordset::setField(const std::string& name, double value) {
    if (!m_currentFeature) return false;
    int idx = getFieldIndex(name);
    if (idx < 0) return false;
    m_currentFeature->SetField(idx, value);
    return true;
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbRecordset::setField(const std::string& name, const std::string& value) {
    if (!m_currentFeature) return false;
    int idx = getFieldIndex(name);
    if (idx < 0) return false;
    m_currentFeature->SetField(idx, value.c_str());
    return true;
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbRecordset::setField(const std::string& name, int64_t value) {
    if (!m_currentFeature) return false;
    int idx = getFieldIndex(name);
    if (idx < 0) return false;
    m_currentFeature->SetField(idx, value);
    return true;
}

/**
 * 修改当前要素的几何对象。
 *
 * 仅修改内存中的 OGRFeature 对象，不会立即写回图层。
 * 必须调用 update() 才能持久化修改。
 * 调用前提：当前有要素，且几何指针非空（不能设置为 null 几何）。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbRecordset::setGeometry(const OGRGeometry* geom) {
    if (!m_currentFeature) return false;
    if (!geom) return false;
    m_currentFeature->SetGeometry(const_cast<OGRGeometry*>(geom));
    return true;
}

/**
 * 提交当前要素的修改到 OGRLayer。
 *
 * 实现流程：
 * 1. SetFeature(m_currentFeature) 将修改后的要素写回图层
 *    根据 FID 查找并替换原始要素
 * 2. ResetReading() 重置游标
 *    这是关键步骤——如果不重置，后续 moveNext() 会从
 *    SetFeature 后的位置继续，可能跳过或重复读取要素。
 *
 * OpenFileGDB 特性：
 * 不销毁 m_currentFeature（OpenFileGDB 需要保持 feature 引用），
 * 但标记为已更新，避免后续操作重复使用。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbRecordset::update() {
    if (!m_currentFeature || !m_layer) {
        if (m_errorCtx) m_errorCtx->setError("No current feature to update");
        return false;
    }
    OGRErr err = m_layer->SetFeature(m_currentFeature);
    if (err != OGRERR_NONE) {
        if (m_errorCtx) m_errorCtx->setError("Failed to update feature");
        return false;
    }
    // 重置游标防止后续操作游标越界
    // 注意：不销毁 m_currentFeature，因为 SetFeature 后要素仍有效
    m_layer->ResetReading();
    return true;
}

/**
 * 删除当前要素。
 *
 * 实现流程：
 * 1. 获取 FID（OGRLayer::DeleteFeature 需要 FID 参数）
 * 2. 调用 DeleteFeature(FID) 从图层中删除
 * 3. 手动释放 m_currentFeature（删除后该指针已失效）
 *
 * 删除后当前要素指针置为 nullptr，
 * 后续的 getFid()/getFieldAsXxx() 返回默认值，
 * 必须调用 moveNext() 获取下一条要素才能继续操作。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbRecordset::deleteCurrent() {
    if (!m_currentFeature || !m_layer) {
        if (m_errorCtx) m_errorCtx->setError("No current feature to delete");
        return false;
    }
    int64_t fid = m_currentFeature->GetFID();
    OGRErr err = m_layer->DeleteFeature(fid);
    if (err != OGRERR_NONE) {
        if (m_errorCtx) m_errorCtx->setError("Failed to delete feature");
        return false;
    }
    // 要素已删除，释放指针防止悬空引用
    OGRFeature::DestroyFeature(m_currentFeature);
    m_currentFeature = nullptr;
    return true;
}
