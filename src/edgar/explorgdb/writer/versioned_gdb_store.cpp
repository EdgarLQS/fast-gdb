// src/edgar/explorgdb/writer/versioned_gdb_store.cpp

#include "versioned_gdb_store_internal.h"

#include <utility>

namespace explorgdb {
namespace writer {

namespace fs = std::filesystem;
using detail::ManifestSwitchResult;

VersionedGdbStore::VersionedGdbStore(fs::path root)
    : state_(detail::state_for_root(root)) {}

bool VersionedGdbStore::fail(std::string message) {
    detail::set_error(state_, std::move(message));
    return false;
}

bool VersionedGdbStore::open() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->opened) return true;
    std::string error;
    if (!detail::recover_locked(state_, error)) {
        state_->last_error = std::move(error);
        return false;
    }
    return true;
}

bool VersionedGdbStore::ensure_open() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->opened) return true;
    }
    return open();
}

bool VersionedGdbStore::initialize_from(
    const fs::path& source_gdb_path,
    const GenerationValidator& validator) {
    if (!ensure_open()) return false;

    std::string generation;
    fs::path working;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->current_generation.empty()) {
            state_->last_error = "store is already initialized";
            return false;
        }
        if (state_->writer_active) {
            state_->last_error = "another Writer owns this repository";
            return false;
        }
        if (state_->current_durability_uncertain) {
            state_->last_error =
                "recover() is required after an uncertain CURRENT switch";
            return false;
        }
        state_->writer_active = true;
        generation = detail::new_generation_name();
        working = state_->work / ("work-" + generation);
    }

    auto rollback_working = [&](std::string message) {
        std::error_code cleanup_error;
        fs::remove_all(working, cleanup_error);
        message = detail::append_cleanup_error(std::move(message), cleanup_error);
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->writer_active = false;
        state_->last_error = std::move(message);
        return false;
    };

    GdbCloneStrategy strategy = GdbCloneStrategy::FullCopy;
    std::string clone_error;
    if (!detail::clone_tree(source_gdb_path, working, strategy, clone_error)) {
        return rollback_working("bootstrap clone failed: " + clone_error);
    }
    (void)strategy;

    const GenerationValidationResult validation =
        detail::run_validator(validator, working);
    if (!validation.ok) {
        return rollback_working("bootstrap validation failed: " +
                                validation.message);
    }

    std::string durability_error;
    if (!detail::sync_tree(working, durability_error)) {
        return rollback_working("bootstrap durability sync failed: " +
                                durability_error);
    }

    const fs::path generation_path = state_->generations / generation;
    std::error_code error;
    fs::rename(working, generation_path, error);
    if (error) {
        return rollback_working("bootstrap promotion failed: " + error.message());
    }
    if (!detail::flush_directory(state_->generations, durability_error)) {
        std::error_code cleanup_error;
        fs::remove_all(generation_path, cleanup_error);
        const std::string failure = detail::append_cleanup_error(
            "bootstrap generation sync failed: " + durability_error,
            cleanup_error);
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->writer_active = false;
        state_->last_error = failure;
        return false;
    }

    std::string manifest_error;
    const ManifestSwitchResult switch_result =
        detail::write_current_manifest(state_, generation, manifest_error);
    if (switch_result == ManifestSwitchResult::NotSwitched) {
        std::error_code cleanup_error;
        fs::remove_all(generation_path, cleanup_error);
        const std::string failure = detail::append_cleanup_error(
            "bootstrap CURRENT switch failed: " + manifest_error,
            cleanup_error);
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->writer_active = false;
        state_->last_error = failure;
        return false;
    }

    const bool durability_uncertain =
        switch_result == ManifestSwitchResult::SwitchedDurabilityUncertain;
    const std::string uncertain_error = durability_uncertain
        ? detail::durability_uncertain_message(manifest_error)
        : std::string();
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->current_generation = generation;
        state_->writer_active = false;
        state_->current_durability_uncertain = durability_uncertain;
        state_->last_error = uncertain_error;
    }
    return !durability_uncertain;
}

GdbReaderSnapshot VersionedGdbStore::acquire_reader() {
    if (!ensure_open()) return {};
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->current_generation.empty()) {
        state_->last_error = "store has no CURRENT generation";
        return {};
    }
    const fs::path path = state_->generations / state_->current_generation;
    std::error_code error;
    if (!fs::is_directory(path, error) || error) {
        state_->last_error = "CURRENT generation directory is missing";
        return {};
    }
    ++state_->reader_counts[state_->current_generation];
    state_->last_error.clear();
    return GdbReaderSnapshot(state_, state_->current_generation, path);
}

GdbWriteTransaction VersionedGdbStore::begin_write() {
    if (!ensure_open()) return {};

    std::string source_generation;
    std::string generation;
    fs::path source;
    fs::path working;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->current_generation.empty()) {
            state_->last_error = "store has no CURRENT generation";
            return {};
        }
        if (state_->writer_active) {
            state_->last_error = "another Writer owns this repository";
            return {};
        }
        if (state_->current_durability_uncertain) {
            state_->last_error =
                "recover() is required after an uncertain CURRENT switch";
            return {};
        }
        state_->writer_active = true;
        source_generation = state_->current_generation;
        generation = detail::new_generation_name();
        source = state_->generations / source_generation;
        working = state_->work / ("work-" + generation);
    }

    GdbCloneStrategy strategy = GdbCloneStrategy::FullCopy;
    std::string clone_error;
    if (!detail::clone_tree(source, working, strategy, clone_error)) {
        std::error_code cleanup_error;
        fs::remove_all(working, cleanup_error);
        const std::string failure = detail::append_cleanup_error(
            "working clone failed: " + clone_error, cleanup_error);
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->writer_active = false;
        state_->last_error = failure;
        return {};
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->last_error.clear();
    }
    return GdbWriteTransaction(state_, std::move(source_generation),
                               std::move(generation), std::move(working),
                               strategy);
}

bool VersionedGdbStore::recover() {
    if (!ensure_open()) return false;
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->writer_active) {
        state_->last_error = "cannot recover while a Writer is active";
        return false;
    }
    for (const auto& readers : state_->reader_counts) {
        if (readers.second != 0) {
            state_->last_error =
                "cannot recover while Reader snapshots are active";
            return false;
        }
    }
    std::string error;
    if (!detail::recover_locked(state_, error)) {
        state_->last_error = std::move(error);
        return false;
    }
    return true;
}

const fs::path& VersionedGdbStore::root() const noexcept {
    return state_->root;
}

std::string VersionedGdbStore::current_generation() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->current_generation;
}

std::string VersionedGdbStore::last_error() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->last_error;
}

}  // namespace writer
}  // namespace explorgdb
