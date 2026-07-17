#ifndef EXPLORGDB_WRITER_RECOVERY_H
#define EXPLORGDB_WRITER_RECOVERY_H

#include "writer_session.h"

#include <string>
#include <vector>

namespace explorgdb {
namespace writer {

enum class WriterRecoveryState : unsigned char {
    Clean = 0,
    WorkingOnly,
    BackupOnly,
    SourceAndWorking,
    SourceAndBackup,
    SourceWorkingAndBackup,
    Ambiguous,
};

enum class WriterRecoveryAction : unsigned char {
    DiscardWorking = 0,
    RestoreBackupIfSourceMissing,
    RemoveBackupIfSourceHealthy,
};

struct WriterRecoveryInfo {
    WriterRecoveryState state = WriterRecoveryState::Clean;
    std::string source_path;
    std::vector<std::string> working_paths;
    std::vector<std::string> backup_paths;
};

WriterRecoveryInfo inspect_writer_recovery(
    const std::string& source_gdb_path);

bool recover_writer_transaction(const WriterRecoveryInfo& info,
                                WriterRecoveryAction action,
                                WriterError* error = nullptr);

}  // namespace writer
}  // namespace explorgdb

#endif
