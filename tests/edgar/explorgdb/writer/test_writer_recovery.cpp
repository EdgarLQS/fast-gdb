#include "writer_recovery.h"

#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using explorgdb::writer::WriterError;
using explorgdb::writer::WriterErrorCode;
using explorgdb::writer::WriterRecoveryAction;
using explorgdb::writer::WriterRecoveryState;
using explorgdb::writer::inspect_writer_recovery;
using explorgdb::writer::recover_writer_transaction;

namespace {
std::string recovery_source(const std::string& name) {
    return (fs::temp_directory_path() /
            ("fast-gdb-recovery-" + name + ".gdb")).string();
}
void make_dir(const fs::path& path) {
    fs::create_directories(path);
    std::ofstream(path / "marker") << "ok";
}
}  // namespace

TEST(WriterRecoveryTest, DiscardsWorkingOnlyWhenSourceIsHealthy) {
    const std::string source = recovery_source("working");
    fs::remove_all(source);
    make_dir(source);
    const std::string working = source + ".transaction-working-one";
    make_dir(working);
    auto info = inspect_writer_recovery(source);
    ASSERT_EQ(info.state, WriterRecoveryState::SourceAndWorking);
    WriterError error;
    EXPECT_TRUE(recover_writer_transaction(
        info, WriterRecoveryAction::DiscardWorking, &error));
    EXPECT_TRUE(fs::is_directory(source));
    EXPECT_FALSE(fs::exists(working));
    fs::remove_all(source);
}

TEST(WriterRecoveryTest, RestoresUniqueBackupWhenSourceIsMissing) {
    const std::string source = recovery_source("backup");
    fs::remove_all(source);
    const std::string backup = source + ".transaction-backup-one";
    make_dir(backup);
    auto info = inspect_writer_recovery(source);
    ASSERT_EQ(info.state, WriterRecoveryState::BackupOnly);
    WriterError error;
    EXPECT_TRUE(recover_writer_transaction(
        info, WriterRecoveryAction::RestoreBackupIfSourceMissing, &error));
    EXPECT_TRUE(fs::is_directory(source));
    EXPECT_FALSE(fs::exists(backup));
    fs::remove_all(source);
}

TEST(WriterRecoveryTest, RefusesAmbiguousCandidates) {
    const std::string source = recovery_source("ambiguous");
    fs::remove_all(source);
    make_dir(source + ".transaction-backup-one");
    make_dir(source + ".transaction-backup-two");
    auto info = inspect_writer_recovery(source);
    ASSERT_EQ(info.state, WriterRecoveryState::Ambiguous);
    WriterError error;
    EXPECT_FALSE(recover_writer_transaction(
        info, WriterRecoveryAction::RestoreBackupIfSourceMissing, &error));
    EXPECT_EQ(error.code, WriterErrorCode::ValidationFailed);
    fs::remove_all(source + ".transaction-backup-one");
    fs::remove_all(source + ".transaction-backup-two");
}
