// src/edgar/explorgdb/reader/geometry/wkb_writer.h
// WKB writer — 将规范 GeometryModel 序列化为正式 ISO WKB 输出。

#ifndef EXPLORGDB_WKB_WRITER_H
#define EXPLORGDB_WKB_WRITER_H

#include "geometry_model.h"

#include <cstdint>

namespace explorgdb {

/** 构建配置可选择的几何输出策略。 */
enum class GeometryOutputMode : uint8_t {
    StandardLinearWkb = 0,
    NativeCurveIsoWkb = 1,
    DebugWkt = 2
};

/** GeometryModel 到 WKB-first GeometryValue 的无状态序列化器。 */
class WkbWriter {
public:
    /**
     * 生成 little-endian ISO WKB，并复制状态、SRID、维度和后端诊断元数据。
     *
     * 无效 GeometryModel 不写出字节；计数溢出或拓扑索引异常转换为失败状态，
     * 不抛出到 Reader 公开调用方。
     */
    /** 将规范几何模型序列化为 little-endian ISO WKB。
     * @param model 待序列化的规范几何模型。
     * @return 包含 WKB 和状态元数据的 GeometryValue。
     */
    static GeometryValue write(const GeometryModel& model);

    /**
     * 计算 ISO WKB 类型码：基础类型 1..6，加 1000/2000/3000 表示 Z/M/ZM。
     */
    /** 计算 ISO WKB 类型码及 Z/M 维度偏移。
     * @param kind 基础几何类型。
     * @param has_z 是否包含 Z 维。
     * @param has_m 是否包含 M 维。
     * @return ISO WKB 类型码。
     */
    static uint32_t iso_type_code(GeometryKind kind,
                                  bool has_z,
                                  bool has_m);
};

} // namespace explorgdb

#endif // EXPLORGDB_WKB_WRITER_H
