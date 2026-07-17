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

    Impl(QueryResult query_result, std::string error)
        : state_(State::Failed),
          query_result_(std::move(query_result)),
          error_(std::move(error)) {}

    explicit Impl(QueryResult query_result)
        : state_(State::Exhausted),
          query_result_(std::move(query_result)) {}

    Impl(QueryEngine* engine,
         uint64_t generation,
         AcquireCallback acquire,
         ReleaseCallback release,
         QueryResult query_result,
         std::vector<uint32_t> fids)
        : engine_(engine),
          generation_(generation),
          acquire_(std::move(acquire)),
          release_(std::move(release)),
          query_result_(std::move(query_result)),
          fids_(std::move(fids)),
          mode_(Mode::CandidateFids) {
        initialize_geometry_field();
        if (fids_.empty()) {
            state_ = State::Exhausted;
            release_lease();
        }
    }

    Impl(QueryEngine* engine,
         uint64_t generation,
         AcquireCallback acquire,
         ReleaseCallback release,
         QueryResult query_result,
         size_t feature_limit)
        : engine_(engine),
          generation_(generation),
          acquire_(std::move(acquire)),
          release_(std::move(release)),
          query_result_(std::move(query_result)),
          feature_limit_(feature_limit),
          mode_(Mode::Sequential) {
        initialize_geometry_field();
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

        uint32_t fid = 0;
        if (!next_fid(fid)) {
            if (state_ == State::Ready) exhaust();
            return false;
        }

        QueryFeature candidate;
        candidate.fid = fid;
        GdbTableParser* parser = engine_ != nullptr ? engine_->table() : nullptr;
        if (parser == nullptr) {
            fail("query engine table is unavailable");
            return false;
        }

        if (!parser->read_record_by_fid(fid, candidate.record)) {
            fail(fid_error("failed to read feature record for fid", fid));
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

        if (geometry_field_index_ < 0) {
            candidate.geometry = GeometryValue{};
            candidate.geometry.status = GeometryStatus::UnsupportedType;
            candidate.geometry.diagnostic = "table has no geometry field";
        } else {
            const size_t index = static_cast<size_t>(geometry_field_index_);
            if (index >= candidate.record.field_values.size()) {
                fail(fid_error("geometry field is missing for fid", fid));
                return false;
            }

            if (std::holds_alternative<std::nullptr_t>(
                    candidate.record.field_values[index])) {
                candidate.geometry = GeometryValue{};
                candidate.geometry.status = GeometryStatus::Empty;
                candidate.geometry.diagnostic = "geometry is null";
            } else if (!parser->read_geometry_value(fid, candidate.geometry)) {
                std::string message =
                    fid_error("failed to decode geometry for fid", fid);
                if (!candidate.geometry.diagnostic.empty()) {
                    message += ": ";
                    message += candidate.geometry.diagnostic;
                }
                fail(std::move(message));
                return false;
            }
        }

        output = std::move(candidate);
        return true;
    }

    bool move_to(uint32_t fid) {
        if (state_ == State::Failed) return false;

        if (mode_ == Mode::CandidateFids) {
            const auto iterator = std::lower_bound(fids_.begin(), fids_.end(), fid);
            fid_index_ = static_cast<size_t>(iterator - fids_.begin());
            if (iterator == fids_.end()) {
                exhaust();
                return false;
            }
        } else {
            GdbTableParser* parser =
                engine_ != nullptr ? engine_->table() : nullptr;
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

        if (!ensure_lease()) return false;
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
    void initialize_geometry_field() {
        if (engine_ == nullptr || engine_->table() == nullptr) return;
        const auto& fields = engine_->table()->fields();
        for (size_t index = 0; index < fields.size(); ++index) {
            if (fields[index].type == FieldType::Geometry) {
                geometry_field_index_ = static_cast<int>(index);
                break;
            }
        }
    }

    bool ensure_lease() {
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
            if (fid_index_ >= fids_.size()) return false;
            fid = fids_[fid_index_++];
            return true;
        }

        GdbTableParser* parser = engine_ != nullptr ? engine_->table() : nullptr;
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

    QueryEngine* engine_ = nullptr;
    uint64_t generation_ = 0;
    AcquireCallback acquire_;
    ReleaseCallback release_;
    State state_ = State::Ready;
    QueryResult query_result_;
    std::string error_;
    std::vector<uint32_t> fids_;
    size_t fid_index_ = 0;
    size_t next_sequential_fid_ = 0;
    size_t feature_limit_ = 0;
    int geometry_field_index_ = -1;
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
    if (feature_cursor_active()) {
        result.execution_path = "cursor:invalid";
        result.fallback_reason = "another feature cursor is active";
        return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
            std::move(result), "another feature cursor is active"));
    }

    auto acquire = [this]() noexcept {
        return register_feature_cursor();
    };
    auto release = [this](uint64_t generation) noexcept {
        release_feature_cursor(generation);
    };

    if (request.kind == QueryKind::SequentialScan) {
        result.execution_path = "cursor:sequential";
        const size_t feature_limit = parser_->feature_count();
        if (parser_->active_feature_count() == 0)
            return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
                std::move(result)));

        const uint64_t generation = register_feature_cursor();
        if (generation == 0) {
            result.execution_path = "cursor:invalid";
            result.fallback_reason = "another feature cursor is active";
            return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
                std::move(result), "another feature cursor is active"));
        }
        return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
            this, generation, std::move(acquire), std::move(release),
            std::move(result), feature_limit));
    }

    result = query(request);
    if (has_invalid_execution_path(result) ||
        result.fallback_reason == "unsupported query kind" ||
        result.execution_path == "query:blocked") {
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
    if (result.matched_fids.empty())
        return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
            std::move(result)));

    const uint64_t generation = register_feature_cursor();
    if (generation == 0) {
        result.execution_path = "cursor:invalid";
        result.fallback_reason = "another feature cursor is active";
        return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
            std::move(result), "another feature cursor is active"));
    }

    std::vector<uint32_t> fids = result.matched_fids;
    return FeatureCursor(std::make_unique<FeatureCursor::Impl>(
        this, generation, std::move(acquire), std::move(release),
        std::move(result), std::move(fids)));
}

} // namespace explorgdb