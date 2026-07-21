#include "versioned_gdb_store_internal.h"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace explorgdb {
namespace writer {
namespace detail {

namespace {
using RegistryKey = fs::path::string_type;

std::mutex g_registry_mutex;
std::unordered_map<RegistryKey, std::weak_ptr<StoreState>> g_registry;
std::atomic<uint64_t> g_name_sequence{0};

fs::path canonical_store_root(const fs::path& input) {
    std::error_code error;
    fs::path canonical = fs::weakly_canonical(input, error);
    if (!error) return canonical.lexically_normal();
    error.clear();
    fs::path absolute = fs::absolute(input, error);
    return error ? input.lexically_normal() : absolute.lexically_normal();
}

RegistryKey registry_key(const fs::path& canonical_root) {
    RegistryKey key = canonical_root.native();
#if defined(_WIN32)
    for (auto& character : key) {
        if (character >= L'A' && character <= L'Z') {
            character = static_cast<wchar_t>(character - L'A' + L'a');
        }
    }
#endif
    return key;
}

std::string unique_token() {
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const uint64_t sequence =
        g_name_sequence.fetch_add(1, std::memory_order_relaxed);
    return std::to_string(ticks) + "-" + std::to_string(sequence);
}

bool valid_generation_name(const std::string& name) {
    return name.size() >= 9 && name.rfind("gen-", 0) == 0 &&
           fs::path(name).filename().string() == name &&
           fs::path(name).extension() == ".gdb";
}

bool flush_file(const fs::path& path, std::string& error_message) {
#if defined(_WIN32)
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE,
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
    if (!ok) error_message = "FlushFileBuffers failed: " + std::to_string(code);
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

}  // namespace

std::shared_ptr<StoreState> state_for_root(const fs::path& root) {
    const fs::path canonical = canonical_store_root(root);
    const RegistryKey key = registry_key(canonical);
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    const auto found = g_registry.find(key);
    if (found != g_registry.end()) {
        if (auto existing = found->second.lock()) return existing;
        g_registry.erase(found);
    }
    auto created = std::make_shared<StoreState>(canonical);
    g_registry.emplace(key, created);
    return created;
}

std::string new_generation_name() {
    return "gen-" + unique_token() + ".gdb";
}

void set_error(const std::shared_ptr<StoreState>& state, std::string message) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->last_error = std::move(message);
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
    for (fs::recursive_directory_iterator it(root, error), end;
         !error && it != end; it.increment(error)) {
        const fs::file_status status = it->symlink_status(error);
        if (error) break;
        if (fs::is_symlink(status)) {
            error_message = "candidate contains a symbolic link: " +
                            it->path().string();
            return false;
        }
        if (fs::is_regular_file(status)) {
            if (!flush_file(it->path(), error_message)) return false;
        } else if (!fs::is_directory(status)) {
            error_message = "candidate contains an unsupported file type: " +
                            it->path().string();
            return false;
        }
    }
    if (error) {
        error_message = "enumerating candidate for sync failed: " + error.message();
        return false;
    }
#if !defined(_WIN32)
    for (fs::recursive_directory_iterator it(root, error), end;
         !error && it != end; it.increment(error)) {
        const fs::file_status status = it->symlink_status(error);
        if (error) break;
        if (fs::is_directory(status) &&
            !flush_directory(it->path(), error_message)) {
            return false;
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

ManifestSwitchResult write_current_manifest(
    const std::shared_ptr<StoreState>& state,
    const std::string& generation,
    std::string& error_message) {
    const fs::path temporary = state->root / ("CURRENT.tmp-" + unique_token());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error_message = "cannot create temporary CURRENT manifest";
            return ManifestSwitchResult::NotSwitched;
        }
        output << generation << '\n';
        output.flush();
        if (!output) {
            error_message = "cannot flush temporary CURRENT manifest";
            output.close();
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return ManifestSwitchResult::NotSwitched;
        }
    }

    if (!flush_file(temporary, error_message) ||
        !atomic_replace(temporary, state->current_manifest, error_message)) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return ManifestSwitchResult::NotSwitched;
    }
    if (!flush_directory(state->root, error_message)) {
        return ManifestSwitchResult::SwitchedDurabilityUncertain;
    }
    return ManifestSwitchResult::SwitchedDurable;
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
    if (!input) {
        error_message = "cannot read CURRENT manifest";
        return false;
    }
    const std::string contents{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
    if (input.bad() || contents.empty() || contents.size() > 512 ||
        contents.back() != '\n') {
        error_message = "CURRENT manifest has an invalid physical format";
        return false;
    }
    if (contents.find('\n') != contents.size() - 1) {
        error_message = "CURRENT manifest contains more than one line";
        return false;
    }
    generation.assign(contents.data(), contents.size() - 1);
    if (!generation.empty() && generation.back() == '\r') generation.pop_back();
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

}  // namespace detail
}  // namespace writer
}  // namespace explorgdb
