// src/edgar/explorgdb/adaptive/adaptive_backends.cpp

#include "adaptive_backends.h"
#include "adaptive_backend_internal.inc"

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace explorgdb {
namespace {

bool blank_text(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
}

bool empty_where_request(const QueryRequest& request) {
    return (request.kind == QueryKind::WhereClause ||
            request.kind == QueryKind::SpatialWhere) &&
           blank_text(request.where_clause);
}

BackendReadResult rejected_empty_where() {
    QueryResult result;
    result.execution_path = "gdal:openfilegdb:rejected";
    result.fallback_reason = "empty where clause";
    return BackendReadResult::success(std::move(result));
}

bool nullable_bit_is_set(const FeatureRecord& record, size_t nullable_bit) {
    const size_t byte_index = nullable_bit / 8U;
    const size_t bit_index = nullable_bit % 8U;
    return byte_index < record.nullable_flags.size() &&
           ((record.nullable_flags[byte_index] >> bit_index) & 1U) != 0;
}

bool validate_record_contract(const AdaptiveLayerBinding& binding,
                              const FeatureRecord& record,
                              std::string& error) {
    if (record.field_values.size() != binding.fields.size()) {
        error = "GDAL record field count does not match adaptive binding";
        return false;
    }

    size_t nullable_bit = 0;
    for (size_t index = 0; index < binding.fields.size(); ++index) {
        const FieldDescriptor& field = binding.fields[index];
        const FieldValue& value = record.field_values[index];
        const bool nullable = (field.flag & 1U) != 0;
        const bool is_null = std::holds_alternative<std::nullptr_t>(value);

        if (!nullable && is_null && field.type != FieldType::Raster) {
            error = "GDAL returned NULL for non-nullable field: " + field.name;
            return false;
        }

        if (nullable) {
            const bool bit_set = nullable_bit_is_set(record, nullable_bit);
            const bool null_contract_value = is_null ||
                (field.type == FieldType::Geometry && bit_set);
            if (bit_set != null_contract_value) {
                error = "GDAL nullable bitmap disagrees with field value: " +
                        field.name;
                return false;
            }
            ++nullable_bit;
        }
    }
    return true;
}

bool validate_feature_contract(const AdaptiveLayerBinding& binding,
                               const QueryFeature& feature,
                               std::string& error) {
    if (feature.record.fid != feature.fid) {
        error = "GDAL feature and record FID disagree";
        return false;
    }
    return validate_record_contract(binding, feature.record, error);
}

BackendCursor rejected_empty_where_cursor() {
    BackendCursor cursor;
    cursor.next = [](QueryFeature&, std::string&) { return false; };
    cursor.close = [] {};
    return cursor;
}

BackendCursor validating_cursor(BackendCursor raw,
                                AdaptiveLayerBinding binding) {
    auto raw_cursor = std::make_shared<BackendCursor>(std::move(raw));
    auto schema = std::make_shared<AdaptiveLayerBinding>(std::move(binding));

    BackendCursor cursor;
    cursor.next = [raw_cursor, schema](QueryFeature& feature,
                                      std::string& error) {
        if (!raw_cursor->next || !raw_cursor->next(feature, error)) return false;
        if (!validate_feature_contract(*schema, feature, error)) return false;
        return true;
    };
    cursor.close = [raw_cursor] {
        if (raw_cursor->close) raw_cursor->close();
    };
    return cursor;
}

}  // namespace

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
    if (empty_where_request(request)) return rejected_empty_where();

    BackendReadResult result = adaptive_backend_detail::read_gdal(
        gdb_path_, binding_, request);
    if (result.ok && result.result.record.has_value()) {
        std::string error;
        if (!validate_record_contract(
                binding_, *result.result.record, error)) {
            return BackendReadResult::read_failure(std::move(error));
        }
    }
    return result;
}

BackendCursor GdalOpenFileGdbReadBackend::open_cursor(
    const QueryRequest& request) const {
    if (empty_where_request(request)) return rejected_empty_where_cursor();
    return validating_cursor(
        adaptive_backend_detail::open_gdal_cursor(
            gdb_path_, binding_, request),
        binding_);
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
