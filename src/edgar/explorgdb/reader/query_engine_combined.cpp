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

constexpr size_t kAtxBypassMaxCandidates = 65536U;
constexpr size_t kAtxBypassRatioDenominator = 8U;

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
    return 0;
}

bool string_index_key_supported(const std::string& value) {
    for (size_t index = 0; index < value.size();) {
        const size_t bytes = utf8_sequence_bytes(
            static_cast<unsigned char>(value[index]));
        // The current .atx UTF-16 decoder handles one code unit at a time and
        // cannot safely reproduce surrogate pairs. Falling back avoids false
        // negatives for non-BMP text.
        if (bytes == 0 || bytes == 4 || index + bytes > value.size())
            return false;
        for (size_t continuation = 1; continuation < bytes; ++continuation) {
            const unsigned char byte = static_cast<unsigned char>(
                value[index + continuation]);
            if ((byte & 0xc0U) != 0x80U) return false;
        }
        index += bytes;
    }
    return true;
}

bool should_bypass_attribute_index(size_t spatial_matches,
                                   size_t active_features) {
    if (spatial_matches == 0 || active_features == 0) return false;
    return spatial_matches <= kAtxBypassMaxCandidates &&
           spatial_matches <=
               active_features / kAtxBypassRatioDenominator;
}

struct AttributeCandidatePlan {
    bool available = false;
    bool used = false;
    std::string atx_path;
    std::vector<uint32_t> fids;
    std::string reason;
    double metadata_ms = 0.0;
    AttributeIndexQueryMetrics index_metrics;
};

AttributeCandidatePlan resolve_attribute_index(
    const GdbCatalog& catalog,
    uint32_t table_id,
    const std::vector<FieldDescriptor>& fields,
    const IndexableWherePredicate& predicate) {
    AttributeCandidatePlan plan;
    const auto metadata_start = CombinedClock::now();
    auto fail = [&](std::string reason) {
        plan.reason = std::move(reason);
        plan.metadata_ms = elapsed_ms(metadata_start);
        return plan;
    };

    if (predicate.field_index >= fields.size()) {
        return fail("attribute field metadata unavailable");
    }
    // The current numeric comparator maps NaN to equality, so indexed !=
    // could exclude a row accepted by the canonical WHERE evaluator. String
    // != is also unsafe with padded/truncated index keys.
    if (predicate.op == AttrOp::Ne) {
        return fail(
            "not-equal is not safely indexable; spatial candidates evaluated");
    }

    const FieldType field_type = fields[predicate.field_index].type;
    if (predicate.is_string) {
        if (field_type != FieldType::String) {
            return fail(
                "attribute index type mismatch; spatial candidates evaluated");
        }
        // OpenFileGDB only treats equality and GE string iterators as safe
        // candidate supersets. Padding/truncation means both still require a
        // final expression recheck.
        if (predicate.op != AttrOp::Eq && predicate.op != AttrOp::Ge) {
            return fail(
                "string operator is not safely indexable; spatial candidates evaluated");
        }
        if (!string_index_key_supported(predicate.string_value)) {
            return fail(
                "string key is not safely representable by the current .atx decoder; spatial candidates evaluated");
        }
    } else if (!numeric_atx_supported(field_type)) {
        return fail(
            "numeric field encoding is not safely indexable; spatial candidates evaluated");
    }

    const CatalogEntry* metadata = catalog.find_indexes(table_id);
    if (metadata == nullptr) {
        return fail(
            "attribute index metadata missing; spatial candidates evaluated");
    }

    std::string index_name;
    bool functional_index_found = false;
    try {
        GdbIndexesParser parser(catalog.path() + "/" + metadata->filename);
        if (!parser.parse()) {
            return fail(
                "attribute index metadata could not be parsed; spatial candidates evaluated");
        }
        const std::string target = lower_copy(predicate.field_name);
        for (const IndexEntry& entry : parser.entries()) {
            const std::string indexed_field =
                GdbIndexesParser::field_name_from_expression(
                    entry.column_name);
            if (lower_copy(indexed_field) != target) continue;
            if (GdbIndexesParser::is_direct_field_expression(
                    entry.column_name)) {
                index_name = entry.name;
                break;
            }
            functional_index_found = true;
        }
    } catch (...) {
        return fail(
            "attribute index metadata could not be parsed; spatial candidates evaluated");
    }

    if (index_name.empty()) {
        return fail(functional_index_found
            ? "functional attribute index is not semantically equivalent to the direct WHERE comparison; spatial candidates evaluated"
            : "attribute field has no .atx index; spatial candidates evaluated");
    }
    const CatalogEntry* atx = catalog.find_atx(table_id, index_name);
    if (atx == nullptr) {
        return fail(
            "attribute index file missing; spatial candidates evaluated");
    }

    plan.available = true;
    plan.atx_path = catalog.path() + "/" + atx->filename;
    plan.metadata_ms = elapsed_ms(metadata_start);
    return plan;
}

bool query_attribute_candidates(
    AttributeCandidatePlan& plan,
    size_t max_fid_count,
    const IndexableWherePredicate& predicate) {
    try {
        GdbAttributeIndexParser index(plan.atx_path);
        const bool ok = predicate.is_string
            ? index.query_string_direct(
                  predicate.string_value, predicate.op,
                  max_fid_count, plan.fids, &plan.index_metrics)
            : index.query_double_direct(
                  predicate.numeric_value, predicate.op,
                  max_fid_count, plan.fids, &plan.index_metrics);
        if (!ok) {
            plan.reason =
                "attribute index could not be parsed; spatial candidates evaluated";
            plan.fids.clear();
            return false;
        }
        plan.used = true;
        return true;
    } catch (...) {
        plan.reason =
            "attribute index could not be parsed; spatial candidates evaluated";
        plan.fids.clear();
        return false;
    }
}

void record_attribute_index_metrics(
    const AttributeCandidatePlan& plan,
    CombinedQueryMetrics& metrics) {
    metrics.attribute_metadata_ms = plan.metadata_ms;
    metrics.attribute_index_page_count = plan.index_metrics.page_count;
    metrics.attribute_index_pages_visited = plan.index_metrics.pages_visited;
    metrics.attribute_index_entries_scanned =
        plan.index_metrics.entries_scanned;
    metrics.attribute_index_file_load_ms =
        plan.index_metrics.file_load_ms;
    metrics.attribute_index_navigation_ms =
        plan.index_metrics.tree_navigation_ms;
    metrics.attribute_index_scan_ms = plan.index_metrics.leaf_scan_ms;
    metrics.attribute_candidate_order_ms =
        plan.index_metrics.candidate_order_ms;
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
        AttributeCandidatePlan attribute = resolve_attribute_index(
            catalog_, resolved_.id, parser_->fields(), *predicate);
        result.combined_metrics.attribute_metadata_ms =
            attribute.metadata_ms;
        result.combined_metrics.attribute_ms += attribute.metadata_ms;

        if (!attribute.available) {
            append_reason(result.fallback_reason, attribute.reason);
        } else if (should_bypass_attribute_index(
                       evaluation_candidates.size(),
                       parser_->active_feature_count())) {
            result.combined_metrics.attribute_index_bypassed = true;
        } else {
            (void)query_attribute_candidates(
                attribute, parser_->feature_count(), *predicate);
            record_attribute_index_metrics(
                attribute, result.combined_metrics);
            result.combined_metrics.attribute_ms +=
                attribute.index_metrics.total_ms;
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

    std::sort(result.matched_fids.begin(), result.matched_fids.end());
    result.matched_fids.erase(
        std::unique(result.matched_fids.begin(), result.matched_fids.end()),
        result.matched_fids.end());
    result.combined_metrics.attribute_recheck_ms =
        elapsed_ms(attribute_eval_start);
    result.combined_metrics.attribute_ms +=
        result.combined_metrics.attribute_recheck_ms;
    result.combined_metrics.final_match_count = result.matched_fids.size();
    result.combined_metrics.total_ms = elapsed_ms(total_start);
    return result;
}

} // namespace explorgdb
