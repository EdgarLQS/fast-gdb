#include "versioned_gdb_store_internal.h"

#include <utility>

namespace explorgdb {
namespace writer {

using detail::StoreState;

GdbReaderSnapshot::GdbReaderSnapshot(std::shared_ptr<StoreState> state,
                                     std::string generation,
                                     std::filesystem::path path)
    : state_(std::move(state)),
      generation_(std::move(generation)),
      path_(std::move(path)) {}

GdbReaderSnapshot::~GdbReaderSnapshot() { release(); }

GdbReaderSnapshot::GdbReaderSnapshot(GdbReaderSnapshot&& other) noexcept
    : state_(std::move(other.state_)),
      generation_(std::move(other.generation_)),
      path_(std::move(other.path_)) {}

GdbReaderSnapshot& GdbReaderSnapshot::operator=(
    GdbReaderSnapshot&& other) noexcept {
    if (this == &other) return *this;
    release();
    state_ = std::move(other.state_);
    generation_ = std::move(other.generation_);
    path_ = std::move(other.path_);
    return *this;
}

void GdbReaderSnapshot::release() noexcept {
    if (!state_) return;
    const std::shared_ptr<StoreState> state = state_;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const auto found = state->reader_counts.find(generation_);
        if (found != state->reader_counts.end()) {
            if (found->second > 1) {
                --found->second;
            } else {
                state->reader_counts.erase(found);
            }
        }
        std::string cleanup_error;
        if (!detail::cleanup_generations_locked(state, cleanup_error)) {
            state->last_error = std::move(cleanup_error);
        }
    }
    state_.reset();
    generation_.clear();
    path_.clear();
}

bool GdbReaderSnapshot::refresh() {
    if (!state_) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->current_generation.empty()) {
        state_->last_error = "cannot refresh: store has no CURRENT generation";
        return false;
    }
    if (generation_ == state_->current_generation) {
        state_->last_error.clear();
        return true;
    }

    const std::filesystem::path next_path =
        state_->generations / state_->current_generation;
    std::error_code error;
    if (!std::filesystem::is_directory(next_path, error) || error) {
        state_->last_error = "cannot refresh: CURRENT generation is missing";
        return false;
    }

    ++state_->reader_counts[state_->current_generation];
    const auto previous = state_->reader_counts.find(generation_);
    if (previous != state_->reader_counts.end()) {
        if (previous->second > 1) {
            --previous->second;
        } else {
            state_->reader_counts.erase(previous);
        }
    }
    generation_ = state_->current_generation;
    path_ = next_path;

    std::string cleanup_error;
    if (!detail::cleanup_generations_locked(state_, cleanup_error)) {
        state_->last_error = std::move(cleanup_error);
    } else {
        state_->last_error.clear();
    }
    return true;
}

}  // namespace writer
}  // namespace explorgdb
