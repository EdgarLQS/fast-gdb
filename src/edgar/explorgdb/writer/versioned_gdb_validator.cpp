// src/edgar/explorgdb/writer/versioned_gdb_validator.cpp

#include "versioned_gdb_validator.h"

#include "catalog_resolver.h"
#include "gdb_attribute_index.h"
#include "gdb_catalog.h"
#include "gdb_spatial_index.h"
#include "query_engine.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace explorgdb {
namespace writer {

namespace fs = std::filesystem;

namespace {

GenerationValidationResult validation_failure(const std::string& layer,
                                               const std::string& message) {
    if (layer.empty()) {
        return GenerationValidationResult::failure(message);
    }
    return GenerationValidationResult::failure("layer '" + layer + "': " +
                                               message);
}

bool contains_index_name(const std::vector<IndexEntry>& entries,
                         const std::string& index_name) {
    return std::any_of(entries.begin(), entries.end(),
                       [&](const IndexEntry& entry) {
                           return entry.name == index_name;
                       });
}

GenerationValidationResult validate_sample_fid(
    const GdbLayerValidationRule& rule,
    QueryEngine& engine,
    GdbTableParser& table,
    uint32_t fid) {
    FeatureRecord record;
    if (!rule.validate_sample_geometry) {
        if (!engine.read_by_fid(fid, record)) {
            return validation_failure(
                rule.layer_name,
                "sample FID " + std::to_string(fid) +
                    " could not be reopened");
        }
    } else {
        GeometryValue geometry;
        if (!table.read_feature_by_fid(fid, record, geometry)) {
            return validation_failure(
                rule.layer_name,
                "sample FID " + std::to_string(fid) +
                    " could not be reopened through the geometry path");
        }
        if (!geometry.valid()) {
            return validation_failure(
                rule.layer_name,
                "sample FID " + std::to_string(fid) +
                    " has invalid geometry: " + geometry.diagnostic);
        }
    }

    if (record.fid != fid) {
        return validation_failure(
            rule.layer_name,
            "sample FID mapping mismatch for " + std::to_string(fid));
    }
    return GenerationValidationResult::success();
}

GenerationValidationResult validate_spatial_index(
    const fs::path& generation_path,
    const GdbLayerValidationRule& rule,
    const ResolvedTable& resolved,
    const GdbCatalog& catalog) {
    const CatalogEntry* spatial_index = catalog.find_spx(resolved.id);
    if (spatial_index == nullptr || spatial_index->file_size == 0) {
        return validation_failure(
            rule.layer_name,
            "required spatial index is missing or empty");
    }

    GdbSpatialIndexParser parser(
        (generation_path / spatial_index->filename).string());
    if (!parser.parse()) {
        return validation_failure(
            rule.layer_name,
            "required spatial index could not be parsed");
    }
    return GenerationValidationResult::success();
}

GenerationValidationResult validate_attribute_indexes(
    const fs::path& generation_path,
    const GdbLayerValidationRule& rule,
    const ResolvedTable& resolved,
    const GdbCatalog& catalog) {
    if (rule.required_attribute_indexes.empty()) {
        return GenerationValidationResult::success();
    }

    std::vector<IndexEntry> metadata;
    if (!catalog.read_index_metadata(resolved.id, metadata)) {
        return validation_failure(
            rule.layer_name,
            "attribute index metadata could not be parsed");
    }

    for (const std::string& index_name :
         rule.required_attribute_indexes) {
        if (index_name.empty()) {
            return validation_failure(
                rule.layer_name,
                "required attribute index name is empty");
        }
        if (!contains_index_name(metadata, index_name)) {
            return validation_failure(
                rule.layer_name,
                "required attribute index metadata is missing: " +
                    index_name);
        }

        const CatalogEntry* attribute_index =
            catalog.find_atx(resolved.id, index_name);
        if (attribute_index == nullptr || attribute_index->file_size == 0) {
            return validation_failure(
                rule.layer_name,
                "required attribute index file is missing or empty: " +
                    index_name);
        }

        GdbAttributeIndexParser parser(
            (generation_path / attribute_index->filename).string());
        if (!parser.parse()) {
            return validation_failure(
                rule.layer_name,
                "required attribute index could not be parsed: " +
                    index_name);
        }
    }
    return GenerationValidationResult::success();
}

GenerationValidationResult validate_generation(
    const fs::path& generation_path,
    const QueryEngineGenerationValidationOptions& options) {
    if (options.layers.empty()) {
        return GenerationValidationResult::failure(
            "at least one layer validation rule is required");
    }

    GdbCatalog catalog;
    if (!catalog.scan(generation_path.string())) {
        return GenerationValidationResult::failure(
            "GdbCatalog scan failed for candidate generation");
    }
    if (options.require_directory_magic && !catalog.read_magic()) {
        return GenerationValidationResult::failure(
            "FileGDB directory magic validation failed");
    }

    CatalogResolver resolver(catalog);
    if (!resolver.load()) {
        return GenerationValidationResult::failure(
            "GDB_SystemCatalog could not be reopened");
    }

    for (const GdbLayerValidationRule& rule : options.layers) {
        if (rule.layer_name.empty()) {
            return GenerationValidationResult::failure(
                "layer validation rule has an empty layer name");
        }

        const std::optional<ResolvedTable> resolved =
            resolver.resolve(rule.layer_name);
        if (!resolved) {
            return validation_failure(rule.layer_name,
                                      "layer is missing from system catalog");
        }

        QueryEngine engine(catalog, *resolved);
        if (!engine.open() || engine.table() == nullptr) {
            return validation_failure(rule.layer_name,
                                      "QueryEngine reopen failed");
        }

        GdbTableParser* table = engine.table();
        const uint64_t active_records =
            static_cast<uint64_t>(table->active_feature_count());
        if (rule.expected_active_records &&
            active_records != *rule.expected_active_records) {
            std::ostringstream message;
            message << "active record count mismatch: expected "
                    << *rule.expected_active_records << ", got "
                    << active_records;
            return validation_failure(rule.layer_name, message.str());
        }

        if (rule.scan_all_records) {
            const uint64_t scanned = engine.scan(
                [](uint32_t, const FieldRef*, int) { return true; });
            if (scanned != active_records) {
                std::ostringstream message;
                message << "full record scan mismatch: active="
                        << active_records << ", scanned=" << scanned;
                return validation_failure(rule.layer_name, message.str());
            }
        }

        for (uint32_t fid : rule.sample_fids) {
            const GenerationValidationResult sample =
                validate_sample_fid(rule, engine, *table, fid);
            if (!sample.ok) return sample;
        }

        if (rule.require_spatial_index) {
            const GenerationValidationResult spatial =
                validate_spatial_index(generation_path, rule, *resolved,
                                       catalog);
            if (!spatial.ok) return spatial;
        }

        const GenerationValidationResult attributes =
            validate_attribute_indexes(generation_path, rule, *resolved,
                                       catalog);
        if (!attributes.ok) return attributes;
    }

    return GenerationValidationResult::success();
}

}  // namespace

GenerationValidator make_query_engine_generation_validator(
    QueryEngineGenerationValidationOptions options) {
    return [options = std::move(options)](const fs::path& generation_path) {
        return validate_generation(generation_path, options);
    };
}

}  // namespace writer
}  // namespace explorgdb
