#ifndef EXPLORGDB_ADAPTIVE_BACKEND_INTERNAL_INC
#define EXPLORGDB_ADAPTIVE_BACKEND_INTERNAL_INC

#include "adaptive_backends.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace explorgdb::adaptive_backend_detail {

class CursorOpenFailure : public std::runtime_error {
public:
    CursorOpenFailure(BackendFailureKind failure, std::string message)
        : std::runtime_error(std::move(message)), failure_(failure) {}

    BackendFailureKind failure() const noexcept { return failure_; }

private:
    BackendFailureKind failure_ = BackendFailureKind::Open;
};

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
