#include "writer_recovery.h"

#include "gdb_catalog.h"
#include "gdb_table.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace explorgdb {
namespace writer {
namespace fs = std::filesystem;

namespace {
bool source_is_usable(const fs::path& source, std::error_code& error) {
    if (!fs::is_directory(source, error) || error) return false;
    explorgdb::GdbCatalog catalog;
    if (!catalog.scan(source.string()) || !catalog.read_magic() ||
        catalog.magic().version != 5 || catalog.magic().magic != 0xEFBEADDEu) {
        error = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    const auto* table = catalog.find_table(1);
    const auto* tablx = catalog.find_tablx(1);
    if (!table || !tablx || table->file_size == 0 || tablx->file_size == 0) {
        error = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    explorgdb::GdbTableParser parser((source / table->filename).string());
    if (!parser.open() ||
        !parser.load_tablx((source / tablx->filename).string())) {
        error = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    error.clear();
    return true;
}
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
    const fs::path parent = source.has_parent_path()
        ? source.parent_path() : fs::path(".");
    const std::string stem = source.stem().string();
    const std::string ext = source.extension().string();
    const std::string new_working_prefix = stem + ".transaction-working-";
    const std::string new_backup_prefix = stem + ".transaction-backup-";
    const std::string legacy_working_prefix =
        source.filename().string() + ".transaction-working-";
    const std::string legacy_backup_prefix =
        source.filename().string() + ".transaction-backup-";

    std::error_code error;
    bool scan_error = false;
    if (fs::is_directory(parent, error)) {
        for (fs::directory_iterator it(parent, error), end;
             !error && it != end; it.increment(error)) {
            if (!it->is_directory(error)) continue;
            const std::string name = it->path().filename().string();
            const bool new_working = has_prefix(name, new_working_prefix) &&
                !ext.empty() && name.size() > ext.size() &&
                name.compare(name.size() - ext.size(), ext.size(), ext) == 0;
            const bool legacy_working = has_prefix(name, legacy_working_prefix);
            const bool new_backup = has_prefix(name, new_backup_prefix) &&
                !ext.empty() && name.size() > ext.size() &&
                name.compare(name.size() - ext.size(), ext.size(), ext) == 0;
            const bool legacy_backup = has_prefix(name, legacy_backup_prefix);
            if (new_working || legacy_working)
                info.working_paths.push_back(it->path().string());
            else if (new_backup || legacy_backup)
                info.backup_paths.push_back(it->path().string());
        }
        scan_error = static_cast<bool>(error);
    } else if (error) {
        scan_error = true;
    }

    std::sort(info.working_paths.begin(), info.working_paths.end());
    std::sort(info.backup_paths.begin(), info.backup_paths.end());
    error.clear();
    const bool source_exists = fs::is_directory(source, error);
    const bool source_error =
        error && error != std::make_error_code(std::errc::no_such_file_or_directory);
    if (scan_error || source_error || info.working_paths.size() > 1 ||
        info.backup_paths.size() > 1) {
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
    const WriterRecoveryInfo current =
        inspect_writer_recovery(info.source_path);
    if (current.state != info.state ||
        current.working_paths != info.working_paths ||
        current.backup_paths != info.backup_paths) {
        set_error(error, WriterErrorCode::ValidationFailed, info.source_path,
                  "recovery snapshot is stale or contains unowned candidates");
        return false;
    }
    const bool action_allowed =
        (action == WriterRecoveryAction::DiscardWorking &&
         current.state == WriterRecoveryState::SourceAndWorking) ||
        (action == WriterRecoveryAction::RestoreBackupIfSourceMissing &&
         current.state == WriterRecoveryState::BackupOnly) ||
        (action == WriterRecoveryAction::RemoveBackupIfSourceHealthy &&
         current.state == WriterRecoveryState::SourceAndBackup);
    if (!action_allowed) {
        set_error(error, WriterErrorCode::ValidationFailed, info.source_path,
                  "recovery action is not allowed for the current state");
        return false;
    }

    std::error_code filesystem_error;
    switch (action) {
        case WriterRecoveryAction::DiscardWorking:
            if (info.working_paths.size() != 1 ||
                !source_is_usable(info.source_path, filesystem_error)) {
                set_error(error, WriterErrorCode::InvalidState, info.source_path,
                          "DiscardWorking requires one working directory and a healthy source");
                return false;
            }
            fs::remove_all(info.working_paths.front(), filesystem_error);
            break;
        case WriterRecoveryAction::RestoreBackupIfSourceMissing:
            if (info.backup_paths.size() != 1 ||
                fs::exists(info.source_path, filesystem_error) ||
                filesystem_error ||
                !source_is_usable(info.backup_paths.front(), filesystem_error)) {
                set_error(error, WriterErrorCode::InvalidState, info.source_path,
                          "RestoreBackupIfSourceMissing requires one backup and no source");
                return false;
            }
            fs::rename(info.backup_paths.front(), info.source_path,
                       filesystem_error);
            if (!filesystem_error &&
                !source_is_usable(info.source_path, filesystem_error)) {
                std::error_code rollback_error;
                fs::rename(info.source_path, info.backup_paths.front(),
                           rollback_error);
                if (rollback_error) filesystem_error = rollback_error;
                else filesystem_error =
                    std::make_error_code(std::errc::invalid_argument);
            }
            break;
        case WriterRecoveryAction::RemoveBackupIfSourceHealthy:
            if (info.backup_paths.size() != 1 ||
                !source_is_usable(info.source_path, filesystem_error)) {
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
