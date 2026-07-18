// src/edgar/explorgdb/reader/capability_report.h
// Reader 能力报告 — 汇总当前图层在纯 C++ Reader 下的可用性与降级原因。

#ifndef EXPLORGDB_CAPABILITY_REPORT_H
#define EXPLORGDB_CAPABILITY_REPORT_H

#include "catalog_resolver.h"
#include "gdb_table.h"
#include <string>

namespace explorgdb {

/** 单项能力的稳定状态，用于 API 诊断而不是异常控制流。 */
enum class CapabilityState { Supported, Degraded, Unsupported };

/** 能力项状态及其人类可读原因。 */
struct CapabilityItem {
    CapabilityState state = CapabilityState::Unsupported;
    std::string reason;
};

/**
 * 单个图层的 Reader 能力快照。
 *
 * 该结构只描述当前数据集在既有实现下能否读取、是否需要降级，以及索引/几何
 * 等关键能力是否可用；它不主动打开数据页，也不改变 catalog 或 table 状态。
 */
struct CapabilityReport {
    CapabilityItem srs;              ///< 空间参考解析能力。
    CapabilityItem curve_geometry;   ///< 曲线几何解析或降级能力。
    CapabilityItem multipatch;       ///< MultiPatch 支持状态。
    CapabilityItem raster;           ///< Raster 字段支持状态。
    CapabilityItem spatial_index;    ///< .spx 空间索引可用性。
    CapabilityItem attribute_index;  ///< .atx 属性索引可用性。

    /** 当没有阻断性 Unsupported 项时，图层可由 Reader 读取。 */
    bool can_read_layer() const;

    /**
     * 基于完整 catalog/resolver 检查图层能力。
     *
     * @param catalog 已解析的 GDB catalog。
     * @param resolver catalog 关系与路径解析器。
     * @param table_id 目标图层的 catalog id。
     * @param table 已打开或已加载字段定义的表解析器。
     */
    static CapabilityReport inspect(const GdbCatalog& catalog,
                                    const CatalogResolver& resolver,
                                    uint32_t table_id,
                                    const GdbTableParser& table);

    /**
     * 轻量检查入口；调用方已知空间参考表是否存在时使用。
     */
    static CapabilityReport inspect(const GdbCatalog& catalog,
                                    bool has_spatial_refs,
                                    uint32_t table_id,
                                    const GdbTableParser& table);
};

/** 返回 CapabilityState 的稳定诊断名称。 */
const char* capability_state_name(CapabilityState state);

} // namespace explorgdb

#endif // EXPLORGDB_CAPABILITY_REPORT_H
