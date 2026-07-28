// src/edgar/explorgdb/adaptive/adaptive_backends.h
// Adaptive Reader 的正式 fast-gdb 与 fresh OpenFileGDB 读取后端。

#ifndef EXPLORGDB_ADAPTIVE_BACKENDS_H
#define EXPLORGDB_ADAPTIVE_BACKENDS_H

#include "adaptive_reader.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace explorgdb {

/**
 * GDAL 读取所需的稳定层绑定。
 *
 * fields 保留 fast-gdb 字段顺序与类型，使 GDAL cursor 能输出与 fast cursor
 * 可对照的 FeatureRecord。attribute_index_fields 把 QueryRequest::index_name
 * 映射到 OGR 字段名；键按 ASCII 大小写不敏感处理。generation 绑定加载时的
 * 稳定代次，后续代次不匹配时必须重建 session，禁止使用旧 Schema。
 */
struct AdaptiveLayerBinding {
    std::string layer_name;
    uint64_t generation = 0;
    std::vector<FieldDescriptor> fields;
    std::unordered_map<std::string, std::string> attribute_index_fields;
};

struct AdaptiveLayerBindingResult {
    bool ok = false;
    AdaptiveLayerBinding binding;
    std::string error;
};

/**
 * 在 Stable 状态下使用 fast-gdb 读取并缓存层 Schema 与索引名到字段名映射。
 * WriterPending/Active 或 fail-closed 状态下返回失败，不读取源文件。
 */
AdaptiveLayerBindingResult load_adaptive_layer_binding(
    const InProcessGdbCoordinator& coordinator,
    const std::string& gdb_path,
    const std::string& layer_name);

/** 每次 query/cursor 独立打开 fast-gdb Reader 对象图。 */
class FastGdbReadBackend {
public:
    FastGdbReadBackend(std::string gdb_path, std::string layer_name);

    BackendReadResult read(const QueryRequest& request) const;
    BackendCursor open_cursor(const QueryRequest& request) const;

private:
    std::string gdb_path_;
    std::string layer_name_;
};

/**
 * 官方 GDAL/OpenFileGDB 只读后端。
 *
 * read() 每次 fresh open/materialize/close；open_cursor() 返回的 cursor 独占一个
 * GDALDataset，销毁时关闭。该类不包含任何 Writer API。
 */
class GdalOpenFileGdbReadBackend {
public:
    GdalOpenFileGdbReadBackend(std::string gdb_path,
                               AdaptiveLayerBinding binding);

    BackendReadResult read(const QueryRequest& request) const;
    BackendCursor open_cursor(const QueryRequest& request) const;

private:
    std::string gdb_path_;
    AdaptiveLayerBinding binding_;
};

/**
 * 使用正式 fast 与 fresh GDAL 后端构造 AdaptiveReadSession。
 * binding 必须在 Stable 状态预先加载；Writer 关闭导致 generation 变化后，旧
 * binding 的并发 GDAL 路径会 fail closed，调用方必须重新加载 binding 并重建 session。
 */
AdaptiveReadSession make_adaptive_read_session(
    InProcessGdbCoordinator coordinator,
    std::string gdb_path,
    AdaptiveLayerBinding binding);

}  // namespace explorgdb

#endif  // EXPLORGDB_ADAPTIVE_BACKENDS_H
