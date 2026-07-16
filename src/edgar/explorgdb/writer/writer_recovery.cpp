#include "writer_recovery.h"

#include <filesystem>
#include <system_error>

namespace explorgdb {
namespace writer {
namespace fs = std::filesystem;

namespace {
void set_error(WriterError* error, WriterErrorCode code,
               const std::string& path, const std::string& reason,
               bool retryable = false) {
    if (!error) return;
    error->stage = WriterStage::Publish;
    error->code = code;
    error->path = path;
    error->system_reason = reason;
    error->message = "[writer recovery] " + reason + " in '" + path + "'";
    error->retryable = retryable;
}

bool has_prefix(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}
}  // namespace

WriterRecoveryInfo inspect_writer_recovery(const std::string& source_gdb_path) {
    WriterRecoveryInfo info;
    info.source_path = source_gdb_path;
    const fs::path source(source_gdb_path);
    const fs::path parent = source.parent_path();
    const std::string base = source.filename().string();
    const std::string working_prefix = base + ".transaction-working-";
    const std::string backup_prefix = base + ".transaction-backup-";

    std::error_code error;
    if (fs::is_directory(parent, error)) {
        for (fs::directory_iterator it(parent, error), end;
             !error && it != end; it.increment(error)) {
            if (!it->is_directory(error)) continue;
            const std::string name = it->path().filename().string();
            if (has_prefix(name, working_prefix))
                info.working_paths.push_back(it->path().string());
            else if (has_prefix(name, backup_prefix))
                info.backup_paths.push_back(it->path().string());
        }
    }

    const bool source_exists = fs::is_directory(source, error);
    if (error || info.working_paths.size() > 1 || info.backup_paths.size() > 1) {
        info.state = WriterRecoveryState::Ambiguous;
    } else if (source_exists && info.working_paths.empty() &&
               info.backup_paths.empty()) {
        info.state = WriterRecoveryState::Clean;
    } else if (!source_exists && info.working_paths.size() == 1 &&
               info.backup_paths.empty()) {
        info.state = WriterRecoveryState::WorkingOnly;
    } else if (!source_exists && info.working_paths.empty() &&
               info.backup_paths.size() == 1) {
        info.state = WriterRecoveryState::BackupOnly;
    } else if (source_exists && info.working_paths.size() == 1 &&
               info.backup_paths.empty()) {
        info.state = WriterRecoveryState::SourceAndWorking;
    } else if (source_exists && info.working_paths.empty() &&
               info.backup_paths.size() == 1) {
        info.state = WriterRecoveryState::SourceAndBackup;
    } else if (source_exists && info.working_paths.size() == 1 &&
               info.backup_paths.size() == 1) {
        info.state = WriterRecoveryState::SourceWorkingAndBackup;
    } else {
        info.state = WriterRecoveryState::Ambiguous;
    }
    return info;
}

bool recover_writer_transaction(const WriterRecoveryInfo& info,
                                WriterRecoveryAction action,
                                WriterError* error) {
    if (error) *error = WriterError{};
    if (info.state == WriterRecoveryState::Ambiguous) {
        set_error(error, WriterErrorCode::ValidationFailed, info.source_path,
                  "recovery state is ambiguous and requires manual inspection");
        return false;
    }

    std::error_code filesystem_error;
    switch (action) {
        case WriterRecoveryAction::DiscardWorking:
            if (info.working_paths.size() != 1 ||
                !fs::is_directory(info.source_path, filesystem_error)) {
                set_error(error, WriterErrorCode::InvalidState, info.source_path,
                          "DiscardWorking requires one working directory and a healthy source");
                return false;
            }
            fs::remove_all(info.working_paths.front(), filesystem_error);
            break;
        case WriterRecoveryAction::RestoreBackupIfSourceMissing:
            if (info.backup_paths.size() != 1 ||
                fs::exists(info.source_path, filesystem_error)) {
                set_error(error, WriterErrorCode::InvalidState, info.source_path,
                          "RestoreBackupIfSourceMissing requires one backup and no source");
                return false;
            }
            fs::rename(info.backup_paths.front(), info.source_path,
                       filesystem_error);
            break;
        case WriterRecoveryAction::RemoveBackupIfSourceHealthy:
            if (info.backup_paths.size() != 1 ||
                !fs::is_directory(info.source_path, filesystem_error)) {
                set_error(error, WriterErrorCode::InvalidState, info.source_path,
                          "RemoveBackupIfSourceHealthy requires one backup and a healthy source");
                return false;
            }
            fs::remove_all(info.backup_paths.front(), filesystem_error);
            break;
    }

    if (filesystem_error) {
        set_error(error, WriterErrorCode::IoFailure, info.source_path,
                  filesystem_error.message(), true);
        return false;
    }
    return true;
}

}  // namespace writer
}  // namespace explorgdb
