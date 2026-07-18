// src/edgar/explorgdb/reader/wkt_writer.h
// WKT writer — GeometryModel 的显式调试/兼容文本输出。

#ifndef EXPLORGDB_WKT_WRITER_H
#define EXPLORGDB_WKT_WRITER_H

#include "geometry_model.h"

#include <string>

namespace explorgdb {

/**
 * 从规范 GeometryModel 生成 WKT。
 *
 * ISO WKB 是 Reader 的正式交换契约；该 writer 仅用于显式调试、对照和内部
 * 转换。Polygon 使用已组织的外环/内环拓扑，不重新解释原始 FileGDB part。
 */
class WktWriter {
public:
    /** 返回与模型类型、Z/M 维度和 Empty 状态一致的 WKT。 */
    static std::string write(const GeometryModel& model);
};

} // namespace explorgdb

#endif // EXPLORGDB_WKT_WRITER_H
