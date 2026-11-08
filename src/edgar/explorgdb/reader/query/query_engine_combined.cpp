// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

#include "query_engine.h"
#include "gdb_indexes.h"
#include "gdb_attribute_value_cache.h"
#include "query_where_internal.h"
#include "spatial_predicate.h"
#include "explorgdb_constants.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <limits>
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
        case FieldType::Int16:
        case FieldType::Float32:
        case FieldType::Int32:
        case FieldType::Int64:
        case FieldType::Float64:
        case FieldType::DateTime:
        case FieldType::Date:
        case FieldType::Time:
            return true;
        default:
            return false;
    }
}

bool is_exactly_representable_as_double(int64_t value) {
    const double converted = static_cast<double>(value);
    constexpr double kInt64Min = -0x1p63;
    constexpr double kInt64UpperBound = 0x1p63;
    if (converted < kInt64Min || converted >= kInt64UpperBound)
        return false;
    return static_cast<int64_t>(converted) == value;
}

bool should_bypass_attribute_index(size_t spatial_matches,
                                   size_t active_features) {
    if (spatial_matches == 0 || active_features == 0) return false;
    return spatial_matches <= kAtxBypassMaxCandidates &&
           spatial_matches <=
               active_features / kAtxBypassRatioDenominator;
}

bool bbox_disjoint(const GdbBbox& bounds,
                   double xmin, double ymin,
                   double xmax, double ymax) {
    return bounds.xmax < xmin || bounds.xmin > xmax ||
           bounds.ymax < ymin || bounds.ymin > ymax;
}

bool bbox_contained_by_query(const GdbBbox& bounds,
                             double xmin, double ymin,
                             double xmax, double ymax) {
    return bounds.xmin >= xmin && bounds.ymin >= ymin &&
           bounds.xmax <= xmax && bounds.ymax <= ymax;
}

double query_coverage_ratio(const FieldDescriptor& field,
                            double xmin, double ymin,
                            double xmax, double ymax) {
    const double width = field.xmax - field.xmin;
    const double height = field.ymax - field.ymin;
    if (!std::isfinite(width) || !std::isfinite(height) ||
        width <= 0.0 || height <= 0.0) {
        return 0.0;
    }

    const double overlap_xmin = std::max(xmin, field.xmin);
    const double overlap_ymin = std::max(ymin, field.ymin);
    const double overlap_xmax = std::min(xmax, field.xmax);
    const double overlap_ymax = std::min(ymax, field.ymax);
    if (overlap_xmax <= overlap_xmin || overlap_ymax <= overlap_ymin)
        return 0.0;

    const long double layer_area =
        static_cast<long double>(width) * static_cast<long double>(height);
    const long double overlap_area =
        static_cast<long double>(overlap_xmax - overlap_xmin) *
        static_cast<long double>(overlap_ymax - overlap_ymin);
    const long double ratio = overlap_area / layer_area;
    if (!std::isfinite(static_cast<double>(ratio))) return 0.0;
    return std::clamp(static_cast<double>(ratio), 0.0, 1.0);
}

enum class SpatialRefDecision {
    Reject,
    Match,
    Invalid
};

SpatialRefDecision evaluate_spatial_ref(
    const FieldRef& geometry,
    GdbGeomDecoder& decoder,
    double xmin, double ymin,
    double xmax, double ymax,
    SpatialQueryMetrics& metrics) {
    if (geometry.is_null || geometry.data == nullptr ||
        geometry.byte_len == 0) {
        ++metrics.invalid_geometries;
        return SpatialRefDecision::Reject;
    }

    const std::optional<GdbBbox> bounds = decoder.peek_bbox(
        geometry.data, geometry.byte_len);
    if (bounds.has_value()) {
        if (bbox_disjoint(*bounds, xmin, ymin, xmax, ymax)) {
            ++metrics.bbox_rejected;
            return SpatialRefDecision::Reject;
        }
        if (bbox_contained_by_query(*bounds, xmin, ymin, xmax, ymax)) {
            ++metrics.bbox_contained;
            return SpatialRefDecision::Match;
        }
    }

    ++metrics.exact_tested;
    GeometryModel model = decoder.decode_model(
        geometry.data, geometry.byte_len);
    if (!model.valid()) {
        ++metrics.invalid_geometries;
        return model.status == GeometryStatus::Empty || model.status == GeometryStatus::Null
            ? SpatialRefDecision::Reject : SpatialRefDecision::Invalid;
    }

    const long double scale = model.transform.xy_scale;
    if (scale == 0.0L) {
        ++metrics.invalid_geometries;
        return SpatialRefDecision::Invalid;
    }

    QueryGridBbox query = QueryGridBbox::from_world(
        model.transform, xmin, ymin, xmax, ymax);
    if (!std::isfinite(static_cast<double>(query.xmin)) ||
        !std::isfinite(static_cast<double>(query.ymin)) ||
        !std::isfinite(static_cast<double>(query.xmax)) ||
        !std::isfinite(static_cast<double>(query.ymax))) {
        ++metrics.invalid_geometries;
        return SpatialRefDecision::Invalid;
    }

    return SpatialPredicate::intersects_bbox(model, query)
        ? SpatialRefDecision::Match
        : SpatialRefDecision::Reject;
}

struct AttributeCandidatePlan {
    bool available = false;
    bool used = false;
    std::string atx_path;
    std::vector<RowSlot> fids;
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
    const FieldType field_type = fields[predicate.field_index].type;
    const bool uuid_field = field_type == FieldType::UUID_1 || field_type == FieldType::UUID_2;
    // Numeric NaN and padded/truncated string keys make != candidates unsafe.
    // Fixed canonical GUID keys do not have those ambiguities.
    if (predicate.op == AttrOp::Ne && !uuid_field) {
        return fail(
            "not-equal is not safely indexable; spatial candidates evaluated");
    }

    if (predicate.is_string) {
        if (field_type != FieldType::String && !uuid_field) {
            return fail(
                "attribute index type mismatch; spatial candidates evaluated");
        }
    } else if (!numeric_atx_supported(field_type)) {
        return fail(
            "numeric field encoding is not safely indexable; spatial candidates evaluated");
    } else if (field_type == FieldType::Int64 &&
               (!predicate.numeric_is_integer ||
                predicate.numeric_integer_overflow)) {
        return fail(
            "INT64 index requires an in-range integer literal; spatial candidates evaluated");
    } else if ((field_type == FieldType::Float32 ||
                field_type == FieldType::Float64 ||
                field_type == FieldType::DateTime ||
                field_type == FieldType::Date ||
                field_type == FieldType::Time) &&
               predicate.numeric_is_integer &&
               !predicate.numeric_integer_overflow &&
               !is_exactly_representable_as_double(
                   predicate.integer_value)) {
        return fail(
            "integer literal is not exactly representable by the indexed floating field; spatial candidates evaluated");
    }

    if (catalog.find_indexes(table_id) == nullptr) {
        return fail(
            "attribute index metadata missing; spatial candidates evaluated");
    }

    std::vector<IndexEntry> metadata_entries;
    if (!catalog.read_index_metadata(table_id, metadata_entries)) {
        return fail(
            "attribute index metadata could not be parsed; spatial candidates evaluated");
    }

    std::string index_name;
    bool functional_index_found = false;
    const std::string target = lower_copy(predicate.field_name);
    for (const IndexEntry& entry : metadata_entries) {
        const std::string indexed_field =
            GdbIndexesParser::field_name_from_expression(entry.column_name);
        if (lower_copy(indexed_field) != target) continue;
        if (GdbIndexesParser::is_direct_field_expression(
                entry.column_name)) {
            index_name = entry.name;
            break;
        }
        functional_index_found = true;
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
    const IndexableWherePredicate& predicate,
    std::shared_ptr<const detail::AttributeValueQueryContext> value_context) {
    if (!value_context) {
        plan.reason = "attribute source unavailable; spatial candidates evaluated";
        return false;
    }
    try {
        GdbAttributeIndexParser index(plan.atx_path, predicate.field_type);
        index.set_value_context(std::move(value_context));
        bool ok = false;
        if (predicate.temporal_milliseconds.has_value()) {
            ok = index.query_temporal_direct(
                *predicate.temporal_milliseconds, predicate.op,
                max_fid_count, plan.fids, &plan.index_metrics);
        } else if (predicate.is_string) {
            ok = predicate.field_type == FieldType::String
                ? index.query_string_candidates(predicate.string_value, predicate.op,
                    max_fid_count, plan.fids, &plan.index_metrics)
                : index.query_string_direct(predicate.string_value, predicate.op,
                    max_fid_count, plan.fids, &plan.index_metrics);
        } else if (predicate.field_type == FieldType::Int64) {
            ok = index.query_int64_direct(
                predicate.integer_value, predicate.op,
                max_fid_count, plan.fids, &plan.index_metrics);
        } else {
            ok = index.query_double_direct(
                predicate.numeric_value, predicate.op,
                max_fid_count, plan.fids, &plan.index_metrics);
        }
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
    metrics.attribute_validated_directory_hit = plan.index_metrics.validated_directory_hit;
    metrics.attribute_index_validation_rows = plan.index_metrics.index_validation_rows;
    metrics.attribute_validated_directory_bytes = plan.index_metrics.validated_directory_bytes;
    metrics.attribute_string_directory_hit = plan.index_metrics.string_directory_hit;
    metrics.attribute_string_validation_rows = plan.index_metrics.string_validation_rows;
    metrics.attribute_string_directory_bytes = plan.index_metrics.string_directory_bytes;
}

} // namespace

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
QueryResult QueryEngine::query_spatial_where(const QueryRequest& request) {
    QueryResult result;
    const auto total_start = CombinedClock::now();
    result.execution_path = kPathSpatialWhereInvalid;

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

    const CompiledWhere expression = prepare_where(
        request.where_clause, &result.query_preparation_cache_hit);
    if (!expression.valid()) {
        result.fallback_reason = expression.error();
        result.combined_metrics.total_ms = elapsed_ms(total_start);
        return result;
    }

    const auto predicate = expression.indexable_predicate();

    // Selective SPX candidates can be spatially checked and WHERE-rechecked
    // from one parsed row. This removes the old geometry pass followed by a
    // second nullable/layout/field pass. All output remains local until the
    // complete candidate scan succeeds; any failure falls through unchanged.
    if (predicate.has_value()) {
        const FieldDescriptor* geom_field = geometry_field();
        const size_t feature_count = parser_->active_feature_count();
        const RowSlot fid_slot_count = parser_->feature_count();
        size_t geometry_field_index = parser_->fields().size();
        for (size_t index = 0; index < parser_->fields().size(); ++index) {
            if (parser_->fields()[index].type == FieldType::Geometry) {
                geometry_field_index = index;
                break;
            }
        }

        const double estimated_coverage = geom_field == nullptr
            ? 1.0
            : query_coverage_ratio(
                  *geom_field,
                  request.xmin, request.ymin,
                  request.xmax, request.ymax);
        const bool spx_extent_supported = geom_field != nullptr &&
            GdbSpatialIndexParser::supports_layer_extent(
                geom_field->xmin, geom_field->ymin,
                geom_field->xmax, geom_field->ymax,
                geom_field->grid_sizes);
        const bool fused_shape_supported =
            geom_field != nullptr && feature_count != 0 &&
            fid_slot_count != 0 &&
            geometry_field_index < parser_->fields().size() &&
            estimated_coverage <= kFusedCoverageThreshold &&
            spx_extent_supported &&
            resolved_.has_spatial_index != false;

        if (fused_shape_supported) {
            AttributeCandidatePlan attribute = resolve_attribute_index(
                catalog_, resolved_.id, parser_->fields(), *predicate);
            if (attribute.available) {
                const auto candidate_start = CombinedClock::now();
                if (!spatial_index_initialized_) {
                    spatial_index_initialized_ = true;
                    const CatalogEntry* spx = catalog_.find_spx(resolved_.id);
                    spatial_index_present_ =
                        spx != nullptr && resolved_.has_spatial_index != false;
                    if (spatial_index_present_) {
                        auto index = std::make_unique<GdbSpatialIndexParser>(
                            catalog_.path() + "/" + spx->filename);
                        if (index->parse()) {
                            spatial_index_ = std::move(index);
                        } else {
                            capabilities_.spatial_index = {
                                CapabilityState::Degraded,
                                ".spx exists but could not be parsed; "
                                "falling back to sequential model filtering"};
                        }
                    }
                }

                if (spatial_index_ != nullptr) {
                    const bool merge_x_ranges =
                        request.ymin <= geom_field->ymin &&
                        request.ymax >= geom_field->ymax;
                    std::vector<RowSlot> candidates =
                        spatial_index_->query_bbox(
                            request.xmin, request.ymin,
                            request.xmax, request.ymax,
                            geom_field->xorig, geom_field->yorig,
                            geom_field->xyscale, geom_field->grid_sizes,
                            fid_slot_count - 1U,
                            merge_x_ranges);
                    const double candidate_lookup_ms =
                        elapsed_ms(candidate_start);

                    if (!candidates.empty() &&
                        should_bypass_attribute_index(
                            candidates.size(), feature_count)) {
                        QueryResult fused;
                        fused.execution_path =
                            kPathSpatialWhereSpatialCandidates;
                        fused.combined_metrics.used_spatial_index = true;
                        fused.combined_metrics.attribute_index_bypassed = true;
                        fused.combined_metrics.fused_spatial_attribute_scan =
                            true;
                        fused.combined_metrics.spatial_candidate_count =
                            candidates.size();
                        fused.combined_metrics.fused_candidate_count =
                            candidates.size();
                        fused.combined_metrics.attribute_metadata_ms =
                            attribute.metadata_ms;
                        fused.combined_metrics.attribute_ms =
                            attribute.metadata_ms;

                        fused.spatial_metrics.feature_count = feature_count;
                        fused.spatial_metrics.candidate_count =
                            candidates.size();
                        fused.spatial_metrics.candidate_ratio =
                            static_cast<double>(candidates.size()) /
                            static_cast<double>(feature_count);
                        fused.spatial_metrics.estimated_coverage =
                            estimated_coverage;
                        fused.spatial_metrics.candidate_lookup_ms =
                            candidate_lookup_ms;

                        const bool has_z =
                            ((parser_->header().geom_type_full >> 24U) &
                             (1U << 7U)) != 0;
                        const bool has_m =
                            ((parser_->header().geom_type_full >> 24U) &
                             (1U << 6U)) != 0;
                        GdbGeomDecoder decoder(
                            geom_field->xorig, geom_field->yorig,
                            geom_field->xyscale,
                            geom_field->zorig, geom_field->zscale,
                            geom_field->morig, geom_field->mscale,
                            has_z, has_m);

                        const auto scan_start = CombinedClock::now();
                        fused.matched_fids.reserve(candidates.size());
                        const uint64_t scanned =
                            parser_->scan_field_candidates(
                                candidates,
                                [&](RowSlot fid,
                                    const FieldRef* fields,
                                    int field_count) {
                                    if (fields == nullptr ||
                                        geometry_field_index >=
                                            static_cast<size_t>(field_count)) {
                                        return false;
                                    }
                                    const SpatialRefDecision decision =
                                        evaluate_spatial_ref(
                                            fields[geometry_field_index],
                                            decoder,
                                            request.xmin, request.ymin,
                                            request.xmax, request.ymax,
                                            fused.spatial_metrics);
                                    if (decision == SpatialRefDecision::Invalid) {
                                        fused.status = QueryStatus::Unsupported;
                                        fused.error = "source geometry predicate could not be evaluated completely";
                                        return false;
                                    }
                                    if (decision != SpatialRefDecision::Match)
                                        return true;

                                    ++fused.combined_metrics.spatial_match_count;
                                    ++fused.combined_metrics.attribute_tested;
                                    if (evaluate_where(
                                            expression, fields, field_count)) {
                                        fused.matched_fids.push_back(fid);
                                    }
                                    return true;
                                });
                        const double fused_scan_ms = elapsed_ms(scan_start);

                        if (fused.status != QueryStatus::Ok) {
                            fused.matched_fids.clear();
                            fused.combined_metrics.total_ms = elapsed_ms(total_start);
                            return fused;
                        }

                        if (scanned == candidates.size()) {
                            std::sort(fused.matched_fids.begin(),
                                      fused.matched_fids.end());
                            fused.matched_fids.erase(
                                std::unique(fused.matched_fids.begin(),
                                            fused.matched_fids.end()),
                                fused.matched_fids.end());
                            fused.combined_metrics.final_match_count =
                                fused.matched_fids.size();
                            fused.combined_metrics.fused_candidate_scan_ms =
                                fused_scan_ms;
                            fused.combined_metrics.spatial_ms =
                                candidate_lookup_ms + fused_scan_ms;
                            fused.spatial_metrics.geometry_scan_ms =
                                fused_scan_ms;
                            fused.spatial_metrics.total_ms =
                                candidate_lookup_ms + fused_scan_ms;
                            fused.combined_metrics.total_ms =
                                elapsed_ms(total_start);

                            if (fused.spatial_metrics.invalid_geometries != 0) {
                                fused.fallback_reason =
                                    std::to_string(
                                        fused.spatial_metrics.invalid_geometries) +
                                    " candidate geometries had explicit "
                                    "decode/topology errors";
                            }
                            return fused;
                        }
                    }
                }
            }
        }
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

    if (spatial.status != QueryStatus::Ok) {
        result.status = spatial.status;
        result.error = spatial.error;
        result.execution_path = kPathSpatialWhereSequential;
        result.combined_metrics.total_ms = elapsed_ms(total_start);
        return result;
    }
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

    std::vector<RowSlot> evaluation_candidates =
        std::move(spatial.matched_fids);
    std::sort(evaluation_candidates.begin(), evaluation_candidates.end());
    evaluation_candidates.erase(
        std::unique(evaluation_candidates.begin(), evaluation_candidates.end()),
        evaluation_candidates.end());

    bool attribute_index_used = false;
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
                attribute, parser_->feature_count(), *predicate,
                make_value_context(predicate->field_index, predicate->temporal_milliseconds.has_value()));
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
        result.execution_path = kPathSpatialWhereSpxAtx;
    } else if (result.combined_metrics.used_spatial_index) {
        result.execution_path = kPathSpatialWhereSpatialCandidates;
    } else {
        result.execution_path = kPathSpatialWhereSequential;
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
        [&](RowSlot fid, const FieldRef* fields, int field_count) {
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
        for (RowSlot fid : evaluation_candidates) {
            FeatureRecord record;
            if (!parser_->read_record_by_fid(fid, record)) {
                result.status = QueryStatus::Unsupported;
                result.error = "attribute candidate could not be read completely";
                result.matched_fids.clear();
                result.combined_metrics.total_ms = elapsed_ms(total_start);
                return result;
            }
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
