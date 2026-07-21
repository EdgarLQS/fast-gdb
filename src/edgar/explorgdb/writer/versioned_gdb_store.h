// src/edgar/explorgdb/writer/versioned_gdb_store.h
// Immutable-generation FileGDB publication with reader snapshot leases.

#ifndef EXPLORGDB_VERSIONED_GDB_STORE_H
#define EXPLORGDB_VERSIONED_GDB_STORE_H

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace explorgdb {
namespace writer {

/** The effective strategy used while creating a working generation. */
enum class GdbCloneStrategy {
    CopyOnWrite,
    FullCopy,
};

/** Publication state remains inspectable when publish() reports uncertainty. */
enum class GdbPublishState {
    NotPublished,
    PublishedDurable,
    PublishedDurabilityUncertain,
};

/** Result returned by the mandatory pre-publication validator. */
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

/**
 * Pins one immutable generation for a Reader.
 *
 * The snapshot must outlive every QueryEngine, GdbCatalog and mmap opened from
 * path(). refresh() is explicit: existing readers never move underneath active
 * cursors or mappings.
 */
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

    /** Rebind this idle reader to CURRENT. Active QueryEngine objects must close first. */
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

/**
 * Exclusive writer transaction targeting a private working GDB.
 *
 * The caller opens WriterSession/GdbTableWriter only against working_path().
 * publish() validates the closed files, promotes the directory to an immutable
 * generation and atomically replaces CURRENT. Destruction aborts unpublished work.
 */
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

    /**
     * Publish only after every Writer and file handle targeting working_path()
     * has been closed. The validator must reopen the candidate and verify the
     * required record count, FIDs, geometry and indexes.
     *
     * A false return with published()==true means CURRENT switched but the final
     * root-directory durability barrier failed. The transaction is terminal;
     * call VersionedGdbStore::recover() before starting another Writer.
     */
    bool publish(const GenerationValidator& validator);

    /** Remove unpublished work and release the single-writer gate. */
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

/**
 * Managed entry point for all access to one versioned FileGDB repository.
 *
 * Layout:
 *   <root>/CURRENT
 *   <root>/generations/gen-*.gdb
 *   <root>/work/work-*.gdb
 *
 * Concurrency scope is one process: all VersionedGdbStore instances for the
 * same canonical root share one reader-lease registry and one writer gate.
 */
class VersionedGdbStore {
public:
    explicit VersionedGdbStore(std::filesystem::path root);

    /** Create layout, validate CURRENT and remove crash leftovers. */
    bool open();

    /** Bootstrap an empty store from an existing GDB without modifying it. */
    bool initialize_from(const std::filesystem::path& source_gdb_path,
                         const GenerationValidator& validator);

    /** Acquire a stable snapshot of CURRENT for a Reader. */
    GdbReaderSnapshot acquire_reader();

    /** Clone CURRENT into private working storage and acquire the writer gate. */
    GdbWriteTransaction begin_write();

    /**
     * Repeat crash cleanup and CURRENT validation. No readers/writer may be active.
     * Also resolves a prior CURRENT durability-uncertain state before allowing writes.
     */
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

#endif  // EXPLORGDB_VERSIONED_GDB_STORE_H
