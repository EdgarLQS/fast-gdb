#include "versioned_gdb_store_internal.h"

#include <system_error>

#if defined(__APPLE__)
#include <copyfile.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace explorgdb {
namespace writer {
namespace detail {
namespace {

fs::path canonical_path(const fs::path& input) {
    std::error_code error;
    fs::path canonical = fs::weakly_canonical(input, error);
    if (!error) return canonical.lexically_normal();
    error.clear();
    fs::path absolute = fs::absolute(input, error);
    return error ? input.lexically_normal() : absolute.lexically_normal();
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

    std::error_code error;
    const fs::perms source_permissions = fs::status(source, error).permissions();
    if (error) {
        error_message = "cannot read source permissions for " + source.string() +
                        ": " + error.message();
        return false;
    }
    const fs::perms working_permissions =
        source_permissions | fs::perms::owner_read | fs::perms::owner_write;
    fs::permissions(destination, working_permissions, fs::perm_options::replace,
                    error);
    if (error) {
        error_message = "cannot make working file writable: " +
                        destination.string() + ": " + error.message();
        return false;
    }
    return true;
}

bool contains_parent_component(const fs::path& relative) {
    for (const fs::path& component : relative) {
        if (component == "..") return true;
    }
    return false;
}

bool path_is_within(const fs::path& candidate, const fs::path& parent) {
    const fs::path relative = candidate.lexically_relative(parent);
    return !relative.empty() && relative != "." &&
           !contains_parent_component(relative);
}

}  // namespace

bool clone_tree(const fs::path& source, const fs::path& destination,
                GdbCloneStrategy& strategy, std::string& error_message) {
    std::error_code error;
    const fs::path source_root = fs::weakly_canonical(source, error);
    if (error || !fs::is_directory(source_root, error) || error) {
        error_message = "source GDB is not a readable directory: " +
                        source.string();
        return false;
    }
    const fs::path destination_root = canonical_path(destination);
    if (source_root == destination_root ||
        path_is_within(destination_root, source_root)) {
        error_message = "working GDB must not be inside the source GDB";
        return false;
    }
    if (!fs::create_directories(destination_root, error) && error) {
        error_message = "cannot create working GDB: " + error.message();
        return false;
    }

    bool used_full_copy = false;
    for (fs::recursive_directory_iterator it(source_root, error), end;
         !error && it != end; it.increment(error)) {
        const fs::file_status status = it->symlink_status(error);
        if (error) break;
        if (fs::is_symlink(status)) {
            error_message = "symbolic links are not allowed in a managed GDB: " +
                            it->path().string();
            return false;
        }

        const fs::path relative = it->path().lexically_relative(source_root);
        if (relative.empty() || relative == "." ||
            contains_parent_component(relative)) {
            error_message = "source entry escaped the GDB root: " +
                            it->path().string();
            return false;
        }
        const fs::path target = destination_root / relative;
        if (fs::is_directory(status)) {
            fs::create_directories(target, error);
        } else if (fs::is_regular_file(status)) {
            if (!clone_regular_file(it->path(), target, used_full_copy,
                                    error_message)) {
                return false;
            }
        } else {
            error_message = "unsupported file type in GDB: " +
                            it->path().string();
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

}  // namespace detail
}  // namespace writer
}  // namespace explorgdb
