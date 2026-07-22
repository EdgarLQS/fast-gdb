// Immutable-generation FileGDB publication with reader snapshot leases.

#ifndef FAST_GDB_PUBLIC_VERSIONED_GDB_STORE_H
#define FAST_GDB_PUBLIC_VERSIONED_GDB_STORE_H

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace explorgdb {
namespace writer {

enum class GdbCloneStrategy {
    CopyOnWrite,
    FullCopy,
};

enum class GdbPublishState {
    NotPublished,
    PublishedDurable,
    PublishedDurabilityUncertain,
};

struct GenerationValidationResult {
    bool ok = false;
    std::string message;

    static GenerationValidationResult success() { return {true, {}}; }
    static GenerationValidationResult failure(std::string message) {
        return {false, std::move(message)};
    }
};

using GenerationValidator = std::function<GenerationValidationResult(
    const std::filesystem::path& generation_gdb_path)>;

namespace detail {
struct VersionedGdbStoreState;
}

class GdbReaderSnapshot {
public:
    GdbReaderSnapshot() = default;
    ~GdbReaderSnapshot();

    GdbReaderSnapshot(GdbReaderSnapshot&& other) noexcept;
    GdbReaderSnapshot& operator=(GdbReaderSnapshot&& other) noexcept;
    GdbReaderSnapshot(const GdbReaderSnapshot&) = delete;
    GdbReaderSnapshot& operator=(const GdbReaderSnapshot&) = delete;

    bool valid() const noexcept { return state_ != nullptr; }
    const std::string& generation() const noexcept { return generation_; }
    const std::filesystem::path& path() const noexcept { return path_; }

    // The caller must close every QueryEngine, cursor, file descriptor and mmap
    // opened from path() before refreshing this snapshot.
    bool refresh();

private:
    friend class VersionedGdbStore;
    GdbReaderSnapshot(std::shared_ptr<detail::VersionedGdbStoreState> state,
                      std::string generation,
                      std::filesystem::path path);
    void release() noexcept;

    std::shared_ptr<detail::VersionedGdbStoreState> state_;
    std::string generation_;
    std::filesystem::path path_;
};

class GdbWriteTransaction {
public:
    GdbWriteTransaction() = default;
    ~GdbWriteTransaction();

    GdbWriteTransaction(GdbWriteTransaction&& other) noexcept;
    GdbWriteTransaction& operator=(GdbWriteTransaction&& other) noexcept;
    GdbWriteTransaction(const GdbWriteTransaction&) = delete;
    GdbWriteTransaction& operator=(const GdbWriteTransaction&) = delete;

    bool valid() const noexcept { return state_ != nullptr && !completed_; }
    const std::string& source_generation() const noexcept {
        return source_generation_;
    }
    const std::string& generation() const noexcept { return generation_; }
    const std::filesystem::path& working_path() const noexcept {
        return working_path_;
    }
    GdbCloneStrategy clone_strategy() const noexcept { return clone_strategy_; }
    GdbPublishState publish_state() const noexcept { return publish_state_; }
    bool published() const noexcept {
        return publish_state_ != GdbPublishState::NotPublished;
    }
    const std::string& last_error() const noexcept { return last_error_; }

    // All handles targeting working_path() must be closed before this call.
    // A false return with published()==true means CURRENT switched but the final
    // durability barrier failed. The transaction is terminal; recover the store
    // before starting another writer.
    bool publish(const GenerationValidator& validator);

    bool abort();

private:
    friend class VersionedGdbStore;
    GdbWriteTransaction(
        std::shared_ptr<detail::VersionedGdbStoreState> state,
        std::string source_generation,
        std::string generation,
        std::filesystem::path working_path,
        GdbCloneStrategy clone_strategy);

    bool fail(std::string message);
    void release_writer_gate() noexcept;

    std::shared_ptr<detail::VersionedGdbStoreState> state_;
    std::string source_generation_;
    std::string generation_;
    std::filesystem::path working_path_;
    GdbCloneStrategy clone_strategy_ = GdbCloneStrategy::FullCopy;
    GdbPublishState publish_state_ = GdbPublishState::NotPublished;
    std::string last_error_;
    bool completed_ = false;
};

class VersionedGdbStore {
public:
    explicit VersionedGdbStore(std::filesystem::path root);

    bool open();

    bool initialize_from(const std::filesystem::path& source_gdb_path,
                         const GenerationValidator& validator);

    GdbReaderSnapshot acquire_reader();

    GdbWriteTransaction begin_write();

    // No reader or writer may be active. This also resolves a prior
    // PublishedDurabilityUncertain state before writes are enabled again.
    bool recover();

    const std::filesystem::path& root() const noexcept;
    std::string current_generation() const;
    std::string last_error() const;

private:
    bool ensure_open();
    bool fail(std::string message);

    std::shared_ptr<detail::VersionedGdbStoreState> state_;
};

}  // namespace writer
}  // namespace explorgdb

#endif  // FAST_GDB_PUBLIC_VERSIONED_GDB_STORE_H
