#include "writer_recovery.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"
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
    fs::remove_all(path);
    const fs::path create_path = path.extension() == ".gdb"
        ? path : fs::path(path.string() + ".fixture.gdb");
    fs::remove_all(create_path);
    GDALAllRegister();
    GDALDriver* driver =
        GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    ASSERT_NE(driver, nullptr);
    GDALDataset* dataset = driver ? driver->Create(
        create_path.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr) : nullptr;
    ASSERT_NE(dataset, nullptr);
    if (!dataset) return;
    OGRLayer* layer = dataset->CreateLayer("items", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);
    GDALClose(dataset);
    if (create_path != path) fs::rename(create_path, path);
}
void make_corrupt_dir(const fs::path& path) {
    fs::remove_all(path);
    fs::create_directories(path);
    std::ofstream(path / "marker") << "corrupt";
}
}  // namespace

TEST(WriterRecoveryTest, DiscardsWorkingOnlyWhenSourceIsHealthy) {
    const std::string source = recovery_source("working");
    fs::remove_all(source);
    make_dir(source);
    const std::string working =
        (fs::path(source).parent_path() /
         (fs::path(source).stem().string() + ".transaction-working-one.gdb"))
            .string();
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
    const std::string backup =
        (fs::path(source).parent_path() /
         (fs::path(source).stem().string() + ".transaction-backup-one.gdb"))
            .string();
    fs::remove_all(backup);
    fs::remove_all(source + ".transaction-backup-one");
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

TEST(WriterRecoveryTest, RestoresLegacyBackupNaming) {
    const std::string source = recovery_source("legacy");
    fs::remove_all(source);
    const fs::path backup = source + ".transaction-backup-one";
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

TEST(WriterRecoveryTest, RejectsCorruptSourceBeforeRemovingBackup) {
    const std::string source = recovery_source("corrupt-source");
    fs::remove_all(source);
    const fs::path backup = fs::path(source).parent_path() /
        (fs::path(source).stem().string() + ".transaction-backup-one.gdb");
    make_corrupt_dir(source);
    make_dir(backup);
    auto info = inspect_writer_recovery(source);
    ASSERT_EQ(info.state, WriterRecoveryState::SourceAndBackup);
    WriterError error;
    EXPECT_FALSE(recover_writer_transaction(
        info, WriterRecoveryAction::RemoveBackupIfSourceHealthy, &error));
    EXPECT_TRUE(fs::exists(backup));
    fs::remove_all(source);
    fs::remove_all(backup);
}

TEST(WriterRecoveryTest, RejectsCorruptBackupBeforeRestore) {
    const std::string source = recovery_source("corrupt-backup");
    fs::remove_all(source);
    const fs::path backup = fs::path(source).parent_path() /
        (fs::path(source).stem().string() + ".transaction-backup-one.gdb");
    make_corrupt_dir(backup);
    auto info = inspect_writer_recovery(source);
    ASSERT_EQ(info.state, WriterRecoveryState::BackupOnly);
    WriterError error;
    EXPECT_FALSE(recover_writer_transaction(
        info, WriterRecoveryAction::RestoreBackupIfSourceMissing, &error));
    EXPECT_TRUE(fs::exists(backup));
    fs::remove_all(backup);
}

TEST(WriterRecoveryTest, RejectsForgedCandidateOutsideSourceSiblings) {
    const std::string source = recovery_source("forged");
    const fs::path unrelated = fs::temp_directory_path() /
        "fast-gdb-recovery-unrelated";
    make_dir(source);
    make_dir(unrelated);
    explorgdb::writer::WriterRecoveryInfo forged;
    forged.state = WriterRecoveryState::SourceAndWorking;
    forged.source_path = source;
    forged.working_paths.push_back(unrelated.string());
    WriterError error;
    EXPECT_FALSE(recover_writer_transaction(
        forged, WriterRecoveryAction::DiscardWorking, &error));
    EXPECT_EQ(error.code, WriterErrorCode::ValidationFailed);
    EXPECT_TRUE(fs::exists(unrelated));
    fs::remove_all(source);
    fs::remove_all(unrelated);
}

TEST(WriterRecoveryTest, RejectsActionThatDoesNotMatchState) {
    const std::string source = recovery_source("wrong-action");
    make_dir(source);
    auto info = inspect_writer_recovery(source);
    ASSERT_EQ(info.state, WriterRecoveryState::Clean);
    WriterError error;
    EXPECT_FALSE(recover_writer_transaction(
        info, WriterRecoveryAction::RemoveBackupIfSourceHealthy, &error));
    EXPECT_EQ(error.code, WriterErrorCode::ValidationFailed);
    fs::remove_all(source);
}

TEST(WriterRecoveryTest, RejectsStaleSnapshotAfterCandidateChanges) {
    const std::string source = recovery_source("stale");
    const fs::path working_one = fs::path(source).parent_path() /
        (fs::path(source).stem().string() + ".transaction-working-one.gdb");
    const fs::path working_two = fs::path(source).parent_path() /
        (fs::path(source).stem().string() + ".transaction-working-two.gdb");
    make_dir(source);
    make_dir(working_one);
    fs::remove_all(working_two);
    auto info = inspect_writer_recovery(source);
    ASSERT_EQ(info.state, WriterRecoveryState::SourceAndWorking);
    fs::rename(working_one, working_two);
    WriterError error;
    EXPECT_FALSE(recover_writer_transaction(
        info, WriterRecoveryAction::DiscardWorking, &error));
    EXPECT_EQ(error.code, WriterErrorCode::ValidationFailed);
    EXPECT_TRUE(fs::exists(working_two));
    fs::remove_all(source);
    fs::remove_all(working_two);
}

TEST(WriterRecoveryTest, RejectsFakeTableAndTablxFiles) {
    const std::string source = recovery_source("fake-files");
    const fs::path backup = fs::path(source).parent_path() /
        (fs::path(source).stem().string() + ".transaction-backup-one.gdb");
    fs::remove_all(source);
    fs::remove_all(backup);
    fs::create_directories(backup);
    std::ofstream(backup / "a00000001.gdbtable") << "table";
    std::ofstream(backup / "a00000001.gdbtablx") << "tablx";
    auto info = inspect_writer_recovery(source);
    ASSERT_EQ(info.state, WriterRecoveryState::BackupOnly);
    WriterError error;
    EXPECT_FALSE(recover_writer_transaction(
        info, WriterRecoveryAction::RestoreBackupIfSourceMissing, &error));
    EXPECT_TRUE(fs::exists(backup));
    fs::remove_all(backup);
}

TEST(WriterRecoveryTest, RefusesAmbiguousCandidates) {
    const std::string source = recovery_source("ambiguous");
    fs::remove_all(source);
    const fs::path backup_one = fs::path(source).parent_path() /
        (fs::path(source).stem().string() + ".transaction-backup-one.gdb");
    const fs::path backup_two = fs::path(source).parent_path() /
        (fs::path(source).stem().string() + ".transaction-backup-two.gdb");
    make_dir(backup_one);
    make_dir(backup_two);
    auto info = inspect_writer_recovery(source);
    ASSERT_EQ(info.state, WriterRecoveryState::Ambiguous);
    WriterError error;
    EXPECT_FALSE(recover_writer_transaction(
        info, WriterRecoveryAction::RestoreBackupIfSourceMissing, &error));
    EXPECT_EQ(error.code, WriterErrorCode::ValidationFailed);
    fs::remove_all(backup_one);
    fs::remove_all(backup_two);
}
