#pragma once

#include <filesystem>

namespace explorgdb_test_paths {

inline std::filesystem::path repo_root() {
    namespace fs = std::filesystem;
    static const fs::path root = []() {
        fs::path cur = fs::current_path();
        for (int depth = 0; depth < 6; ++depth) {
            if (fs::exists(cur / "CMakeLists.txt") &&
                fs::exists(cur / "test_data")) {
                return cur;
            }
            if (!cur.has_parent_path()) break;
            cur = cur.parent_path();
        }
        return fs::current_path();
    }();
    return root;
}

inline std::filesystem::path test_data_path(const std::filesystem::path& rel) {
    return repo_root() / rel;
}

} // namespace explorgdb_test_paths
