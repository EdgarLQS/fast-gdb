#include "versioned_gdb_store_internal.h"

#include <utility>

namespace explorgdb {
namespace writer {

using detail::ManifestSwitchResult;
using detail::StoreState;
namespace fs = std::filesystem;

GdbWriteTransaction::GdbWriteTransaction(
    std::shared_ptr<StoreState> state,
    std::string source_generation,
    std::string generation,
    fs::path working_path,
    GdbCloneStrategy clone_strategy)
    : state_(std::move(state)),
      source_generation_(std::move(source_generation)),
      generation_(std::move(generation)),
      working_path_(std::move(working_path)),
      clone_strategy_(clone_strategy) {}

GdbWriteTransaction::~GdbWriteTransaction() {
    if (valid()) abort();
}

GdbWriteTransaction::GdbWriteTransaction(GdbWriteTransaction&& other) noexcept
    : state_(std::move(other.state_)),
      source_generation_(std::move(other.source_generation_)),
      generation_(std::move(other.generation_)),
      working_path_(std::move(other.working_path_)),
      clone_strategy_(other.clone_strategy_),
      publish_state_(other.publish_state_),
      last_error_(std::move(other.last_error_)),
      completed_(other.completed_) {
    other.completed_ = true;
}

GdbWriteTransaction& GdbWriteTransaction::operator=(
    GdbWriteTransaction&& other) noexcept {
    if (this == &other) return *this;
    if (valid()) abort();
    state_ = std::move(other.state_);
    source_generation_ = std::move(other.source_generation_);
    generation_ = std::move(other.generation_);
    working_path_ = std::move(other.working_path_);
    clone_strategy_ = other.clone_strategy_;
    publish_state_ = other.publish_state_;
    last_error_ = std::move(other.last_error_);
    completed_ = other.completed_;
    other.completed_ = true;
    return *this;
}

bool GdbWriteTransaction::fail(std::string message) {
    last_error_ = std::move(message);
    if (state_) detail::set_error(state_, last_error_);
    return false;
}

void GdbWriteTransaction::release_writer_gate() noexcept {
    if (!state_) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->writer_active = false;
}

bool GdbWriteTransaction::publish(const GenerationValidator& validator) {
    if (!valid()) return fail("write transaction is not active");

    const GenerationValidationResult validation =
        detail::run_validator(validator, working_path_);
    if (!validation.ok) {
        return fail("candidate validation failed: " + validation.message);
    }

    std::string durability_error;
    if (!detail::sync_tree(working_path_, durability_error)) {
        return fail("candidate durability sync failed: " + durability_error);
    }

    const fs::path generation_path = state_->generations / generation_;
    std::error_code error;
    fs::rename(working_path_, generation_path, error);
    if (error) {
        return fail("promoting immutable generation failed: " + error.message());
    }
    if (!detail::flush_directory(state_->generations, durability_error)) {
        std::error_code cleanup_error;
        fs::remove_all(generation_path, cleanup_error);
        completed_ = true;
        release_writer_gate();
        return fail(detail::append_cleanup_error(
            "generation directory sync failed: " + durability_error,
            cleanup_error));
    }

    std::string manifest_error;
    const ManifestSwitchResult switch_result =
        detail::write_current_manifest(state_, generation_, manifest_error);
    if (switch_result == ManifestSwitchResult::NotSwitched) {
        std::error_code cleanup_error;
        fs::remove_all(generation_path, cleanup_error);
        completed_ = true;
        release_writer_gate();
        return fail(detail::append_cleanup_error(
            "CURRENT switch failed; candidate rolled back: " + manifest_error,
            cleanup_error));
    }

    const bool durability_uncertain =
        switch_result == ManifestSwitchResult::SwitchedDurabilityUncertain;
    publish_state_ = durability_uncertain
        ? GdbPublishState::PublishedDurabilityUncertain
        : GdbPublishState::PublishedDurable;
    const std::string uncertain_error = durability_uncertain
        ? detail::durability_uncertain_message(manifest_error)
        : std::string();

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->current_generation = generation_;
        state_->writer_active = false;
        state_->current_durability_uncertain = durability_uncertain;
        completed_ = true;

        if (durability_uncertain) {
            state_->last_error = uncertain_error;
        } else {
            std::string cleanup_error;
            if (!detail::cleanup_generations_locked(state_, cleanup_error)) {
                state_->last_error = std::move(cleanup_error);
            } else {
                state_->last_error.clear();
            }
        }
    }

    last_error_ = uncertain_error;
    return !durability_uncertain;
}

bool GdbWriteTransaction::abort() {
    if (!state_ || completed_) return true;
    std::error_code error;
    fs::remove_all(working_path_, error);
    completed_ = true;
    release_writer_gate();
    if (error) return fail("removing working GDB failed: " + error.message());
    last_error_.clear();
    detail::set_error(state_, {});
    return true;
}

}  // namespace writer
}  // namespace explorgdb
