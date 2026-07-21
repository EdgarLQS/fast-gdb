// src/edgar/explorgdb/writer/versioned_gdb_store.cpp

#include "versioned_gdb_store.h"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <exception>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <utility>

#if defined(__APPLE__)
#include <copyfile.h>
#include <fcntl.h>
#include <unistd.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace explorgdb {
namespace writer {

namespace fs = std::filesystem;

namespace detail {

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
};

}  // namespace detail

namespace {

using StoreState = detail::VersionedGdbStoreState;

std::mutex g_registry_mutex;
std::unordered_map<std::string, std::weak_ptr<StoreState>> g_registry;
std::atomic<uint64_t> g_name_sequence{0};

fs::path normalized_absolute(const fs::path& input) {
    std::error_code error;
    fs::path absolute = fs::absolute(input, error);
    if (error) return input.lexically_normal();
    return absolute.lexically_normal();
}

std::shared_ptr<StoreState> state_for_root(const fs::path& root) {
    const fs::path canonical = normalized_absolute(root);
    const std::string key = canonical.generic_string();
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    const auto found = g_registry.find(key);
    if (found != g_registry.end()) {
        if (auto existing = found->second.lock()) return existing;
    }
    auto created = std::make_shared<StoreState>(canonical);
    g_registry[key] = created;
    return created;
}

std::string unique_token() {
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const uint64_t sequence =
        g_name_sequence.fetch_add(1, std::memory_order_relaxed);
    return std::to_string(ticks) + "-" + std::to_string(sequence);
}

std::string new_generation_name() {
    return "gen-" + unique_token() + ".gdb";
}

bool valid_generation_name(const std::string& name) {
    if (name.size() < 9 || name.rfind("gen-", 0) != 0) return false;
    if (fs::path(name).filename().string() != name) return false;
    return fs::path(name).extension() == ".gdb";
}

void set_error(const std::shared_ptr<StoreState>& state, std::string message) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->last_error = std::move(message);
}

bool flush_file(const fs::path& path, std::string& error_message) {
#if defined(_WIN32)
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE |
                                      FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error_message = "open for FlushFileBuffers failed: " +
                        std::to_string(::GetLastError());
        return false;
    }
    const bool ok = ::FlushFileBuffers(handle) != 0;
    const DWORD code = ok ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(handle);
    if (!ok) {
        error_message = "FlushFileBuffers failed: " + std::to_string(code);
    }
    return ok;
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        error_message = "open for fsync failed: " +
                        std::error_code(errno, std::generic_category()).message();
        return false;
    }
    const bool ok = ::fsync(fd) == 0;
    const int code = ok ? 0 : errno;
    ::close(fd);
    if (!ok) {
        error_message = "fsync failed: " +
                        std::error_code(code, std::generic_category()).message();
    }
    return ok;
#endif
}

bool flush_directory(const fs::path& path, std::string& error_message) {
#if defined(_WIN32)
    (void)path;
    (void)error_message;
    return true;
#else
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        error_message = "open directory for fsync failed: " +
                        std::error_code(errno, std::generic_category()).message();
        return false;
    }
    const bool ok = ::fsync(fd) == 0;
    const int code = ok ? 0 : errno;
    ::close(fd);
    if (!ok) {
        error_message = "directory fsync failed: " +
                        std::error_code(code, std::generic_category()).message();
    }
    return ok;
#endif
}

bool sync_tree(const fs::path& root, std::string& error_message) {
    std::error_code error;
    for (fs::recursive_directory_iterator iterator(root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file(error)) {
            if (error || !flush_file(iterator->path(), error_message)) {
                return false;
            }
        }
    }
    if (error) {
        error_message = "enumerating candidate for sync failed: " +
                        error.message();
        return false;
    }

#if !defined(_WIN32)
    for (fs::recursive_directory_iterator iterator(root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_directory(error)) {
            if (error || !flush_directory(iterator->path(), error_message)) {
                return false;
            }
        }
    }
    if (error) {
        error_message = "enumerating candidate directories failed: " +
                        error.message();
        return false;
    }
#endif
    return flush_directory(root, error_message);
}

bool atomic_replace(const fs::path& source, const fs::path& destination,
                    std::string& error_message) {
#if defined(_WIN32)
    if (::MoveFileExW(source.c_str(), destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return true;
    }
    error_message = "MoveFileExW(CURRENT) failed: " +
                    std::to_string(::GetLastError());
    return false;
#else
    if (::rename(source.c_str(), destination.c_str()) == 0) return true;
    error_message = "rename(CURRENT) failed: " +
                    std::error_code(errno, std::generic_category()).message();
    return false;
#endif
}

bool write_current_manifest(const std::shared_ptr<StoreState>& state,
                            const std::string& generation,
                            std::string& error_message) {
    const fs::path temporary =
        state->root / ("CURRENT.tmp-" + unique_token());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error_message = "cannot create temporary CURRENT manifest";
            return false;
        }
        output << generation << '\n';
        output.flush();
        if (!output) {
            error_message = "cannot flush temporary CURRENT manifest";
            output.close();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return false;
        }
    }

    if (!flush_file(temporary, error_message) ||
        !atomic_replace(temporary, state->current_manifest, error_message) ||
        !flush_directory(state->root, error_message)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

bool read_current_manifest(const std::shared_ptr<StoreState>& state,
                           std::string& generation,
                           std::string& error_message) {
    std::error_code error;
    if (!fs::exists(state->current_manifest, error)) {
        if (error) {
            error_message = "cannot inspect CURRENT: " + error.message();
            return false;
        }
        generation.clear();
        return true;
    }

    std::ifstream input(state->current_manifest, std::ios::binary);
    if (!input || !std::getline(input, generation)) {
        error_message = "cannot read CURRENT manifest";
        return false;
    }
    if (!generation.empty() && generation.back() == '\r') {
        generation.pop_back();
    }
    if (!valid_generation_name(generation)) {
        error_message = "CURRENT contains an invalid generation name";
        return false;
    }
    const fs::path generation_path = state->generations / generation;
    if (!fs::is_directory(generation_path, error) || error) {
        error_message = "CURRENT generation does not exist: " +
                        generation_path.string();
        return false;
    }
    return true;
}

bool native_clone_file(const fs::path& source, const fs::path& destination) {
#if defined(__APPLE__)
    return ::clonefile(source.c_str(), destination.c_str(), 0) == 0;
#elif defined(__linux__)
    const int source_fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    if (source_fd < 0) return false;
    const int destination_fd = ::open(destination.c_str(),
                                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                                      0600);
    if (destination_fd < 0) {
        ::close(source_fd);
        return false;
    }
    const bool cloned = ::ioctl(destination_fd, FICLONE, source_fd) == 0;
    ::close(destination_fd);
    ::close(source_fd);
    if (!cloned) {
        std::error_code ignored;
        fs::remove(destination, ignored);
    }
    return cloned;
#else
    (void)source;
    (void)destination;
    return false;
#endif
}

bool clone_regular_file(const fs::path& source, const fs::path& destination,
                        bool& used_full_copy, std::string& error_message) {
    if (!native_clone_file(source, destination)) {
        used_full_copy = true;
        std::error_code ignored;
        fs::remove(destination, ignored);
        std::error_code error;
        if (!fs::copy_file(source, destination, fs::copy_options::none, error)) {
            error_message = "copy failed for " + source.string() + ": " +
                            error.message();
            return false;
        }
    }

    std::error_code permission_error;
    const fs::file_status source_status = fs::status(source, permission_error);
    if (!permission_error) {
        fs::permissions(destination, source_status.permissions(), permission_error);
    }
    return true;
}

bool clone_tree(const fs::path& source, const fs::path& destination,
                GdbCloneStrategy& strategy, std::string& error_message) {
    std::error_code error;
    if (!fs::is_directory(source, error) || error) {
        error_message = "source GDB is not a directory: " + source.string();
        return false;
    }
    if (!fs::create_directories(destination, error) && error) {
        error_message = "cannot create working GDB: " + error.message();
        return false;
    }

    bool used_full_copy = false;
    for (fs::recursive_directory_iterator iterator(source, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const fs::path relative = fs::relative(iterator->path(), source, error);
        if (error) break;
        const fs::path target = destination / relative;
        const fs::file_status status = iterator->symlink_status(error);
        if (error) break;
        if (fs::is_directory(status)) {
            fs::create_directories(target, error);
        } else if (fs::is_regular_file(status)) {
            if (!clone_regular_file(iterator->path(), target, used_full_copy,
                                    error_message)) {
                return false;
            }
        } else if (fs::is_symlink(status)) {
            fs::copy_symlink(iterator->path(), target, error);
            used_full_copy = true;
        } else {
            error_message = "unsupported file type in GDB: " +
                            iterator->path().string();
            return false;
        }
        if (error) break;
    }
    if (error) {
        error_message = "cloning GDB failed: " + error.message();
        return false;
    }
    strategy = used_full_copy ? GdbCloneStrategy::FullCopy
                              : GdbCloneStrategy::CopyOnWrite;
    return true;
}

void cleanup_generations_locked(const std::shared_ptr<StoreState>& state) {
    std::error_code error;
    for (fs::directory_iterator iterator(state->generations, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_directory(error)) continue;
        const std::string name = iterator->path().filename().string();
        if (name == state->current_generation) continue;
        const auto readers = state->reader_counts.find(name);
        if (readers != state->reader_counts.end() && readers->second != 0) {
            continue;
        }
        std::error_code ignored;
        fs::remove_all(iterator->path(), ignored);
    }
}

bool recover_locked(const std::shared_ptr<StoreState>& state,
                    std::string& error_message) {
    std::error_code error;
    fs::create_directories(state->generations, error);
    if (error) {
        error_message = "cannot create generations directory: " +
                        error.message();
        return false;
    }
    fs::create_directories(state->work, error);
    if (error) {
        error_message = "cannot create work directory: " + error.message();
        return false;
    }

    for (fs::directory_iterator iterator(state->work, error), end;
         !error && iterator != end; iterator.increment(error)) {
        std::error_code ignored;
        fs::remove_all(iterator->path(), ignored);
    }
    if (error) {
        error_message = "cannot clean stale work directories: " +
                        error.message();
        return false;
    }

    for (fs::directory_iterator iterator(state->root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const std::string name = iterator->path().filename().string();
        if (name.rfind("CURRENT.tmp-", 0) == 0) {
            std::error_code ignored;
            fs::remove_all(iterator->path(), ignored);
        }
    }
    if (error) {
        error_message = "cannot clean temporary manifests: " +
                        error.message();
        return false;
    }

    std::string current;
    if (!read_current_manifest(state, current, error_message)) return false;
    state->current_generation = current;

    // Recovery runs only without live leases. Non-current directories are
    // unpublished crash leftovers or generations whose readers died with the process.
    cleanup_generations_locked(state);
    state->opened = true;
    state->last_error.clear();
    return true;
}

GenerationValidationResult run_validator(const GenerationValidator& validator,
                                         const fs::path& path) {
    if (!validator) {
        return GenerationValidationResult::failure(
            "publication requires a reopen validator");
    }
    try {
        return validator(path);
    } catch (const std::exception& exception) {
        return GenerationValidationResult::failure(
            std::string("validator threw: ") + exception.what());
    } catch (...) {
        return GenerationValidationResult::failure(
            "validator threw a non-standard exception");
    }
}

}  // namespace

GdbReaderSnapshot::GdbReaderSnapshot(std::shared_ptr<StoreState> state,
                                     std::string generation,
                                     fs::path path)
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
        cleanup_generations_locked(state);
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
    if (generation_ == state_->current_generation) return true;

    const fs::path next_path = state_->generations / state_->current_generation;
    std::error_code error;
    if (!fs::is_directory(next_path, error) || error) {
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
    cleanup_generations_locked(state_);
    return true;
}

GdbWriteTransaction::GdbWriteTransaction(
    std::shared_ptr<StoreState> state, std::string source_generation,
    std::string generation, fs::path working_path,
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
    last_error_ = std::move(other.last_error_);
    completed_ = other.completed_;
    other.completed_ = true;
    return *this;
}

bool GdbWriteTransaction::fail(std::string message) {
    last_error_ = std::move(message);
    if (state_) set_error(state_, last_error_);
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
        run_validator(validator, working_path_);
    if (!validation.ok) {
        return fail("candidate validation failed: " + validation.message);
    }

    std::string durability_error;
    if (!sync_tree(working_path_, durability_error)) {
        return fail("candidate durability sync failed: " + durability_error);
    }

    const fs::path generation_path = state_->generations / generation_;
    std::error_code error;
    fs::rename(working_path_, generation_path, error);
    if (error) {
        return fail("promoting immutable generation failed: " + error.message());
    }
    if (!flush_directory(state_->generations, durability_error)) {
        std::error_code ignored;
        fs::remove_all(generation_path, ignored);
        completed_ = true;
        release_writer_gate();
        return fail("generation directory sync failed: " + durability_error);
    }

    std::string manifest_error;
    if (!write_current_manifest(state_, generation_, manifest_error)) {
        // CURRENT was not replaced, therefore the old version remains authoritative.
        std::error_code ignored;
        fs::remove_all(generation_path, ignored);
        completed_ = true;
        release_writer_gate();
        return fail("CURRENT switch failed; candidate rolled back: " +
                    manifest_error);
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->current_generation = generation_;
        state_->writer_active = false;
        state_->last_error.clear();
        completed_ = true;
        cleanup_generations_locked(state_);
    }
    last_error_.clear();
    return true;
}

bool GdbWriteTransaction::abort() {
    if (!state_ || completed_) return true;
    std::error_code error;
    fs::remove_all(working_path_, error);
    completed_ = true;
    release_writer_gate();
    if (error) return fail("removing working GDB failed: " + error.message());
    return true;
}

VersionedGdbStore::VersionedGdbStore(fs::path root)
    : state_(state_for_root(root)) {}

bool VersionedGdbStore::fail(std::string message) {
    set_error(state_, std::move(message));
    return false;
}

bool VersionedGdbStore::open() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->opened) return true;
    std::string error;
    if (!recover_locked(state_, error)) {
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
    const fs::path& source_gdb_path, const GenerationValidator& validator) {
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
        state_->writer_active = true;
        generation = new_generation_name();
        working = state_->work / ("work-" + generation);
    }

    auto rollback = [&](const std::string& message) {
        std::error_code ignored;
        fs::remove_all(working, ignored);
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->writer_active = false;
            state_->last_error = message;
        }
        return false;
    };

    GdbCloneStrategy strategy = GdbCloneStrategy::FullCopy;
    std::string clone_error;
    if (!clone_tree(source_gdb_path, working, strategy, clone_error)) {
        return rollback("bootstrap clone failed: " + clone_error);
    }
    (void)strategy;
    const GenerationValidationResult validation =
        run_validator(validator, working);
    if (!validation.ok) {
        return rollback("bootstrap validation failed: " + validation.message);
    }

    std::string durability_error;
    if (!sync_tree(working, durability_error)) {
        return rollback("bootstrap durability sync failed: " +
                        durability_error);
    }

    const fs::path generation_path = state_->generations / generation;
    std::error_code error;
    fs::rename(working, generation_path, error);
    if (error) {
        return rollback("bootstrap promotion failed: " + error.message());
    }
    if (!flush_directory(state_->generations, durability_error)) {
        std::error_code ignored;
        fs::remove_all(generation_path, ignored);
        return rollback("bootstrap generation sync failed: " +
                        durability_error);
    }

    std::string manifest_error;
    if (!write_current_manifest(state_, generation, manifest_error)) {
        std::error_code ignored;
        fs::remove_all(generation_path, ignored);
        return rollback("bootstrap CURRENT switch failed: " + manifest_error);
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->current_generation = generation;
        state_->writer_active = false;
        state_->last_error.clear();
    }
    return true;
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
        state_->writer_active = true;
        source_generation = state_->current_generation;
        generation = new_generation_name();
        source = state_->generations / source_generation;
        working = state_->work / ("work-" + generation);
    }

    GdbCloneStrategy strategy = GdbCloneStrategy::FullCopy;
    std::string clone_error;
    if (!clone_tree(source, working, strategy, clone_error)) {
        std::error_code ignored;
        fs::remove_all(working, ignored);
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->writer_active = false;
            state_->last_error = "working clone failed: " + clone_error;
        }
        return {};
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
    if (!recover_locked(state_, error)) {
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
