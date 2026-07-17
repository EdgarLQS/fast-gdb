#include "query_engine.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <sstream>
#include <utility>

namespace explorgdb {
namespace {

bool has_invalid_execution_path(const QueryResult& result) {
    static const std::string suffix = ":invalid";
    return result.execution_path.size() >= suffix.size() &&
           result.execution_path.compare(
               result.execution_path.size() - suffix.size(),
               suffix.size(), suffix) == 0;
}

bool query_result_is_error(const QueryRequest& request,
                           const QueryResult& result) {
    if (has_invalid_execution_path(result) ||
        result.execution_path == "query:blocked" ||
        result.fallback_reason == "unsupported query kind") {
        return true;
    }
    if (request.kind == QueryKind::WhereClause &&
        !result.fallback_reason.empty()) {
        return true;
    }
    if (request.kind == QueryKind::SpatialBbox &&
        (result.execution_path == "bbox:unavailable" ||
         result.execution_path == "bbox:model:unavailable")) {
        return true;
    }
    if (result.execution_path.empty() &&
        !result.fallback_reason.empty()) {
        return true;
    }
    return false;
}

std::string fid_error(const char* prefix, uint32_t fid) {
    std::ostringstream stream;
    stream << prefix << " " << fid;
    return stream.str();
}

} // namespace

class FeatureCursor::Impl {
public:
    enum class State {
        Ready,
        Exhausted,
        Failed
    };

    enum class Mode {
        CandidateFids,
        Sequential
    };

    using AcquireCallback = std::function<uint64_t()>;
    using ReleaseCallback = std::function<void(uint64_t)>;
    using EngineValidCallback = std::function<bool()>;

    Impl(QueryResult query_result, std::string error)
        : state_(State::Failed),
          query_result_(std::move(query_result)),
          error_(std::move(error)) {}

    explicit Impl(QueryResult query_result)
        : state_(State::Exhausted),
          query_result_(std::move(query_result)) {}

    Impl(GdbTableParser* table,
         uint64_t generation,
         AcquireCallback acquire,
         ReleaseCallback release,
         EngineValidCallback engine_valid,
         QueryResult query_result,
         bool profile_enabled)
        : table_(table),
          generation_(generation),
          acquire_(std::move(acquire)),
          release_(std::move(release)),
          engine_valid_(std::move(engine_valid)),
          query_result_(std::move(query_result)),
          profile_enabled_(profile_enabled),
          mode_(Mode::CandidateFids) {
        if (query_result_.matched_fids.empty()) {
            state_ = State::Exhausted;
            release_lease();
        }
    }

    Impl(GdbTableParser* table,
         uint64_t generation,
         AcquireCallback acquire,
         ReleaseCallback release,
         EngineValidCallback engine_valid,
         QueryResult query_result,
         bool profile_enabled,
         size_t feature_limit)
        : table_(table),
          generation_(generation),
          acquire_(std::move(acquire)),
          release_(std::move(release)),
          engine_valid_(std::move(engine_valid)),
          query_result_(std::move(query_result)),
          feature_limit_(feature_limit),
          profile_enabled_(profile_enabled),
          mode_(Mode::Sequential) {
        if (feature_limit_ == 0) {
            state_ = State::Exhausted;
            release_lease();
        }
    }

    ~Impl() {
        release_lease();
    }

    bool next(QueryFeature& output) {
        if (state_ != State::Ready) return false;
        if (!engine_is_current()) return false;

        uint32_t fid = 0;
        if (!next_fid(fid)) {
            if (state_ == State::Ready) exhaust();
            return false;
        }

        QueryFeature candidate;
        candidate.fid = fid;
        GdbTableParser* parser = table_;
        if (parser == nullptr) {
            fail("query engine table is unavailable");
            return false;
        }

        FeatureReadMetrics metrics;
        FeatureReadMetrics* metrics_ptr = profile_enabled_ ? &metrics : nullptr;
        if (!parser->read_feature_by_fid(
                fid, candidate.record, candidate.geometry, metrics_ptr)) {
            std::string message = candidate.geometry.diagnostic.empty()
                ? fid_error("failed to read full feature for fid", fid)
                : fid_error("failed to decode geometry for fid", fid) +
                    ": " + candidate.geometry.diagnostic;
            fail(std::move(message));
            return false;
        }
        if (candidate.record.fid != fid) {
            fail(fid_error("feature record fid mismatch for fid", fid));
            return false;
        }
        if (candidate.record.field_values.size() != parser->fields().size()) {
            fail(fid_error("feature field count mismatch for fid", fid));
            return false;
        }

        if (profile_enabled_) add_metrics(metrics);
        output = std::move(candidate);
        return true;
    }

    bool move_to(uint32_t fid) {
        if (state_ == State::Failed) return false;
        if (!engine_is_current() || !ensure_lease()) return false;

        if (mode_ == Mode::CandidateFids) {
            const auto& fids = query_result_.matched_fids;
            const auto iterator = std::lower_bound(fids.begin(), fids.end(), fid);
            fid_index_ = static_cast<size_t>(iterator - fids.begin());
            if (iterator == fids.end()) {
                exhaust();
                return false;
            }
        } else {
            GdbTableParser* parser = table_;
            if (parser == nullptr) {
                fail("query engine table is unavailable");
                return false;
            }

            size_t candidate = static_cast<size_t>(fid);
            while (candidate < feature_limit_) {
                if (candidate > static_cast<size_t>(
                                    std::numeric_limits<uint32_t>::max())) {
                    fail("feature fid exceeds uint32 range");
                    return false;
                }
                if (parser->has_feature(static_cast<uint32_t>(candidate))) break;
                ++candidate;
            }
            next_sequential_fid_ = candidate;
            if (candidate >= feature_limit_) {
                exhaust();
                return false;
            }
        }

        state_ = State::Ready;
        return true;
    }

    bool done() const noexcept {
        return state_ == State::Exhausted;
    }

    const QueryResult& query_result() const noexcept {
        return query_result_;
    }

    const std::string& error() const noexcept {
        return error_;
    }

private:
    void add_metrics(const FeatureReadMetrics& metrics) {
        FeatureCursorMetrics& total = query_result_.feature_cursor_metrics;
        ++total.feature_count;
        total.row_lookup_ms += metrics.row_lookup_ms;
        total.field_materialization_ms += metrics.field_materialization_ms;
        total.geometry_decode_ms += metrics.geometry_decode_ms;
        total.wkt_write_ms += metrics.wkt_write_ms;
        total.wkb_write_ms += metrics.wkb_write_ms;
    }

    bool engine_is_current() {
        if (!engine_valid_ || engine_valid_()) return true;
        fail("query engine was reopened while cursor existed");
        return false;
    }

    bool ensure_lease() {
        if (!engine_is_current()) return false;
        if (generation_ != 0) return true;
        if (!acquire_) {
            fail("feature cursor cannot reacquire query engine");
            return false;
        }
        generation_ = acquire_();
        if (generation_ == 0) {
            fail("another feature cursor is active");
            return false;
        }
        return true;
    }

    bool next_fid(uint32_t& fid) {
        if (mode_ == Mode::CandidateFids) {
            const auto& fids = query_result_.matched_fids;
            if (fid_index_ >= fids.size()) return false;
            fid = fids[fid_index_++];
            return true;
        }

        GdbTableParser* parser = table_;
        if (parser == nullptr) {
            fail("query engine table is unavailable");
            return false;
        }
        while (next_sequential_fid_ < feature_limit_) {
            const size_t candidate = next_sequential_fid_++;
            if (candidate > static_cast<size_t>(
                                std::numeric_limits<uint32_t>::max())) {
                fail("feature fid exceeds uint32 range");
                return false;
            }
            const uint32_t current = static_cast<uint32_t>(candidate);
            if (!parser->has_feature(current)) continue;
            fid = current;
            return true;
        }
        return false;
    }

    void exhaust() noexcept {
        if (state_ == State::Failed) return;
        state_ = State::Exhausted;
        release_lease();
    }

    void fail(std::string message) {
        if (state_ == State::Failed) return;
        state_ = State::Failed;
        error_ = std::move(message);
        release_lease();
    }

    void release_lease() noexcept {
        if (generation_ == 0) return;
        const uint64_t generation = generation_;
        generation_ = 0;
        if (release_) release_(generation);
    }

    GdbTableParser* table_ = nullptr;
    uint64_t generation_ = 0;
    AcquireCallback acquire_;
    ReleaseCallback release_;
    EngineValidCallback engine_valid_;
    State state_ = State::Ready;
    QueryResult query_result_;
    std::string error_;
    size_t fid_index_ = 0;
    size_t next_sequential_fid_ = 0;
    size_t feature_limit_ = 0;
    bool profile_enabled_ = false;
    Mode mode_ = Mode::CandidateFids;
};

FeatureCursor::FeatureCursor(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

FeatureCursor::FeatureCursor(FeatureCursor&& other) noexcept = default;
FeatureCursor& FeatureCursor::operator=(FeatureCursor&& other) noexcept = default;
FeatureCursor::~FeatureCursor() = default;

bool FeatureCursor::next(QueryFeature& feature) {
    return impl_ != nullptr && impl_->next(feature);
}

bool FeatureCursor::move_to(uint32_t fid) {
    return impl_ != nullptr && impl_->move_to(fid);
}

bool FeatureCursor::done() const noexcept {
    return impl_ == nullptr || impl_->done();
}

const QueryResult& FeatureCursor::query_result() const noexcept {
    static const QueryResult empty;
    return impl_ != nullptr ? impl_->query_result() : empty;
}

const std::string& FeatureCursor::error() const noexcept {
    static const std::string empty;
    return impl_ != nullptr ? impl_->error() : empty;
}

FeatureCursor QueryEngine::open_cursor(const QueryRequest& request) {
    QueryResult result;
    if (parser_ == nullptr) {
        result.execution_path = "cursor:invalid";
        result.fallback_reason = "table not open";
        return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
            std::move(result), "table not open"));
    }
    if (cursor_control_->feature_cursor_active()) {
        result.execution_path = "cursor:invalid";
        result.fallback_reason = "another feature cursor is active";
        return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
            std::move(result), "another feature cursor is active"));
    }

    const uint64_t planned_open_generation = cursor_control_->open_generation;
    CursorControl* const control = cursor_control_.get();
    auto acquire = [control]() noexcept {
        return control->register_feature_cursor();
    };
    auto release = [control](uint64_t generation) noexcept {
        control->release_feature_cursor(generation);
    };
    auto engine_valid = [control, planned_open_generation]() noexcept {
        return control->open_generation == planned_open_generation;
    };

    if (request.kind == QueryKind::SequentialScan) {
        result.execution_path = "cursor:sequential";
        const size_t feature_limit = parser_->feature_count();
        if (parser_->active_feature_count() == 0) {
            return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
                parser_.get(), 0, std::move(acquire), std::move(release),
                std::move(engine_valid), std::move(result),
                request.profile_feature_reads, size_t(0)));
        }

        const uint64_t generation = control->register_feature_cursor();
        if (generation == 0) {
            result.execution_path = "cursor:invalid";
            result.fallback_reason = "another feature cursor is active";
            return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
                std::move(result), "another feature cursor is active"));
        }
        return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
            parser_.get(), generation, std::move(acquire), std::move(release),
            std::move(engine_valid), std::move(result),
            request.profile_feature_reads, feature_limit));
    }

    result = query(request);
    if (query_result_is_error(request, result)) {
        std::string error = result.fallback_reason.empty()
            ? "invalid query request"
            : result.fallback_reason;
        return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
            std::move(result), std::move(error)));
    }

    std::sort(result.matched_fids.begin(), result.matched_fids.end());
    result.matched_fids.erase(
        std::unique(result.matched_fids.begin(), result.matched_fids.end()),
        result.matched_fids.end());
    result.record.reset();
    if (result.matched_fids.empty()) {
        return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
            parser_.get(), 0, std::move(acquire), std::move(release),
            std::move(engine_valid), std::move(result),
            request.profile_feature_reads));
    }

    const uint64_t generation = control->register_feature_cursor();
    if (generation == 0) {
        result.execution_path = "cursor:invalid";
        result.fallback_reason = "another feature cursor is active";
        return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
            std::move(result), "another feature cursor is active"));
    }

    return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
        parser_.get(), generation, std::move(acquire), std::move(release),
        std::move(engine_valid), std::move(result),
        request.profile_feature_reads));
}

uint64_t QueryEngine::register_feature_cursor() noexcept {
    return cursor_control_ != nullptr
        ? cursor_control_->register_feature_cursor()
        : 0;
}

void QueryEngine::release_feature_cursor(uint64_t generation) noexcept {
    if (cursor_control_ != nullptr)
        cursor_control_->release_feature_cursor(generation);
}

bool QueryEngine::feature_cursor_active() const noexcept {
    return cursor_control_ != nullptr &&
        cursor_control_->feature_cursor_active();
}

bool QueryEngine::peek_bbox_source(
    uint32_t fid,
    const uint8_t*& blob,
    size_t& size) {
    if (feature_cursor_active()) return false;
    return parser_ && parser_->peek_geometry_blob(fid, blob, size);
}

} // namespace explorgdb
