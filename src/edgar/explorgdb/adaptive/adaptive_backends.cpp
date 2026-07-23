// src/edgar/explorgdb/adaptive/adaptive_backends.cpp

#include "adaptive_backends.h"
#include "adaptive_backend_internal.inc"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace explorgdb {

AdaptiveLayerBindingResult load_adaptive_layer_binding(
    const InProcessGdbCoordinator& coordinator,
    const std::string& gdb_path,
    const std::string& layer_name) {
    return adaptive_backend_detail::load_binding(
        coordinator, gdb_path, layer_name);
}

FastGdbReadBackend::FastGdbReadBackend(std::string gdb_path,
                                       std::string layer_name)
    : gdb_path_(std::move(gdb_path)),
      layer_name_(std::move(layer_name)) {}

BackendReadResult FastGdbReadBackend::read(
    const QueryRequest& request) const {
    return adaptive_backend_detail::read_fast(
        gdb_path_, layer_name_, request);
}

BackendCursor FastGdbReadBackend::open_cursor(
    const QueryRequest& request) const {
    return adaptive_backend_detail::open_fast_cursor(
        gdb_path_, layer_name_, request);
}

GdalOpenFileGdbReadBackend::GdalOpenFileGdbReadBackend(
    std::string gdb_path,
    AdaptiveLayerBinding binding)
    : gdb_path_(std::move(gdb_path)),
      binding_(std::move(binding)) {}

BackendReadResult GdalOpenFileGdbReadBackend::read(
    const QueryRequest& request) const {
    return adaptive_backend_detail::read_gdal(
        gdb_path_, binding_, request);
}

BackendCursor GdalOpenFileGdbReadBackend::open_cursor(
    const QueryRequest& request) const {
    return adaptive_backend_detail::open_gdal_cursor(
        gdb_path_, binding_, request);
}

AdaptiveReadSession make_adaptive_read_session(
    InProcessGdbCoordinator coordinator,
    std::string gdb_path,
    AdaptiveLayerBinding binding) {
    const std::string layer_name = binding.layer_name;
    const uint64_t binding_generation = binding.generation;
    InProcessGdbCoordinator state_reader = coordinator;
    const std::string state_path = gdb_path;

    auto fast = std::make_shared<FastGdbReadBackend>(
        gdb_path, layer_name);
    auto gdal = std::make_shared<GdalOpenFileGdbReadBackend>(
        gdb_path, std::move(binding));

    return AdaptiveReadSession(
        std::move(coordinator), std::move(gdb_path),
        [fast](const QueryRequest& request) {
            return fast->read(request);
        },
        [gdal, state_reader, state_path,
         binding_generation](const QueryRequest& request) {
            if (state_reader.state(state_path).generation !=
                binding_generation) {
                return BackendReadResult::read_failure(
                    "adaptive layer binding expired; rebuild the session");
            }
            return gdal->read(request);
        },
        [fast](const QueryRequest& request) {
            return fast->open_cursor(request);
        },
        [gdal, state_reader, state_path,
         binding_generation](const QueryRequest& request) {
            if (state_reader.state(state_path).generation !=
                binding_generation) {
                throw std::runtime_error(
                    "adaptive layer binding expired; rebuild the session");
            }
            return gdal->open_cursor(request);
        });
}

}  // namespace explorgdb
