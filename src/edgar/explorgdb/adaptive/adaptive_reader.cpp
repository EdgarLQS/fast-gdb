// src/edgar/explorgdb/adaptive/adaptive_reader.cpp

#include "adaptive_reader.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace explorgdb {
namespace detail {

struct CoordinatedSourceEntry {
    bool writer_pending = false;
    bool writer_active = false;
    bool source_verified = true;
    uint64_t generation = 0;
    size_t fast_reader_count = 0;
    uint64_t writer_token_id = 0;
};

struct CoordinatorRegistry {
    std::mutex mutex;
    std::condition_variable condition;
    std::unordered_map<std::string, CoordinatedSourceEntry> sources;
    uint64_t next_token_id = 1;
};

}  // namespace detail

namespace {

using Clock = std::chrono::steady_clock;

CoordinatedSourceState snapshot(
    const detail::CoordinatedSourceEntry& entry) {
    CoordinatedSourceState result;
    result.writer_pending = entry.writer_pending;
    result.writer_active = entry.writer_active;
    result.source_verified = entry.source_verified;
    result.generation = entry.generation;
    result.fast_reader_count = entry.fast_reader_count;
    return result;
}

CoordinationStatus complete_external_update(
    const std::shared_ptr<detail::CoordinatorRegistry>& registry,
    const std::string& normalized_path,
    uint64_t token_id,
    bool close_succeeded) {
    if (!registry || normalized_path.empty() || token_id == 0) {
        return CoordinationStatus::InvalidCoordinationToken;
    }

    {
        std::lock_guard<std::mutex> lock(registry->mutex);
        const auto found = registry->sources.find(normalized_path);
        if (found == registry->sources.end() ||
            !found->second.writer_active ||
            found->second.writer_token_id != token_id) {
            return CoordinationStatus::InvalidCoordinationToken;
        }
        found->second.writer_active = false;
        found->second.writer_token_id = 0;
        ++found->second.generation;
        found->second.source_verified = close_succeeded;
    }
    registry->condition.notify_all();
    return close_succeeded ? CoordinationStatus::Ok
                           : CoordinationStatus::ExternalUpdateNotClosed;
}

AdaptiveReadStatus backend_failure_status(BackendFailureKind failure) {
    return failure == BackendFailureKind::Open
        ? AdaptiveReadStatus::GdalOpenFailed
        : AdaptiveReadStatus::GdalReadFailed;
}

}  // namespace

FastReaderLease::FastReaderLease(
    std::shared_ptr<detail::CoordinatorRegistry> registry,
    std::string normalized_path,
    uint64_t generation)
    : registry_(std::move(registry)),
      normalized_path_(std::move(normalized_path)),
      generation_(generation),
      counted_(true) {}

FastReaderLease::FastReaderLease(FastReaderLease&& other) noexcept
    : registry_(std::move(other.registry_)),
      normalized_path_(std::move(other.normalized_path_)),
      generation_(other.generation_),
      counted_(other.counted_) {
    other.counted_ = false;
    other.generation_ = 0;
}

FastReaderLease& FastReaderLease::operator=(FastReaderLease&& other) noexcept {
    if (this == &other) return *this;
    release();
    registry_ = std::move(other.registry_);
    normalized_path_ = std::move(other.normalized_path_);
    generation_ = other.generation_;
    counted_ = other.counted_;
    other.counted_ = false;
    other.generation_ = 0;
    return *this;
}

FastReaderLease::~FastReaderLease() {
    release();
}

bool FastReaderLease::expired_at_safe_point() const {
    if (!registry_ || normalized_path_.empty()) return true;

    std::lock_guard<std::mutex> lock(registry_->mutex);
    const auto found = registry_->sources.find(normalized_path_);
    if (found == registry_->sources.end()) return true;
    const auto& entry = found->second;
    return entry.writer_pending || entry.writer_active ||
           !entry.source_verified || entry.generation != generation_;
}

void FastReaderLease::release() {
    if (!counted_ || !registry_) return;

    {
        std::lock_guard<std::mutex> lock(registry_->mutex);
        const auto found = registry_->sources.find(normalized_path_);
        if (found != registry_->sources.end() &&
            found->second.fast_reader_count > 0) {
            --found->second.fast_reader_count;
        }
        counted_ = false;
    }
    registry_->condition.notify_all();
}

ExternalUpdateToken::ExternalUpdateToken(
    std::shared_ptr<detail::CoordinatorRegistry> registry,
    std::string normalized_path,
    uint64_t token_id)
    : registry_(std::move(registry)),
      normalized_path_(std::move(normalized_path)),
      token_id_(token_id),
      phase_(Phase::Pending) {}

ExternalUpdateToken::ExternalUpdateToken(
    ExternalUpdateToken&& other) noexcept
    : registry_(std::move(other.registry_)),
      normalized_path_(std::move(other.normalized_path_)),
      token_id_(other.token_id_),
      phase_(other.phase_) {
    other.token_id_ = 0;
    other.phase_ = Phase::Invalid;
}

ExternalUpdateToken& ExternalUpdateToken::operator=(
    ExternalUpdateToken&& other) noexcept {
    if (this == &other) return *this;
    abandon_current_state();
    registry_ = std::move(other.registry_);
    normalized_path_ = std::move(other.normalized_path_);
    token_id_ = other.token_id_;
    phase_ = other.phase_;
    other.token_id_ = 0;
    other.phase_ = Phase::Invalid;
    return *this;
}

ExternalUpdateToken::~ExternalUpdateToken() {
    abandon_current_state();
}

bool ExternalUpdateToken::valid() const noexcept {
    return phase_ == Phase::Pending || phase_ == Phase::Active;
}

bool ExternalUpdateToken::pending() const noexcept {
    return phase_ == Phase::Pending;
}

bool ExternalUpdateToken::active() const noexcept {
    return phase_ == Phase::Active;
}

CoordinationStatus ExternalUpdateToken::notify_update_opened() {
    if (phase_ != Phase::Pending || !registry_) {
        return CoordinationStatus::InvalidCoordinationToken;
    }

    {
        std::lock_guard<std::mutex> lock(registry_->mutex);
        const auto found = registry_->sources.find(normalized_path_);
        if (found == registry_->sources.end() ||
            !found->second.writer_pending ||
            found->second.writer_token_id != token_id_) {
            return CoordinationStatus::InvalidCoordinationToken;
        }
        if (found->second.fast_reader_count != 0) {
            return CoordinationStatus::ReadersActive;
        }
        found->second.writer_pending = false;
        found->second.writer_active = true;
        phase_ = Phase::Active;
    }
    registry_->condition.notify_all();
    return CoordinationStatus::Ok;
}

CoordinationStatus ExternalUpdateToken::cancel_before_update() {
    if (phase_ != Phase::Pending || !registry_) {
        return CoordinationStatus::InvalidCoordinationToken;
    }

    {
        std::lock_guard<std::mutex> lock(registry_->mutex);
        const auto found = registry_->sources.find(normalized_path_);
        if (found == registry_->sources.end() ||
            !found->second.writer_pending ||
            found->second.writer_token_id != token_id_) {
            return CoordinationStatus::InvalidCoordinationToken;
        }
        found->second.writer_pending = false;
        found->second.writer_token_id = 0;
        phase_ = Phase::Closed;
    }
    registry_->condition.notify_all();
    return CoordinationStatus::Ok;
}

CoordinationStatus ExternalUpdateToken::notify_update_closed(
    bool close_succeeded) {
    if (phase_ != Phase::Active || !registry_) {
        return CoordinationStatus::InvalidCoordinationToken;
    }

    const CoordinationStatus status = complete_external_update(
        registry_, normalized_path_, token_id_, close_succeeded);
    if (status == CoordinationStatus::Ok ||
        status == CoordinationStatus::ExternalUpdateNotClosed) {
        phase_ = Phase::Closed;
    }
    return status;
}

void ExternalUpdateToken::abandon_current_state() noexcept {
    if (phase_ == Phase::Pending) {
        try {
            (void)cancel_before_update();
        } catch (...) {
            // Destruction cannot safely report a cancellation failure. The
            // registry remains fail-closed if cancellation did not complete.
        }
    }

    // Active is intentionally not cleared here: the external GDALDataset may
    // still be open. Losing the token leaves the source fail-closed until the
    // caller confirms close with the saved coordination id.
    registry_.reset();
    normalized_path_.clear();
    token_id_ = 0;
    phase_ = Phase::Invalid;
}

InProcessGdbCoordinator::InProcessGdbCoordinator()
    : registry_(std::make_shared<detail::CoordinatorRegistry>()) {}

FastReaderLease InProcessGdbCoordinator::try_acquire_fast_reader(
    const std::string& gdb_path) const {
    const std::string normalized = normalize_path(gdb_path);
    if (normalized.empty()) return {};

    std::lock_guard<std::mutex> lock(registry_->mutex);
    auto& entry = registry_->sources[normalized];
    if (entry.writer_pending || entry.writer_active ||
        !entry.source_verified) {
        return {};
    }
    ++entry.fast_reader_count;
    return FastReaderLease(registry_, normalized, entry.generation);
}

PrepareExternalUpdateResult
InProcessGdbCoordinator::prepare_external_update(
    const std::string& gdb_path,
    std::chrono::milliseconds drain_timeout) const {
    PrepareExternalUpdateResult result;
    const auto started = Clock::now();
    const std::string normalized = normalize_path(gdb_path);
    if (normalized.empty()) {
        result.status = CoordinationStatus::InvalidCoordinationToken;
        return result;
    }
    if (drain_timeout.count() < 0) {
        drain_timeout = std::chrono::milliseconds(0);
    }

    std::unique_lock<std::mutex> lock(registry_->mutex);
    auto& initial = registry_->sources[normalized];
    if (initial.writer_pending) {
        result.status = CoordinationStatus::WriterAlreadyPending;
        result.active_readers = initial.fast_reader_count;
        return result;
    }
    if (initial.writer_active) {
        result.status = CoordinationStatus::WriterAlreadyActive;
        result.active_readers = initial.fast_reader_count;
        return result;
    }

    const uint64_t token_id = registry_->next_token_id++;
    initial.writer_pending = true;
    initial.writer_token_id = token_id;
    registry_->condition.notify_all();

    const bool drained = registry_->condition.wait_for(
        lock, drain_timeout, [&] {
            const auto found = registry_->sources.find(normalized);
            return found != registry_->sources.end() &&
                   found->second.writer_pending &&
                   found->second.writer_token_id == token_id &&
                   found->second.fast_reader_count == 0;
        });

    auto& current = registry_->sources[normalized];
    result.waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - started);
    result.active_readers = current.fast_reader_count;

    if (!drained) {
        if (current.writer_pending &&
            current.writer_token_id == token_id) {
            current.writer_pending = false;
            current.writer_token_id = 0;
        }
        result.status = CoordinationStatus::ReadersActive;
        lock.unlock();
        registry_->condition.notify_all();
        return result;
    }

    result.status = CoordinationStatus::Ok;
    result.token = ExternalUpdateToken(registry_, normalized, token_id);
    return result;
}

CoordinationStatus InProcessGdbCoordinator::notify_external_update_closed(
    const std::string& gdb_path,
    uint64_t coordination_id,
    bool close_succeeded) const {
    return complete_external_update(
        registry_, normalize_path(gdb_path), coordination_id,
        close_succeeded);
}

CoordinatedSourceState InProcessGdbCoordinator::state(
    const std::string& gdb_path) const {
    const std::string normalized = normalize_path(gdb_path);
    if (normalized.empty()) return {};

    std::lock_guard<std::mutex> lock(registry_->mutex);
    const auto found = registry_->sources.find(normalized);
    if (found == registry_->sources.end()) return {};
    return snapshot(found->second);
}

std::string InProcessGdbCoordinator::normalize_path(
    const std::string& gdb_path) {
    if (gdb_path.empty()) return {};

    namespace fs = std::filesystem;
    std::error_code error;
    fs::path normalized_path = fs::absolute(fs::path(gdb_path), error);
    if (error) normalized_path = fs::path(gdb_path);
    normalized_path = normalized_path.lexically_normal();
    std::string normalized = normalized_path.generic_string();

#ifdef _WIN32
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
#endif

    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

BackendReadResult BackendReadResult::success(QueryResult result) {
    BackendReadResult output;
    output.ok = true;
    output.result = std::move(result);
    output.failure = BackendFailureKind::None;
    return output;
}

BackendReadResult BackendReadResult::open_failure(std::string error) {
    BackendReadResult output;
    output.ok = false;
    output.failure = BackendFailureKind::Open;
    output.error = std::move(error);
    return output;
}

BackendReadResult BackendReadResult::read_failure(std::string error) {
    BackendReadResult output;
    output.ok = false;
    output.failure = BackendFailureKind::Read;
    output.error = std::move(error);
    return output;
}

AdaptiveFeatureCursor::AdaptiveFeatureCursor(
    AdaptiveReadStatus status,
    AdaptiveReadBackend backend,
    AdaptiveReadConsistency consistency,
    FastReaderLease fast_lease,
    BackendCursor backend_cursor)
    : status_(status),
      backend_(backend),
      consistency_(consistency),
      fast_lease_(std::move(fast_lease)),
      backend_cursor_(std::move(backend_cursor)),
      done_(status != AdaptiveReadStatus::Ok || !backend_cursor_.next) {}

AdaptiveFeatureCursor::AdaptiveFeatureCursor(
    AdaptiveFeatureCursor&& other) noexcept
    : status_(other.status_),
      backend_(other.backend_),
      consistency_(other.consistency_),
      fast_lease_(std::move(other.fast_lease_)),
      backend_cursor_(std::move(other.backend_cursor_)),
      done_(other.done_),
      backend_closed_(other.backend_closed_),
      error_(std::move(other.error_)) {
    other.done_ = true;
    other.backend_closed_ = true;
}

AdaptiveFeatureCursor& AdaptiveFeatureCursor::operator=(
    AdaptiveFeatureCursor&& other) noexcept {
    if (this == &other) return *this;
    close();
    status_ = other.status_;
    backend_ = other.backend_;
    consistency_ = other.consistency_;
    fast_lease_ = std::move(other.fast_lease_);
    backend_cursor_ = std::move(other.backend_cursor_);
    done_ = other.done_;
    backend_closed_ = other.backend_closed_;
    error_ = std::move(other.error_);
    other.done_ = true;
    other.backend_closed_ = true;
    return *this;
}

AdaptiveFeatureCursor::~AdaptiveFeatureCursor() {
    close();
}

bool AdaptiveFeatureCursor::next(QueryFeature& feature) {
    if (done_ || status_ != AdaptiveReadStatus::Ok) return false;

    if (backend_ == AdaptiveReadBackend::FastGdb &&
        fast_lease_.expired_at_safe_point()) {
        status_ = AdaptiveReadStatus::ReaderExpired;
        consistency_ = AdaptiveReadConsistency::NotApplicable;
        error_ = "fast reader lease expired at cursor safe point";
        close();
        return false;
    }

    std::string backend_error;
    bool has_feature = false;
    try {
        has_feature = backend_cursor_.next(feature, backend_error);
    } catch (const std::exception& exception) {
        backend_error = exception.what();
    } catch (...) {
        backend_error = "backend cursor threw an unknown exception";
    }

    if (has_feature) return true;

    if (!backend_error.empty()) {
        error_ = std::move(backend_error);
        if (backend_ == AdaptiveReadBackend::GdalOpenFileGDB) {
            status_ = AdaptiveReadStatus::GdalReadFailed;
            consistency_ =
                AdaptiveReadConsistency::UnverifiedConcurrentRead;
        } else {
            status_ = AdaptiveReadStatus::FastBackendReadFailed;
            consistency_ = AdaptiveReadConsistency::NotApplicable;
        }
    }
    close();
    return false;
}

void AdaptiveFeatureCursor::close() {
    if (!backend_closed_) {
        if (backend_cursor_.close) {
            try {
                backend_cursor_.close();
            } catch (...) {
                // Cursor destruction and safe-point expiry must still release
                // the fast lease even if a backend cleanup callback fails.
            }
        }
        backend_closed_ = true;
    }
    fast_lease_.release();
    done_ = true;
}

AdaptiveReadSession::AdaptiveReadSession(
    InProcessGdbCoordinator coordinator,
    std::string gdb_path,
    ReadExecutor fast_executor,
    ReadExecutor gdal_executor,
    CursorFactory fast_cursor_factory,
    CursorFactory gdal_cursor_factory)
    : coordinator_(std::move(coordinator)),
      gdb_path_(std::move(gdb_path)),
      fast_executor_(std::move(fast_executor)),
      gdal_executor_(std::move(gdal_executor)),
      fast_cursor_factory_(std::move(fast_cursor_factory)),
      gdal_cursor_factory_(std::move(gdal_cursor_factory)) {}

AdaptiveReadResult AdaptiveReadSession::read(
    const QueryRequest& request,
    ConcurrentReadPolicy policy) const {
    AdaptiveReadResult output;
    auto fast_lease = coordinator_.try_acquire_fast_reader(gdb_path_);

    if (fast_lease.valid()) {
        output.backend = AdaptiveReadBackend::FastGdb;
        output.generation_before = fast_lease.generation();

        BackendReadResult backend;
        try {
            backend = fast_executor_
                ? fast_executor_(request)
                : BackendReadResult::read_failure(
                      "fast executor is not configured");
        } catch (const std::exception& exception) {
            backend = BackendReadResult::read_failure(exception.what());
        } catch (...) {
            backend = BackendReadResult::read_failure(
                "fast executor threw an unknown exception");
        }

        const auto during = coordinator_.state(gdb_path_);
        output.generation_after = fast_lease.generation();
        output.writer_pending_seen = during.writer_pending;
        output.writer_active_seen = during.writer_active;
        fast_lease.release();

        if (backend.ok) {
            output.status = AdaptiveReadStatus::Ok;
            output.consistency = AdaptiveReadConsistency::Verified;
            output.result = std::move(backend.result);
        } else {
            output.status = AdaptiveReadStatus::FastBackendReadFailed;
            output.consistency = AdaptiveReadConsistency::NotApplicable;
            output.fast_error = std::move(backend.error);
        }
        return output;
    }

    const auto before = coordinator_.state(gdb_path_);
    output.generation_before = before.generation;
    output.generation_after = before.generation;
    output.writer_pending_seen = before.writer_pending;
    output.writer_active_seen = before.writer_active;

    if (policy == ConcurrentReadPolicy::SourceBusy) {
        output.status = AdaptiveReadStatus::SourceBusy;
        output.backend = AdaptiveReadBackend::None;
        output.consistency = AdaptiveReadConsistency::NotApplicable;
        return output;
    }

    output.backend = AdaptiveReadBackend::GdalOpenFileGDB;
    output.consistency =
        AdaptiveReadConsistency::UnverifiedConcurrentRead;

    BackendReadResult backend;
    try {
        backend = gdal_executor_
            ? gdal_executor_(request)
            : BackendReadResult::open_failure(
                  "fresh GDAL executor is not configured");
    } catch (const std::exception& exception) {
        backend = BackendReadResult::read_failure(exception.what());
    } catch (...) {
        backend = BackendReadResult::read_failure(
            "GDAL executor threw an unknown exception");
    }

    const auto after = coordinator_.state(gdb_path_);
    output.generation_after = after.generation;
    output.writer_pending_seen = output.writer_pending_seen ||
                                 after.writer_pending;
    output.writer_active_seen = output.writer_active_seen ||
                                after.writer_active;

    if (backend.ok) {
        output.status = AdaptiveReadStatus::Ok;
        output.result = std::move(backend.result);
    } else {
        output.status = backend_failure_status(backend.failure);
        output.gdal_error = std::move(backend.error);
    }
    return output;
}

AdaptiveFeatureCursor AdaptiveReadSession::open_cursor(
    const QueryRequest& request,
    ConcurrentReadPolicy policy) const {
    auto fast_lease = coordinator_.try_acquire_fast_reader(gdb_path_);
    if (fast_lease.valid()) {
        if (!fast_cursor_factory_) {
            fast_lease.release();
            AdaptiveFeatureCursor cursor(
                AdaptiveReadStatus::FastBackendReadFailed,
                AdaptiveReadBackend::FastGdb,
                AdaptiveReadConsistency::NotApplicable,
                {}, {});
            cursor.error_ = "fast cursor factory is not configured";
            return cursor;
        }

        try {
            BackendCursor backend_cursor = fast_cursor_factory_(request);
            if (!backend_cursor.next) {
                fast_lease.release();
                AdaptiveFeatureCursor cursor(
                    AdaptiveReadStatus::FastBackendReadFailed,
                    AdaptiveReadBackend::FastGdb,
                    AdaptiveReadConsistency::NotApplicable,
                    {}, {});
                cursor.error_ = "fast cursor factory returned no next callback";
                return cursor;
            }
            return AdaptiveFeatureCursor(
                AdaptiveReadStatus::Ok,
                AdaptiveReadBackend::FastGdb,
                AdaptiveReadConsistency::Verified,
                std::move(fast_lease),
                std::move(backend_cursor));
        } catch (const std::exception& exception) {
            fast_lease.release();
            AdaptiveFeatureCursor cursor(
                AdaptiveReadStatus::FastBackendReadFailed,
                AdaptiveReadBackend::FastGdb,
                AdaptiveReadConsistency::NotApplicable,
                {}, {});
            cursor.error_ = exception.what();
            return cursor;
        } catch (...) {
            fast_lease.release();
            AdaptiveFeatureCursor cursor(
                AdaptiveReadStatus::FastBackendReadFailed,
                AdaptiveReadBackend::FastGdb,
                AdaptiveReadConsistency::NotApplicable,
                {}, {});
            cursor.error_ = "fast cursor factory threw an unknown exception";
            return cursor;
        }
    }

    if (policy == ConcurrentReadPolicy::SourceBusy) {
        return AdaptiveFeatureCursor(
            AdaptiveReadStatus::SourceBusy,
            AdaptiveReadBackend::None,
            AdaptiveReadConsistency::NotApplicable,
            {}, {});
    }

    if (!gdal_cursor_factory_) {
        AdaptiveFeatureCursor cursor(
            AdaptiveReadStatus::GdalOpenFailed,
            AdaptiveReadBackend::GdalOpenFileGDB,
            AdaptiveReadConsistency::UnverifiedConcurrentRead,
            {}, {});
        cursor.error_ = "fresh GDAL cursor factory is not configured";
        return cursor;
    }

    try {
        BackendCursor backend_cursor = gdal_cursor_factory_(request);
        if (!backend_cursor.next) {
            AdaptiveFeatureCursor cursor(
                AdaptiveReadStatus::GdalOpenFailed,
                AdaptiveReadBackend::GdalOpenFileGDB,
                AdaptiveReadConsistency::UnverifiedConcurrentRead,
                {}, {});
            cursor.error_ = "GDAL cursor factory returned no next callback";
            return cursor;
        }
        return AdaptiveFeatureCursor(
            AdaptiveReadStatus::Ok,
            AdaptiveReadBackend::GdalOpenFileGDB,
            AdaptiveReadConsistency::UnverifiedConcurrentRead,
            {}, std::move(backend_cursor));
    } catch (const std::exception& exception) {
        AdaptiveFeatureCursor cursor(
            AdaptiveReadStatus::GdalOpenFailed,
            AdaptiveReadBackend::GdalOpenFileGDB,
            AdaptiveReadConsistency::UnverifiedConcurrentRead,
            {}, {});
        cursor.error_ = exception.what();
        return cursor;
    } catch (...) {
        AdaptiveFeatureCursor cursor(
            AdaptiveReadStatus::GdalOpenFailed,
            AdaptiveReadBackend::GdalOpenFileGDB,
            AdaptiveReadConsistency::UnverifiedConcurrentRead,
            {}, {});
        cursor.error_ = "GDAL cursor factory threw an unknown exception";
        return cursor;
    }
}

}  // namespace explorgdb
