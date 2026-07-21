#ifndef EXPLORGDB_VERSIONED_GDB_STORE_INTERNAL_H
#define EXPLORGDB_VERSIONED_GDB_STORE_INTERNAL_H

#include "versioned_gdb_store.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace explorgdb {
namespace writer {
namespace detail {

namespace fs = std::filesystem;

struct VersionedGdbStoreState {
    explicit VersionedGdbStoreState(fs::path canonical_root)
        : root(std::move(canonical_root)),
          generations(root / "generations"),
          work(root / "work"),
          current_manifest(root / "CURRENT") {}

    fs::path root;
    fs::path generations;
    fs::path work;
    fs::path current_manifest;
    mutable std::mutex mutex;
    std::unordered_map<std::string, size_t> reader_counts;
    std::string current_generation;
    std::string last_error;
    bool writer_active = false;
    bool opened = false;
    bool current_durability_uncertain = false;
};

using StoreState = VersionedGdbStoreState;

enum class ManifestSwitchResult {
    NotSwitched,
    SwitchedDurable,
    SwitchedDurabilityUncertain,
};

std::shared_ptr<StoreState> state_for_root(const fs::path& root);
std::string new_generation_name();
void set_error(const std::shared_ptr<StoreState>& state, std::string message);

bool flush_directory(const fs::path& path, std::string& error_message);
bool sync_tree(const fs::path& root, std::string& error_message);
ManifestSwitchResult write_current_manifest(
    const std::shared_ptr<StoreState>& state,
    const std::string& generation,
    std::string& error_message);
bool read_current_manifest(const std::shared_ptr<StoreState>& state,
                           std::string& generation,
                           std::string& error_message);

bool clone_tree(const fs::path& source,
                const fs::path& destination,
                GdbCloneStrategy& strategy,
                std::string& error_message);
bool cleanup_generations_locked(const std::shared_ptr<StoreState>& state,
                                std::string& error_message);
bool recover_locked(const std::shared_ptr<StoreState>& state,
                    std::string& error_message);
GenerationValidationResult run_validator(const GenerationValidator& validator,
                                         const fs::path& path);
std::string durability_uncertain_message(const std::string& detail);
std::string append_cleanup_error(std::string message,
                                 const std::error_code& cleanup_error);

}  // namespace detail
}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_VERSIONED_GDB_STORE_INTERNAL_H
