// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

#ifndef EXPLORGDB_ADAPTIVE_BACKEND_INTERNAL_INC
#define EXPLORGDB_ADAPTIVE_BACKEND_INTERNAL_INC

#include "adaptive_backends.h"

namespace explorgdb::adaptive_backend_detail {

AdaptiveLayerBindingResult load_binding(
    const InProcessGdbCoordinator& coordinator,
    const std::string& gdb_path,
    const std::string& layer_name);

BackendReadResult read_fast(const std::string& gdb_path,
                            const std::string& layer_name,
                            const QueryRequest& request);
BackendCursor open_fast_cursor(const std::string& gdb_path,
                               const std::string& layer_name,
                               const QueryRequest& request);

BackendReadResult read_gdal(const std::string& gdb_path,
                            const AdaptiveLayerBinding& binding,
                            const QueryRequest& request);
BackendCursor open_gdal_cursor(const std::string& gdb_path,
                               const AdaptiveLayerBinding& binding,
                               const QueryRequest& request);

}  // namespace explorgdb::adaptive_backend_detail

#endif
