// src/edgar/explorgdb/writer/versioned_gdb_validator.h
// Reopen validation rules for VersionedGdbStore publication.

#ifndef EXPLORGDB_VERSIONED_GDB_VALIDATOR_H
#define EXPLORGDB_VERSIONED_GDB_VALIDATOR_H

#include "versioned_gdb_store.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace explorgdb {
namespace writer {

/** Validation contract for one logical FileGDB layer. */
struct GdbLayerValidationRule {
    std::string layer_name;

    /** Exact active-record count when the Writer knows the expected result. */
    std::optional<uint64_t> expected_active_records;

    /** Parse every active row and require scan count == active record count. */
    bool scan_all_records = true;

    /** FIDs that must reopen successfully after publication. */
    std::vector<uint32_t> sample_fids;

    /** Decode sample_fids through the WKB-first geometry path. */
    bool validate_sample_geometry = true;

    /** Require a non-empty and structurally parseable .spx file. */
    bool require_spatial_index = false;

    /** Require each named index in metadata and fully parse its .atx B+ tree. */
    std::vector<std::string> required_attribute_indexes;
};

struct QueryEngineGenerationValidationOptions {
    /** Require the directory-level FileGDB magic file to parse successfully. */
    bool require_directory_magic = true;

    /** At least one layer rule is required. */
    std::vector<GdbLayerValidationRule> layers;
};

/**
 * Build the mandatory VersionedGdbStore publication validator.
 *
 * The returned callback constructs fresh GdbCatalog, CatalogResolver and
 * QueryEngine objects from the candidate generation. It does not reuse Writer
 * state, cached paths, file descriptors or mmaps.
 */
GenerationValidator make_query_engine_generation_validator(
    QueryEngineGenerationValidationOptions options);

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_VERSIONED_GDB_VALIDATOR_H
