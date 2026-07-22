// Reopen validation rules for VersionedGdbStore publication.

#ifndef FAST_GDB_PUBLIC_VERSIONED_GDB_VALIDATOR_H
#define FAST_GDB_PUBLIC_VERSIONED_GDB_VALIDATOR_H

#include "versioned_gdb_store.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace explorgdb {
namespace writer {

struct GdbLayerValidationRule {
    std::string layer_name;
    std::optional<uint64_t> expected_active_records;
    bool scan_all_records = true;
    std::vector<uint32_t> sample_fids;
    bool validate_sample_geometry = true;
    bool require_spatial_index = false;
    std::vector<std::string> required_attribute_indexes;
};

struct QueryEngineGenerationValidationOptions {
    bool require_directory_magic = true;
    std::vector<GdbLayerValidationRule> layers;
};

GenerationValidator make_query_engine_generation_validator(
    QueryEngineGenerationValidationOptions options);

}  // namespace writer
}  // namespace explorgdb

#endif  // FAST_GDB_PUBLIC_VERSIONED_GDB_VALIDATOR_H
