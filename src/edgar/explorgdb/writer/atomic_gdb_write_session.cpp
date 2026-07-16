#include "atomic_gdb_write_session.h"

#include <cerrno>
#include <filesystem>
#include <system_error>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <cstdio>
#elif defined(_WIN32)
#include "windows_posix_compat.h"
#endif

namespace explorgdb {
namespace writer {

namespace fs = std::filesystem;

namespace {

bool rename_exclusive(const fs::path& source, const fs::path& destination,
                      std::error_code& error) {
#if defined(__APPLE__)
    if (::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0) {
        return true;
    }
    error = std::error_code(errno, std::generic_category());
#elif defined(__linux__)
#if defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD,
                  destination.c_str(), RENAME_NOREPLACE) == 0) {
        return true;
    }
    error = std::error_code(errno, std::generic_category());
#else
    error = std::make_error_code(std::errc::operation_not_supported);
#endif
#elif defined(_WIN32)
    // 不使用 exists() 预检查，避免检查与发布之间的竞态窗口。
    // 不传 MOVEFILE_REPLACE_EXISTING 时，目标已存在会原子失败。
    if (::MoveFileExW(source.c_str(), destination.c_str(), 0) != 0) {
        return true;
    }
    const DWORD windows_error = ::GetLastError();
    if (windows_error == ERROR_FILE_EXISTS ||
        windows_error == ERROR_ALREADY_EXISTS) {
        error = std::make_error_code(std::errc::file_exists);
    } else {
        error = std::error_code(static_cast<int>(windows_error),
                                std::system_category());
    }
#else
    error = std::make_error_code(std::errc::operation_not_supported);
#endif
    return false;
}

}  // namespace

AtomicGdbWriteSession::~AtomicGdbWriteSession() {
    if (writer_.is_open()) writer_.close();
}

bool AtomicGdbWriteSession::fail(const std::string& message) {
    last_error_ = "[atomic writer] " + message;
    return false;
}

bool AtomicGdbWriteSession::adopt_open_writer(
    const std::string& staging_gdb_path) {
    if (adopted_) return fail("a Writer has already been adopted");
    if (!writer_.is_open()) return fail("Writer is not open");

    std::error_code error;
    const fs::path staging = fs::weakly_canonical(staging_gdb_path, error);
    if (error || !fs::is_directory(staging, error)) {
        return fail("staging GDB is not a readable directory: " +
                    staging_gdb_path);
    }
    const fs::path table = fs::weakly_canonical(
        writer_.data_table_path(), error);
    if (error || table.parent_path() != staging) {
        return fail("Writer table is outside the staging GDB");
    }

    staging_gdb_path_ = staging.string();
    adopted_ = true;
    last_error_.clear();
    return true;
}

bool AtomicGdbWriteSession::validate_publish_paths(
    const std::string& final_gdb_path) {
    std::error_code error;
    const fs::path staging_parent =
        fs::path(staging_gdb_path_).parent_path();
    const fs::path final_parent =
        fs::weakly_canonical(fs::path(final_gdb_path).parent_path(), error);
    if (error || staging_parent != final_parent) {
        return fail("staging and final GDB must share the same parent directory");
    }
    return true;
}

bool AtomicGdbWriteSession::commit(const std::string& final_gdb_path) {
    if (!adopted_) return fail("no open Writer has been adopted");
    if (committed_) return fail("session is already committed");
    if (!writer_.is_open()) return fail("Writer was closed outside the session");

    const bool had_earlier_error = !writer_.last_error().empty();
    const bool closed = writer_.close();
    if (had_earlier_error) {
        return fail("Writer reported an earlier error; staging GDB was not published");
    }
    if (!closed) {
        return fail("Writer close failed: " + writer_.last_error());
    }
    if (!validate_publish_paths(final_gdb_path)) return false;

    std::error_code error;
    if (!rename_exclusive(staging_gdb_path_, final_gdb_path, error)) {
        if (error == std::errc::file_exists) {
            return fail("final GDB already exists: " + final_gdb_path);
        }
        return fail("atomic directory rename failed: " + error.message());
    }
    committed_ = true;
    return true;
}

}  // namespace writer
}  // namespace explorgdb
