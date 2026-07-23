// src/edgar/explorgdb/adaptive/adaptive_coordination.cpp

#include "adaptive_reader.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
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
    bool writer_generation_invalidated = false;
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

std::shared_ptr<detail::CoordinatorRegistry> process_registry() {
    static const auto registry =
        std::make_shared<detail::CoordinatorRegistry>();
    return registry;
}

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

    CoordinationStatus status = CoordinationStatus::Ok;
    {
        std::lock_guard<std::mutex> lock(registry->mutex);
        const auto found = registry->sources.find(normalized_path);
        if (found == registry->sources.end() ||
            !found->second.writer_active ||
            found->second.writer_token_id != token_id) {
            return CoordinationStatus::InvalidCoordinationToken;
        }

        auto& entry = found->second;
        if (!entry.writer_generation_invalidated) {
            ++entry.generation;
            entry.writer_generation_invalidated = true;
        }

        if (!close_succeeded) {
            // The external Dataset is not confirmed closed. Keep Active and
            // retain the token id so no new Writer or fast lease can overlap.
            entry.source_verified = false;
            status = CoordinationStatus::ExternalUpdateNotClosed;
        } else {
            entry.writer_active = false;
            entry.writer_token_id = 0;
            entry.writer_generation_invalidated = false;
            entry.source_verified = true;
        }
    }
    registry->condition.notify_all();
    return status;
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
        found->second.writer_generation_invalidated = false;
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
    if (status == CoordinationStatus::Ok) phase_ = Phase::Closed;
    return status;
}

void ExternalUpdateToken::abandon_current_state() noexcept {
    if (phase_ == Phase::Pending) {
        try {
            (void)cancel_before_update();
        } catch (...) {
            // Destruction cannot report failure. If cancellation did not
            // complete, the registry remains fail-closed.
        }
    }

    // Active is never cleared by destruction. The external Dataset may still
    // be open; recovery requires the saved coordination id.
    registry_.reset();
    normalized_path_.clear();
    token_id_ = 0;
    phase_ = Phase::Invalid;
}

InProcessGdbCoordinator::InProcessGdbCoordinator()
    : registry_(process_registry()) {}

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

    uint64_t token_id = registry_->next_token_id++;
    if (token_id == 0) token_id = registry_->next_token_id++;
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

    std::error_code canonical_error;
    const fs::path canonical = fs::weakly_canonical(
        normalized_path, canonical_error);
    if (!canonical_error) normalized_path = canonical;

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

}  // namespace explorgdb
