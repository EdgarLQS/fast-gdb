// src/edgar/explorgdb/writer/writer_recovery.h
// Writer 恢复 — 识别 source/working/backup 组合并执行显式保守恢复动作。

#ifndef EXPLORGDB_WRITER_RECOVERY_H
#define EXPLORGDB_WRITER_RECOVERY_H

#include "writer_session.h"

#include <string>
#include <vector>

namespace explorgdb {
namespace writer {

/** 发布中断后磁盘上可观察到的目录组合。 */
enum class WriterRecoveryState : unsigned char {
    Clean = 0,
    WorkingOnly,
    BackupOnly,
    SourceAndWorking,
    SourceAndBackup,
    SourceWorkingAndBackup,
    Ambiguous,
};

/**
 * 调用方显式选择的保守恢复动作。
 *
 * 恢复器不自动猜测最新数据；Ambiguous 状态必须由上层策略或人工判断。
 */
enum class WriterRecoveryAction : unsigned char {
    DiscardWorking = 0,
    RestoreBackupIfSourceMissing,
    RemoveBackupIfSourceHealthy,
};

/** 恢复扫描结果；路径集合保留全部候选，便于审计和人工处理。 */
struct WriterRecoveryInfo {
    WriterRecoveryState state = WriterRecoveryState::Clean;
    std::string source_path;
    std::vector<std::string> working_paths;
    std::vector<std::string> backup_paths;
};

/**
 * 扫描源目录同级的 Writer working/backup 命名产物并分类状态。
 *
 * 该函数只检查和报告，不修改磁盘。
 */
WriterRecoveryInfo inspect_writer_recovery(
    const std::string& source_gdb_path);

/**
 * 执行一个显式恢复动作。
 *
 * @param info 先前扫描得到的恢复信息。
 * @param action 调用方选择的保守动作。
 * @param error 可选错误输出；成功时保持无错误状态。
 * @return 动作完整完成时为 true。
 */
bool recover_writer_transaction(const WriterRecoveryInfo& info,
                                WriterRecoveryAction action,
                                WriterError* error = nullptr);

} // namespace writer
} // namespace explorgdb

#endif // EXPLORGDB_WRITER_RECOVERY_H
