// Attribute-index plan benchmark: index candidates vs a full field scan.
//
// Gated by FAST_GDB_RUN_ATTRIBUTE_BENCHMARKS=1. Required environment:
//   FAST_GDB_ATTR_BENCHMARK_PATH    .gdb directory
//   FAST_GDB_ATTR_BENCHMARK_LAYER   layer name
//   FAST_GDB_ATTR_BENCHMARK_FIELD   indexed field name (column name match)
//   FAST_GDB_ATTR_BENCHMARK_VALUE   predicate value
// Optional:
//   FAST_GDB_ATTR_BENCHMARK_OP      = != < <= > >= (default =)
//   FAST_GDB_ATTR_BENCHMARK_TRIALS  trials (default 5, max 100)
//
// Measures a cold path (fresh open + first query per trial) and a warm path
// (one open, one warm-up query, repeated trials), verifies the matched FIDs
// against an independent GDAL full scan, and reports the execution path plus
// candidate/pages metrics so plan selection can be compared across values.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <sys/resource.h>

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include "catalog_resolver.h"
#include "gdb_catalog.h"
#include "query_engine.h"

using Clock = std::chrono::steady_clock;
using namespace explorgdb;

namespace {

std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

bool env_enabled(const char* name) { return env_value(name) == "1"; }

int trial_count() {
    const std::string text = env_value("FAST_GDB_ATTR_BENCHMARK_TRIALS");
    if (text.empty()) return 5;
    char* end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || parsed < 1 || parsed > 100) return 5;
    return static_cast<int>(parsed);
}

bool parse_op(const std::string& text, AttrOp& op) {
    if (text.empty() || text == "=") { op = AttrOp::Eq; return true; }
    if (text == "!=") { op = AttrOp::Ne; return true; }
    if (text == "<") { op = AttrOp::Lt; return true; }
    if (text == "<=") { op = AttrOp::Le; return true; }
    if (text == ">") { op = AttrOp::Gt; return true; }
    if (text == ">=") { op = AttrOp::Ge; return true; }
    return false;
}

const char* op_text(AttrOp op) {
    switch (op) {
        case AttrOp::Eq: return "=";
        case AttrOp::Ne: return "!=";
        case AttrOp::Lt: return "<";
        case AttrOp::Le: return "<=";
        case AttrOp::Gt: return ">";
        case AttrOp::Ge: return ">=";
    }
    return "=";
}

bool compare_int(int64_t left, int64_t right, AttrOp op) {
    switch (op) {
        case AttrOp::Eq: return left == right;
        case AttrOp::Ne: return left != right;
        case AttrOp::Lt: return left < right;
        case AttrOp::Le: return left <= right;
        case AttrOp::Gt: return left > right;
        case AttrOp::Ge: return left >= right;
    }
    return false;
}

bool compare_double(double left, double right, AttrOp op) {
    switch (op) {
        case AttrOp::Eq: return left == right;
        case AttrOp::Ne: return left != right;
        case AttrOp::Lt: return left < right;
        case AttrOp::Le: return left <= right;
        case AttrOp::Gt: return left > right;
        case AttrOp::Ge: return left >= right;
    }
    return false;
}

bool compare_string(const std::string& left, const std::string& right, AttrOp op) {
    switch (op) {
        case AttrOp::Eq: return left == right;
        case AttrOp::Ne: return left != right;
        case AttrOp::Lt: return left < right;
        case AttrOp::Le: return left <= right;
        case AttrOp::Gt: return left > right;
        case AttrOp::Ge: return left >= right;
    }
    return false;
}

bool same_field_name(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(left[i]);
        const unsigned char b = static_cast<unsigned char>(right[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

uint64_t peak_rss_bytes() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss);
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024U;
#endif
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t index = static_cast<size_t>(
        std::ceil(fraction * static_cast<double>(values.size())) - 1.0);
    return values[std::min(index, values.size() - 1)];
}

struct Probe {
    std::string path, layer, field, value, op_text;
    AttrOp op = AttrOp::Eq;
    size_t field_index = 0;
    FieldType field_type = FieldType::Int32;
    std::string index_name;
    bool string_field = false;
    bool int64_field = false;
    double double_value = 0.0;
    int64_t int_value = 0;
};

std::optional<Probe> load_probe() {
    Probe probe;
    probe.path = env_value("FAST_GDB_ATTR_BENCHMARK_PATH");
    probe.layer = env_value("FAST_GDB_ATTR_BENCHMARK_LAYER");
    probe.field = env_value("FAST_GDB_ATTR_BENCHMARK_FIELD");
    probe.value = env_value("FAST_GDB_ATTR_BENCHMARK_VALUE");
    probe.op_text = env_value("FAST_GDB_ATTR_BENCHMARK_OP");
    if (probe.path.empty() || probe.layer.empty() || probe.field.empty()) return std::nullopt;
    if (!parse_op(probe.op_text, probe.op)) return std::nullopt;
    probe.op_text = op_text(probe.op);
    return probe;
}

std::unique_ptr<GdbCatalog> open_catalog(const std::string& path) {
    auto catalog = std::make_unique<GdbCatalog>();
    if (!catalog->scan(path)) return nullptr;
    return catalog;
}

struct EngineState {
    std::unique_ptr<GdbCatalog> catalog;
    std::unique_ptr<QueryEngine> engine;
    uint32_t table_id = 0;
};

std::unique_ptr<EngineState> open_engine(const std::string& path, const std::string& layer) {
    auto state = std::make_unique<EngineState>();
    state->catalog = open_catalog(path);
    if (!state->catalog) return nullptr;
    CatalogResolver resolver(*state->catalog);
    if (!resolver.load()) return nullptr;
    const auto resolved = resolver.resolve(layer);
    if (!resolved) return nullptr;
    state->table_id = resolved->id;
    state->engine = std::make_unique<QueryEngine>(*state->catalog, *resolved);
    if (!state->engine->open()) return nullptr;
    return state;
}

// Resolve the field position, its type, and the direct index whose .atx exists.
bool resolve_probe(EngineState& state, Probe& probe) {
    const auto& fields = state.engine->table()->fields();
    for (size_t i = 0; i < fields.size(); ++i) {
        if (same_field_name(fields[i].name, probe.field)) {
            probe.field_index = i;
            probe.field_type = fields[i].type;
            break;
        }
    }
    if (probe.field_index >= fields.size() ||
        !same_field_name(fields[probe.field_index].name, probe.field))
        return false;
    probe.string_field = probe.field_type == FieldType::String;
    probe.int64_field = probe.field_type == FieldType::Int64;
    if (probe.string_field) {
        return true;
    }
    if (probe.int64_field) {
        char* end = nullptr;
        probe.int_value = std::strtoll(probe.value.c_str(), &end, 10);
        if (end == nullptr || *end != '\0') return false;
        probe.double_value = static_cast<double>(probe.int_value);
        return true;
    }
    char* end = nullptr;
    probe.double_value = std::strtod(probe.value.c_str(), &end);
    return end != nullptr && *end == '\0';
}

bool resolve_index(GdbCatalog& catalog, uint32_t table_id, Probe& probe) {
    std::vector<IndexEntry> metadata;
    if (!catalog.read_index_metadata(table_id, metadata)) return false;
    for (const auto& entry : metadata) {
        if (!same_field_name(entry.column_name, probe.field)) continue;
        if (catalog.find_atx(table_id, entry.name) == nullptr) continue;
        probe.index_name = entry.name;
        return true;
    }
    return false;
}

QueryResult run_query(QueryEngine& engine, const Probe& probe) {
    QueryRequest request;
    request.index_name = probe.index_name;
    request.attr_op = probe.op;
    if (probe.string_field) {
        request.kind = QueryKind::AttributeString;
        request.string_value = probe.value;
        return engine.query(request);
    }
    if (probe.int64_field)
        return engine.query_attribute_int64(probe.index_name, probe.int_value, probe.op);
    request.kind = QueryKind::AttributeDouble;
    request.double_value = probe.double_value;
    return engine.query(request);
}

std::string where_clause(const Probe& probe) {
    std::string clause = probe.field + " " + probe.op_text + " ";
    if (probe.string_field) {
        std::string escaped;
        for (char character : probe.value) {
            if (character == '\'') escaped += "''";
            else escaped += character;
        }
        clause += "'" + escaped + "'";
    } else {
        clause += probe.value;
    }
    return clause;
}

// Independent truth: GDAL full scan using the same comparison semantics.
std::vector<RowSlot> gdal_truth(const std::string& path, const std::string& layer,
                                const Probe& probe) {
    std::vector<RowSlot> fids;
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    if (dataset == nullptr) return fids;
    OGRLayer* source = dataset->GetLayerByName(layer.c_str());
    if (source == nullptr) { GDALClose(dataset); return fids; }
    const int gdal_field = source->GetLayerDefn()->GetFieldIndex(probe.field.c_str());
    if (gdal_field < 0) { GDALClose(dataset); return fids; }
    source->ResetReading();
    while (OGRFeature* feature = source->GetNextFeature()) {
        bool matched = false;
        if (!feature->IsFieldSetAndNotNull(gdal_field)) {
            matched = false;
        } else if (probe.string_field) {
            matched = compare_string(feature->GetFieldAsString(gdal_field), probe.value, probe.op);
        } else if (probe.int64_field) {
            matched = compare_int(feature->GetFieldAsInteger64(gdal_field), probe.int_value, probe.op);
        } else {
            matched = compare_double(feature->GetFieldAsDouble(gdal_field), probe.double_value,
                                     probe.op);
        }
        if (matched) {
            const GIntBig fid = feature->GetFID();
            if (fid >= 1) fids.push_back(static_cast<RowSlot>(fid - 1));
        }
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);
    std::sort(fids.begin(), fids.end());
    fids.erase(std::unique(fids.begin(), fids.end()), fids.end());
    return fids;
}

} // namespace

TEST(AttributePlanBenchmark, ConfiguredPredicate) {
    if (!env_enabled("FAST_GDB_RUN_ATTRIBUTE_BENCHMARKS"))
        GTEST_SKIP() << "set FAST_GDB_RUN_ATTRIBUTE_BENCHMARKS=1 and the "
                        "FAST_GDB_ATTR_BENCHMARK_* variables";
    auto probe = load_probe();
    if (!probe) GTEST_SKIP() << "missing or invalid FAST_GDB_ATTR_BENCHMARK_* variables";
    GDALAllRegister();

    auto cold_state = open_engine(probe->path, probe->layer);
    ASSERT_NE(cold_state, nullptr) << "cannot open " << probe->path << "/" << probe->layer;
    ASSERT_TRUE(resolve_probe(*cold_state, *probe)) << "unknown or invalid field " << probe->field;
    ASSERT_TRUE(resolve_index(*cold_state->catalog, cold_state->table_id, *probe))
        << "no usable .atx for field " << probe->field;

    const size_t trials = static_cast<size_t>(trial_count());
    std::vector<double> cold_ms, warm_ms;
    QueryResult cold_result;
    for (size_t trial = 0; trial < trials; ++trial) {
        const auto start = Clock::now();
        auto state = open_engine(probe->path, probe->layer);
        ASSERT_NE(state, nullptr);
        Probe current = *probe;
        ASSERT_TRUE(resolve_probe(*state, current));
        ASSERT_TRUE(resolve_index(*state->catalog, state->table_id, current));
        cold_result = run_query(*state->engine, current);
        cold_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }

    auto warm_state = open_engine(probe->path, probe->layer);
    ASSERT_NE(warm_state, nullptr);
    Probe warm_probe = *probe;
    ASSERT_TRUE(resolve_probe(*warm_state, warm_probe));
    ASSERT_TRUE(resolve_index(*warm_state->catalog, warm_state->table_id, warm_probe));
    QueryResult warm_result;
    for (size_t trial = 0; trial < trials; ++trial) {
        const auto start = Clock::now();
        warm_result = run_query(*warm_state->engine, warm_probe);
        warm_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }

    const auto truth = gdal_truth(probe->path, probe->layer, *probe);
    ASSERT_EQ(warm_result.status, QueryStatus::Ok) << warm_result.error;
    EXPECT_EQ(warm_result.matched_fids, truth)
        << "path=" << warm_result.execution_path
        << " reason=" << warm_result.fallback_reason;

    const auto& metrics = warm_result.attribute_index_metrics;
    std::printf("\n[attr] path=%s reason=%s\n", warm_result.execution_path.c_str(),
                warm_result.fallback_reason.c_str());
    std::printf("[attr] predicate=%s %s %s matches=%zu truth=%zu\n", probe->field.c_str(),
                probe->op_text.c_str(), probe->value.c_str(), warm_result.matched_fids.size(),
                truth.size());
    std::printf("[attr] candidates=%zu pages=%zu directory=%s scan_preferred=%s\n",
                metrics.candidate_count, metrics.pages_visited,
                metrics.validated_directory_hit ? "hit" : "miss",
                metrics.validated_directory_scan_preferred ? "true" : "false");
    std::printf("[attr] cold_median=%.3fms warm_median=%.3fms warm_p95=%.3fms\n",
                percentile(cold_ms, 0.5), percentile(warm_ms, 0.5),
                percentile(warm_ms, 0.95));
    std::printf("[attr] peak_rss_bytes=%llu\n",
                static_cast<unsigned long long>(peak_rss_bytes()));

    // Sequential comparison through the public WHERE entry point.
    QueryRequest where_request;
    where_request.kind = QueryKind::WhereClause;
    where_request.where_clause = where_clause(*probe);
    std::vector<double> where_ms;
    QueryResult where_result;
    for (size_t trial = 0; trial < trials; ++trial) {
        const auto start = Clock::now();
        where_result = warm_state->engine->query(where_request);
        where_ms.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    EXPECT_EQ(where_result.matched_fids, truth)
        << "WHERE path=" << where_result.execution_path
        << " reason=" << where_result.fallback_reason;
    std::printf("[attr] where_median=%.3fms where_path=%s\n",
                percentile(where_ms, 0.5), where_result.execution_path.c_str());
}
