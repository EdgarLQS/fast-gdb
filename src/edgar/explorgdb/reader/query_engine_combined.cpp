#include "query_engine.h"
#include "gdb_indexes.h"
#include "query_where_internal.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace explorgdb {
namespace {

using CombinedClock = std::chrono::steady_clock;

double elapsed_ms(CombinedClock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        CombinedClock::now() - start).count();
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

void append_reason(std::string& target, const std::string& reason) {
    if (reason.empty()) return;
    if (!target.empty()) target += "; ";
    target += reason;
}

bool used_spx_path(const std::string& path) {
    return path.find(":spx-") != std::string::npos;
}

bool numeric_atx_supported(FieldType type) {
    // The current .atx decoder is exact for signed Int32 and double-backed
    // fields. Other physical encodings remain on the candidate row evaluator
    // until the index decoder has explicit type metadata.
    switch (type) {
        case FieldType::Int32:
        case FieldType::Float64:
        case FieldType::DateTime:
        case FieldType::Date:
        case FieldType::Time:
            return true;
        default:
            return false;
    }
}

size_t utf8_sequence_bytes(unsigned char lead) {
    if ((lead & 0x80U) == 0) return 1;
    if ((lead & 0xe0U) == 0xc0U) return 2;
    if ((lead & 0xf0U) == 0xe0U) return 3;
    if ((lead & 0xf8U) == 0xf0U) return 4;
    return 1;
}

std::string truncate_index_string(const std::string& value,
                                  size_t max_utf16_units) {
    std::string result;
    size_t units = 0;
    for (size_t index = 0; index < value.size() && units < max_utf16_units;) {
        if (value[index] == ' ') break;
        const size_t bytes = std::min(
            utf8_sequence_bytes(static_cast<unsigned char>(value[index])),
            value.size() - index);
        const size_t required_units = bytes == 4 ? 2U : 1U;
        if (units + required_units > max_utf16_units) break;
        result.append(value, index, bytes);
        index += bytes;
        units += required_units;
    }
    return result;
}

struct AttributeCandidatePlan {
    bool used = false;
    std::vector<uint32_t> fids;
    std::string reason;
};

AttributeCandidatePlan build_attribute_candidates(
    const GdbCatalog& catalog,
    uint32_t table_id,
    const std::vector<FieldDescriptor>& fields,
    const IndexableWherePredicate& predicate) {
    AttributeCandidatePlan plan;
    if (predicate.field_index >= fields.size()) {
        plan.reason = "attribute field metadata unavailable";
        return plan;
    }

    const CatalogEntry* metadata = catalog.find_indexes(table_id);
    if (metadata == nullptr) {
        plan.reason = "attribute index metadata missing; spatial candidates evaluated";
        return plan;
    }

    std::string index_name;
    try {
        GdbIndexesParser parser(catalog.path() + "/" + metadata->filename);
        if (!parser.parse()) {
            plan.reason = "attribute index metadata could not be parsed; spatial candidates evaluated";
            return plan;
        }
        const std::string target = lower_copy(predicate.field_name);
        for (const IndexEntry& entry : parser.entries()) {
            if (lower_copy(entry.column_name) == target) {
                index_name = entry.name;
                break;
            }
        }
    } catch (...) {
        plan.reason = "attribute index metadata could not be parsed; spatial candidates evaluated";
        return plan;
    }

    if (index_name.empty()) {
        plan.reason = "attribute field has no .atx index; spatial candidates evaluated";
        return plan;
    }
    const CatalogEntry* atx = catalog.find_atx(table_id, index_name);
    if (atx == nullptr) {
        plan.reason = "attribute index file missing; spatial candidates evaluated";
        return plan;
    }

    try {
        GdbAttributeIndexParser index(catalog.path() + "/" + atx->filename);
        if (!index.parse()) {
            plan.reason = "attribute index could not be parsed; spatial candidates evaluated";
            return plan;
        }

        const FieldType field_type = fields[predicate.field_index].type;
        if (predicate.is_string) {
            if (!index.trailer().is_string) {
                plan.reason = "attribute index type mismatch; spatial candidates evaluated";
                return plan;
            }
            // OpenFileGDB only treats equality and GE string iterators as safe
            // candidate supersets. Padding/truncation means both still require
            // a final expression recheck.
            if (predicate.op != AttrOp::Eq && predicate.op != AttrOp::Ge) {
                plan.reason = "string operator is not safely indexable; spatial candidates evaluated";
                return plan;
            }
            const size_t max_units = index.trailer().value_size / 2U;
            const std::string key = truncate_index_string(
                predicate.string_value, max_units);
            plan.fids = index.query_string(key, predicate.op);
        } else {
            if (!index.trailer().is_numeric ||
                !numeric_atx_supported(field_type)) {
                plan.reason = "numeric field encoding is not safely indexable; spatial candidates evaluated";
                return plan;
            }
            plan.fids = index.query_double(
                predicate.numeric_value, predicate.op);
        }
        plan.used = true;
        return plan;
    } catch (...) {
        plan.reason = "attribute index could not be parsed; spatial candidates evaluated";
        return plan;
    }
}

} // namespace

QueryResult QueryEngine::query_spatial_where(const QueryRequest& request) {
    QueryResult result;
    const auto total_start = CombinedClock::now();
    result.execution_path = "spatial-where:invalid";

    if (!parser_) {
        result.fallback_reason = "table not open";
        result.combined_metrics.total_ms = elapsed_ms(total_start);
        return result;
    }
    if (!std::isfinite(request.xmin) || !std::isfinite(request.ymin) ||
        !std::isfinite(request.xmax) || !std::isfinite(request.ymax) ||
        request.xmin > request.xmax || request.ymin > request.ymax) {
        result.fallback_reason = "invalid query bbox";
        result.combined_metrics.total_ms = elapsed_ms(total_start);
        return result;
    }

    const CompiledWhere expression = compile_where(
        request.where_clause, parser_->fields());
    if (!expression.valid()) {
        result.fallback_reason = expression.error();
        result.combined_metrics.total_ms = elapsed_ms(total_start);
        return result;
    }

    const auto spatial_start = CombinedClock::now();
    QueryResult spatial = query_bbox_unified(
        request.xmin, request.ymin, request.xmax, request.ymax);
    result.combined_metrics.spatial_ms = elapsed_ms(spatial_start);
    result.spatial_metrics = spatial.spatial_metrics;
    result.combined_metrics.spatial_candidate_count =
        spatial.spatial_metrics.candidate_count;
    result.combined_metrics.spatial_match_count = spatial.matched_fids.size();
    result.combined_metrics.used_spatial_index =
        used_spx_path(spatial.execution_path);
    append_reason(result.fallback_reason, spatial.fallback_reason);

    if (spatial.execution_path == "bbox:model:invalid") {
        result.fallback_reason = "invalid query bbox";
        result.combined_metrics.total_ms = elapsed_ms(total_start);
        return result;
    }
    if (spatial.execution_path == "bbox:model:unavailable") {
        result.fallback_reason = spatial.fallback_reason;
        result.combined_metrics.total_ms = elapsed_ms(total_start);
        return result;
    }

    std::vector<uint32_t> evaluation_candidates =
        std::move(spatial.matched_fids);
    std::sort(evaluation_candidates.begin(), evaluation_candidates.end());
    evaluation_candidates.erase(
        std::unique(evaluation_candidates.begin(), evaluation_candidates.end()),
        evaluation_candidates.end());

    bool attribute_index_used = false;
    const auto predicate = expression.indexable_predicate();
    if (!evaluation_candidates.empty() && predicate.has_value()) {
        const auto attribute_index_start = CombinedClock::now();
        AttributeCandidatePlan attribute = build_attribute_candidates(
            catalog_, resolved_.id, parser_->fields(), *predicate);
        result.combined_metrics.attribute_ms +=
            elapsed_ms(attribute_index_start);
        if (attribute.used) {
            attribute_index_used = true;
            result.combined_metrics.used_attribute_index = true;
            result.combined_metrics.attribute_candidate_count =
                attribute.fids.size();
            const auto intersection_start = CombinedClock::now();
            evaluation_candidates = intersect_sorted_fids(
                evaluation_candidates, attribute.fids);
            result.combined_metrics.intersection_ms =
                elapsed_ms(intersection_start);
        } else {
            append_reason(result.fallback_reason, attribute.reason);
        }
    }

    if (attribute_index_used &&
        result.combined_metrics.used_spatial_index) {
        result.execution_path = "spatial-where:spx+atx";
    } else if (result.combined_metrics.used_spatial_index) {
        result.execution_path = "spatial-where:spatial-candidates";
    } else {
        result.execution_path = "spatial-where:sequential";
    }

    if (evaluation_candidates.empty()) {
        result.combined_metrics.final_match_count = 0;
        result.combined_metrics.total_ms = elapsed_ms(total_start);
        return result;
    }

    const auto attribute_eval_start = CombinedClock::now();
    result.matched_fids.reserve(evaluation_candidates.size());
    const uint64_t scanned = parser_->scan_field_candidates(
        evaluation_candidates,
        [&](uint32_t fid, const FieldRef* fields, int field_count) {
            ++result.combined_metrics.attribute_tested;
            if (evaluate_where(expression, fields, field_count))
                result.matched_fids.push_back(fid);
            return true;
        });

    if (scanned != evaluation_candidates.size()) {
        result.matched_fids.clear();
        result.combined_metrics.attribute_tested = 0;
        append_reason(result.fallback_reason,
                      "candidate field scan unavailable; canonical row reads used");
        for (uint32_t fid : evaluation_candidates) {
            FeatureRecord record;
            if (!parser_->read_record_by_fid(fid, record)) continue;
            ++result.combined_metrics.attribute_tested;
            if (evaluate_where(expression, record))
                result.matched_fids.push_back(fid);
        }
    }
    result.combined_metrics.attribute_ms += elapsed_ms(attribute_eval_start);

    std::sort(result.matched_fids.begin(), result.matched_fids.end());
    result.matched_fids.erase(
        std::unique(result.matched_fids.begin(), result.matched_fids.end()),
        result.matched_fids.end());
    result.combined_metrics.final_match_count = result.matched_fids.size();
    result.combined_metrics.total_ms = elapsed_ms(total_start);
    return result;
}

} // namespace explorgdb
