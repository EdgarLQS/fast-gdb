#include "versioned_gdb_store_internal.h"

#include <exception>
#include <utility>

namespace explorgdb {
namespace writer {
namespace detail {
namespace {

bool remove_directory_contents(const fs::path& directory,
                               const std::string& description,
                               std::string& error_message) {
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code remove_error;
        fs::remove_all(it->path(), remove_error);
        if (remove_error) {
            error_message = "cannot remove " + description + " " +
                            it->path().string() + ": " +
                            remove_error.message();
            return false;
        }
    }
    if (error) {
        error_message = "cannot enumerate " + description + ": " +
                        error.message();
        return false;
    }
    return true;
}

}  // namespace

bool cleanup_generations_locked(const std::shared_ptr<StoreState>& state,
                                std::string& error_message) {
    if (state->current_durability_uncertain) return true;

    std::error_code error;
    for (fs::directory_iterator it(state->generations, error), end;
         !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name == state->current_generation) continue;
        const auto readers = state->reader_counts.find(name);
        if (readers != state->reader_counts.end() && readers->second != 0) {
            continue;
        }

        std::error_code remove_error;
        fs::remove_all(it->path(), remove_error);
        if (remove_error) {
            error_message = "cannot remove obsolete generation " +
                            it->path().string() + ": " +
                            remove_error.message();
            return false;
        }
    }
    if (error) {
        error_message = "cannot enumerate generations for cleanup: " +
                        error.message();
        return false;
    }
    return true;
}

bool recover_locked(const std::shared_ptr<StoreState>& state,
                    std::string& error_message) {
    std::error_code error;
    fs::create_directories(state->generations, error);
    if (error) {
        error_message = "cannot create generations directory: " + error.message();
        return false;
    }
    fs::create_directories(state->work, error);
    if (error) {
        error_message = "cannot create work directory: " + error.message();
        return false;
    }
    if (!remove_directory_contents(state->work, "stale work entry",
                                   error_message)) {
        return false;
    }

    for (fs::directory_iterator it(state->root, error), end;
         !error && it != end; it.increment(error)) {
        if (it->path().filename().string().rfind("CURRENT.tmp-", 0) != 0) {
            continue;
        }
        std::error_code remove_error;
        fs::remove_all(it->path(), remove_error);
        if (remove_error) {
            error_message = "cannot remove temporary manifest " +
                            it->path().string() + ": " +
                            remove_error.message();
            return false;
        }
    }
    if (error) {
        error_message = "cannot clean temporary manifests: " + error.message();
        return false;
    }

    std::string current;
    if (!read_current_manifest(state, current, error_message)) return false;

    if (!flush_directory(state->generations, error_message) ||
        !flush_directory(state->work, error_message) ||
        !flush_directory(state->root, error_message)) {
        return false;
    }
    state->current_generation = current;
    state->current_durability_uncertain = false;
    if (!cleanup_generations_locked(state, error_message)) return false;
    if (!flush_directory(state->generations, error_message)) return false;

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

std::string durability_uncertain_message(const std::string& detail) {
    return "CURRENT switched, but root directory durability is uncertain; "
           "old generations were retained and recover() is required before the "
           "next Writer: " + detail;
}

std::string append_cleanup_error(std::string message,
                                 const std::error_code& cleanup_error) {
    if (!cleanup_error) return message;
    return std::move(message) + "; candidate cleanup also failed: " +
           cleanup_error.message();
}

}  // namespace detail
}  // namespace writer
}  // namespace explorgdb
