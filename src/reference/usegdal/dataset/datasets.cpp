// src/datasets.cpp — GdbDatasets 和 GdbDataset 实现
//
// GdbDatasets 和 GdbDataset 是 GdbDatasource 的视图层，不拥有底层 GDAL 指针：
// - GdbDatasets：通过 GDALDataset* 提供图层枚举、创建、删除
// - GdbDataset：通过 OGRLayer* 提供记录集访问、查询、要素操作
//
// 关键设计决策：
// 1. OGRLayer 维护共享状态（游标位置、属性滤镜、空间滤镜）
//    因此 query() 每次调用前必须手动重置滤镜，防止条件叠加
// 2. getRecordset() 内部调用 ResetReading() 确保新游标从头开始
// 3. 写入操作（addNew/deleteAll）通过 OGRLayer 的 CreateFeature/DeleteFeature 完成

#include "datasets.h"
#include "query_builder.h"
#include "query_parameter.h"
#include "recordset.h"

// ========== GdbDataset ==========

// ========== 构造/析构 ==========

/** 默认构造。m_layer 初始为 nullptr，为无效视图。 */
GdbDataset::GdbDataset() = default;

/** 内部构造。由 GdbDatasets::get()/create() 调用。 */
GdbDataset::GdbDataset(OGRLayer* layer, GdbErrorContext* errCtx)
    : m_layer(layer), m_errorCtx(errCtx) {}

// ========== 图层信息 ==========

/** 获取图层名称。基于 OGRLayer::GetName()。无效图层返回空字符串。 */
std::string GdbDataset::getName() const {
    return m_layer ? m_layer->GetName() : "";
}

OGRwkbGeometryType GdbDataset::getGeometryType() const {
    return m_layer ? m_layer->GetGeomType() : wkbUnknown;
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
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
    m_layer->ResetReading();  // 确保新 recordset 从头开始
    return GdbRecordset(m_layer, m_errorCtx);
}

GdbRecordset GdbDataset::getRecordsetFiltered(const std::string& attributeFilter) const {
    GdbRecordset rs = getRecordset();
    if (rs.m_layer) {
        rs.m_layer->SetAttributeFilter(attributeFilter.c_str());
    }
    return rs;
}

GdbRecordset GdbDataset::query(const GdbQueryParameter& param) const {
    GdbRecordset rs = getRecordset();
    if (!rs.m_layer) return rs;

    // Reset previous filter state
    rs.m_layer->SetAttributeFilter(nullptr);
    rs.m_layer->SetSpatialFilter(nullptr);

    // 属性过滤
    if (!param.getAttributeFilter().empty()) {
        rs.m_layer->SetAttributeFilter(param.getAttributeFilter().c_str());
    }

    // 空间过滤
    if (param.getSpatialFilter() != nullptr) {
        rs.m_layer->SetSpatialFilter(const_cast<OGRGeometry*>(param.getSpatialFilter()));
    }

    // 结果字段（忽略不需要的字段以提升性能）
    if (!param.getResultFields().empty()) {
        std::vector<const char*> cstrs;
        for (const auto& f : param.getResultFields()) cstrs.push_back(f.c_str());
        cstrs.push_back(nullptr);
        rs.m_layer->SetIgnoredFields(cstrs.data());
    }

    // 不返回几何时忽略所有几何字段
    if (!param.hasGeometry()) {
        // GDAL 通过 SetIgnoredFields 忽略几何字段
        // 这里不处理，调用方可通过 setResultFields 控制
    }

    return rs;
}

bool GdbDataset::supportsAttributeFilter() const {
    // GDAL 所有 OGRLayer 都支持 SetAttributeFilter（SWQ 解析器）
    return m_layer != nullptr;
}

bool GdbDataset::supportsFastSpatialFilter() const {
    return m_layer && m_layer->TestCapability(OLCFastSpatialFilter);
}

bool GdbDataset::supportsFastFeatureCount() const {
    return m_layer && m_layer->TestCapability(OLCFastFeatureCount);
}

/**
 * getFeatureCountFiltered() — 使用 GdbQueryParameter 条件统计要素数量。
 *
 * 实现策略：通过 query() 获取过滤后的 recordset，遍历 moveNext() 逐个计数。
 * 不使用 GDAL 的快速计数（GetFeatureCount 只返回总数，不应用滤镜）。
 * 性能：O(N)，N 为符合条件的要素数量。
 */
int GdbDataset::getFeatureCountFiltered(const GdbQueryParameter& param) const {
    if (!m_layer) return -1;

    GdbRecordset rs = query(param);
    if (!rs.isValid()) return -1;

    int count = 0;
    while (rs.moveNext()) count++;
    return count;
}

GdbRecordset GdbDataset::query(const GdbQuery& q) const {
    // getRecordset() 内部调用 ResetReading() 重置游标，确保从头遍历
    // 如果 layer 无效（如空 Dataset），返回空 Recordset
    GdbRecordset rs = getRecordset();
    if (!rs.m_layer) return rs;

    // 【关键】OGRLayer 维护持久化滤镜状态，不会因创建新 Recordset 而清除。
    // 例如：之前 query("x > 1") 设置了属性过滤，后续不手动清空的话，
    // 下次查询会叠加该条件导致结果错误。必须先清空再设置新过滤。
    rs.m_layer->SetAttributeFilter(nullptr);
    rs.m_layer->SetSpatialFilter(nullptr);

    // 第 1 层：属性过滤
    // GDAL SWQ 解析器将 SQL-like 字符串转为 AST，在 GetNextFeature() 时逐要素检查
    // 支持：等于(=)、比较(>/</>=/<=)、LIKE、AND/OR 等
    if (!q.getWhere().empty()) {
        rs.m_layer->SetAttributeFilter(q.getWhere().c_str());
    }

    // 第 2 层：空间过滤（两层架构）
    if (q.getSpatialFilter() != nullptr) {
        // 第一层：OGRLayer bbox 预过滤
        // GDAL SetSpatialFilter() 用过滤几何的外接矩形做快速筛选，
        // 在 GetNextFeature() 中先检查 bbox 相交，再检查几何相交（文件驱动）。
        // 性能：有空间索引时 O(log N)，无索引时全表扫描但只做 bbox 比较。
        //
        // 【Disjoint 特殊处理】
        // Disjoint 需要"不相交"的要素，而 bbox 预过滤返回的是"相交"的候选集——
        // 正好把所有目标都排除了。所以 Disjoint 必须跳过预过滤，全表遍历。
        if (q.getSpatialRelation() != GdbSpatialRelation::Disjoint) {
            rs.m_layer->SetSpatialFilter(const_cast<OGRGeometry*>(q.getSpatialFilter()));
        }

        // 第二层：将空间关系元数据传递给 Recordset，供 moveNext() 后过滤使用
        // 注意：这里不操作 OGRLayer，只记录元数据。真正的后过滤发生在 moveNext() 中：
        //   - Intersects：直接返回（SetSpatialFilter 已处理，无需后过滤）
        //   - Within：geom->Within(filterGeom)
        //   - Contains：filterGeom->Contains(geom)
        //   - Disjoint：filterGeom->Disjoint(geom)
        rs.setSpatialFilter(q.getSpatialFilter());
        rs.setSpatialRelation(q.getSpatialRelation());
    }

    return rs;
}

/**
 * count() — 使用 GdbQuery 条件统计要素数量。
 *
 * 实现策略：通过 query() 获取过滤后的 recordset，遍历 moveNext() 逐个计数。
 * 支持 limit 截断：当 count >= limit 时停止遍历，提升性能。
 * 适用于"是否存在"、"前 N 条"等场景。
 */
int GdbDataset::count(const GdbQuery& q) const {
    if (!m_layer) return -1;

    GdbRecordset rs = query(q);
    if (!rs.isValid()) return -1;

    int count = 0;
    while (rs.moveNext()) {
        count++;
        if (q.getLimit() >= 0 && count >= q.getLimit()) break;
    }
    return count;
}

/**
 * addNew() — 在当前图层创建新要素。
 *
 * 实现策略：
 * 1. 基于 LayerDefn 创建 OGRFeature 模板（空要素，字段为默认值）
 * 2. 设置几何对象（如果提供）
 * 3. 遍历 fields map，按名称查找字段索引，SetField 设置字符串值
 *    注意：只支持字符串设置，数值类型由 GDAL 自动转换
 * 4. CreateFeature 写入图层（GDAL 自动分配 FID）
 *
 * 注意：此方法不修改 Recordset 游标位置，创建后需重新 query() 才能看到新要素。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbDataset::addNew(const OGRGeometry* geom, const std::map<std::string, std::string>& fields) {
    if (!m_layer) return false;

    OGRFeature feat(m_layer->GetLayerDefn());
    if (geom) {
        feat.SetGeometry(const_cast<OGRGeometry*>(geom));
    }
    for (const auto& [name, value] : fields) {
        int idx = m_layer->GetLayerDefn()->GetFieldIndex(name.c_str());
        if (idx >= 0) {
            feat.SetField(idx, value.c_str());
        }
    }

    OGRErr err = m_layer->CreateFeature(&feat);
    if (err != OGRERR_NONE) {
        if (m_errorCtx) m_errorCtx->setError("Failed to create feature");
        return false;
    }
    return true;
}

/**
 * deleteAll() — 删除图层中全部要素。
 *
 * 实现策略：
 * 1. ResetReading() 重置游标到开头
 * 2. GetNextFeature() 遍历每个要素
 * 3. 获取 FID，DeleteFeature(FID) 删除
 * 4. DestroyFeature() 释放要素指针（防止内存泄漏）
 *
 * 注意：此操作不可逆，建议在事务中执行。
 * 删除后图层为空，但图层定义（字段、几何类型）保持不变。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbDataset::deleteAll() {
    if (!m_layer) return false;

    m_layer->ResetReading();
    OGRFeature* feat;
    while ((feat = m_layer->GetNextFeature()) != nullptr) {
        int64_t fid = feat->GetFID();
        m_layer->DeleteFeature(fid);
        OGRFeature::DestroyFeature(feat);
    }
    return true;
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

// ========== 构造 ==========

/** 默认构造。m_ds 初始为 nullptr，为无效视图。 */
GdbDatasets::GdbDatasets() = default;

/** 内部构造。由 GdbDatasource::getDatasets() 调用。 */
GdbDatasets::GdbDatasets(GDALDataset* ds, GdbErrorContext* errCtx)
    : m_ds(ds), m_errorCtx(errCtx) {}

/** 获取图层数量。未打开时返回 0。 */
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

/**
 * create() — 在当前数据源创建新图层。
 *
 * 实现策略：调用 GDALDataset::CreateLayer() 创建 OGRLayer，
 * 返回包装后的 GdbDataset 视图。
 * 创建失败时（如名称冲突、驱动不支持）设置错误信息并返回无效视图。
 */
GdbDataset GdbDatasets::create(const std::string& name, OGRwkbGeometryType type,
                                const OGRSpatialReference* srs) const {
    if (!m_ds) return GdbDataset();
    OGRLayer* layer = m_ds->CreateLayer(name.c_str(), const_cast<OGRSpatialReference*>(srs), type, nullptr);
    if (!layer) {
        if (m_errorCtx) m_errorCtx->setError("Failed to create layer: " + name);
        return GdbDataset();
    }
    return GdbDataset(layer, m_errorCtx);
}

/**
 * remove() — 删除指定名称的图层。
 *
 * 实现策略：
 * 1. 遍历图层列表查找匹配名称的图层
 * 2. 获取图层索引（GDALDataset::DeleteLayer 需要索引参数）
 * 3. 调用 DeleteLayer(index) 删除
 *
 * 注意：不是所有驱动都支持 DeleteLayer（如 OpenFileGDB 只读格式）。
 * 删除失败时设置错误信息。
 */
bool GdbDatasets::remove(const std::string& name) const {
    if (!m_ds) return false;
    // GDALDataset::DeleteLayer 需要图层索引
    int count = m_ds->GetLayerCount();
    for (int i = 0; i < count; i++) {
        OGRLayer* layer = m_ds->GetLayer(i);
        if (layer && std::string(layer->GetName()) == name) {
            OGRErr err = m_ds->DeleteLayer(i);
            if (err != OGRERR_NONE) {
                if (m_errorCtx) m_errorCtx->setError("Failed to delete layer: " + name);
                return false;
            }
            return true;
        }
    }
    if (m_errorCtx) m_errorCtx->setError("Layer not found: " + name);
    return false;
}
