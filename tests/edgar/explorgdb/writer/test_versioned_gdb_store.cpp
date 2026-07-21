// tests/edgar/explorgdb/writer/test_versioned_gdb_store.cpp

#include <gtest/gtest.h>

#include "versioned_gdb_store.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>

using namespace explorgdb::writer;
namespace fs = std::filesystem;

namespace {

std::atomic<uint64_t> g_test_sequence{0};

fs::path unique_test_directory() {
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return fs::temp_directory_path() /
           ("fast_gdb_versioned_store_" + std::to_string(ticks) + "_" +
            std::to_string(g_test_sequence.fetch_add(1)));
}

void write_text(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.good());
    output << value;
    output.close();
    ASSERT_TRUE(output.good());
}

std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

GenerationValidator expect_marker(std::string expected) {
    return [expected = std::move(expected)](const fs::path& path) {
        const fs::path marker = path / "records.txt";
        if (!fs::is_regular_file(marker)) {
            return GenerationValidationResult::failure("records.txt is missing");
        }
        if (read_text(marker) != expected) {
            return GenerationValidationResult::failure("record content mismatch");
        }
        if (!fs::is_regular_file(path / "a00000001.gdbtable") ||
            !fs::is_regular_file(path / "a00000001.gdbtablx") ||
            !fs::is_regular_file(path / "a00000001.spx") ||
            !fs::is_regular_file(path / "a00000001.index.atx")) {
            return GenerationValidationResult::failure(
                "FID/geometry/index fixtures are incomplete");
        }
        return GenerationValidationResult::success();
    };
}

class VersionedGdbStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_directory_ = unique_test_directory();
        source_ = test_directory_ / "source.gdb";
        store_root_ = test_directory_ / "store";
        fs::create_directories(source_);
        write_text(source_ / "records.txt", "v1");
        write_text(source_ / "a00000001.gdbtable", "records-v1");
        write_text(source_ / "a00000001.gdbtablx", "fids-v1");
        write_text(source_ / "a00000001.spx", "geometry-index-v1");
        write_text(source_ / "a00000001.index.atx", "attribute-index-v1");
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(test_directory_, ignored);
    }

    void initialize(VersionedGdbStore& store) {
        ASSERT_TRUE(store.open()) << store.last_error();
        ASSERT_TRUE(store.initialize_from(source_, expect_marker("v1")))
            << store.last_error();
    }

    fs::path test_directory_;
    fs::path source_;
    fs::path store_root_;
};

TEST_F(VersionedGdbStoreTest, ReaderSnapshotRemainsOnOldGenerationUntilRefresh) {
    VersionedGdbStore store(store_root_);
    initialize(store);

    auto old_reader = store.acquire_reader();
    ASSERT_TRUE(old_reader.valid()) << store.last_error();
    const std::string old_generation = old_reader.generation();
    const fs::path old_path = old_reader.path();
    EXPECT_EQ(read_text(old_path / "records.txt"), "v1");

    auto writer = store.begin_write();
    ASSERT_TRUE(writer.valid()) << store.last_error();
    write_text(writer.working_path() / "records.txt", "v2");
    write_text(writer.working_path() / "a00000001.gdbtable", "records-v2");
    ASSERT_TRUE(writer.publish(expect_marker("v2"))) << writer.last_error();
    EXPECT_TRUE(writer.published());
    EXPECT_EQ(writer.publish_state(), GdbPublishState::PublishedDurable);

    auto new_reader = store.acquire_reader();
    ASSERT_TRUE(new_reader.valid()) << store.last_error();
    EXPECT_NE(new_reader.generation(), old_generation);
    EXPECT_EQ(read_text(new_reader.path() / "records.txt"), "v2");

    EXPECT_TRUE(fs::is_directory(old_path));
    EXPECT_EQ(read_text(old_reader.path() / "records.txt"), "v1");

    ASSERT_TRUE(old_reader.refresh()) << store.last_error();
    EXPECT_EQ(old_reader.generation(), new_reader.generation());
    EXPECT_EQ(read_text(old_reader.path() / "records.txt"), "v2");
    EXPECT_FALSE(fs::exists(old_path));
}

TEST_F(VersionedGdbStoreTest, OldReaderRemainsContinuouslyVisibleDuringPublish) {
    VersionedGdbStore store(store_root_);
    initialize(store);
    auto old_reader = store.acquire_reader();
    ASSERT_TRUE(old_reader.valid()) << store.last_error();

    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> observed_failure{false};
    const fs::path old_marker = old_reader.path() / "records.txt";
    std::thread reader([&] {
        started.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_acquire)) {
            if (read_text(old_marker) != "v1") {
                observed_failure.store(true, std::memory_order_release);
                break;
            }
        }
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    auto writer = store.begin_write();
    const bool writer_valid = writer.valid();
    if (writer_valid) {
        write_text(writer.working_path() / "records.txt", "v2");
    }
    const bool published =
        writer_valid && writer.publish(expect_marker("v2"));
    auto new_reader = published ? store.acquire_reader() : GdbReaderSnapshot{};

    stop.store(true, std::memory_order_release);
    reader.join();

    ASSERT_TRUE(writer_valid) << store.last_error();
    ASSERT_TRUE(published) << writer.last_error();
    ASSERT_TRUE(new_reader.valid()) << store.last_error();
    EXPECT_EQ(read_text(new_reader.path() / "records.txt"), "v2");
    EXPECT_FALSE(observed_failure.load(std::memory_order_acquire));
    EXPECT_EQ(read_text(old_marker), "v1");
}

TEST_F(VersionedGdbStoreTest, OnlyOneWriterMayOwnRepositoryAcrossStoreInstances) {
    VersionedGdbStore first(store_root_);
    VersionedGdbStore second(store_root_);
    initialize(first);
    ASSERT_TRUE(second.open()) << second.last_error();

    auto writer = first.begin_write();
    ASSERT_TRUE(writer.valid()) << first.last_error();

    auto rejected = second.begin_write();
    EXPECT_FALSE(rejected.valid());
    EXPECT_NE(second.last_error().find("another Writer"), std::string::npos);

    ASSERT_TRUE(writer.abort()) << writer.last_error();
    auto next = second.begin_write();
    EXPECT_TRUE(next.valid()) << second.last_error();
    EXPECT_TRUE(second.last_error().empty());
}

TEST_F(VersionedGdbStoreTest, SymlinkAliasCannotBypassWriterGate) {
    VersionedGdbStore direct(store_root_);
    initialize(direct);

    const fs::path alias_path = test_directory_ / "store-alias";
    std::error_code error;
    fs::create_directory_symlink(store_root_, alias_path, error);
    if (error) {
        GTEST_SKIP() << "directory symlinks unavailable: " << error.message();
    }

    VersionedGdbStore alias(alias_path);
    ASSERT_TRUE(alias.open()) << alias.last_error();

    auto writer = direct.begin_write();
    ASSERT_TRUE(writer.valid()) << direct.last_error();
    auto rejected = alias.begin_write();
    EXPECT_FALSE(rejected.valid());
    EXPECT_NE(alias.last_error().find("another Writer"), std::string::npos);
}

#ifdef _WIN32
TEST_F(VersionedGdbStoreTest, WindowsCaseAliasCannotBypassWriterGate) {
    VersionedGdbStore direct(store_root_);
    initialize(direct);

    const fs::path case_alias = store_root_.parent_path() / "STORE";
    VersionedGdbStore alias(case_alias);
    ASSERT_TRUE(alias.open()) << alias.last_error();

    auto writer = direct.begin_write();
    ASSERT_TRUE(writer.valid()) << direct.last_error();
    auto rejected = alias.begin_write();
    EXPECT_FALSE(rejected.valid());
    EXPECT_NE(alias.last_error().find("another Writer"), std::string::npos);
}
#endif

TEST_F(VersionedGdbStoreTest, ManagedSourceRejectsSymbolicLinks) {
    const fs::path external = test_directory_ / "external.bin";
    write_text(external, "mutable-external-data");
    std::error_code error;
    fs::create_symlink(external, source_ / "external-link", error);
    if (error) {
        GTEST_SKIP() << "file symlinks unavailable: " << error.message();
    }

    VersionedGdbStore store(store_root_);
    ASSERT_TRUE(store.open()) << store.last_error();
    EXPECT_FALSE(store.initialize_from(source_, expect_marker("v1")));
    EXPECT_TRUE(store.current_generation().empty());
    EXPECT_NE(store.last_error().find("symbolic links"), std::string::npos);
}

#ifndef _WIN32
TEST_F(VersionedGdbStoreTest, WorkingCloneAddsOwnerWritePermission) {
    std::error_code error;
    fs::permissions(source_ / "records.txt", fs::perms::owner_read,
                    fs::perm_options::replace, error);
    ASSERT_FALSE(error) << error.message();

    VersionedGdbStore store(store_root_);
    initialize(store);
    auto writer = store.begin_write();
    ASSERT_TRUE(writer.valid()) << store.last_error();

    const fs::perms permissions =
        fs::status(writer.working_path() / "records.txt", error).permissions();
    ASSERT_FALSE(error) << error.message();
    EXPECT_NE(permissions & fs::perms::owner_write, fs::perms::none);
}
#endif

TEST_F(VersionedGdbStoreTest, FailedValidationLeavesCurrentUntouched) {
    VersionedGdbStore store(store_root_);
    initialize(store);
    const std::string before = store.current_generation();

    auto writer = store.begin_write();
    ASSERT_TRUE(writer.valid()) << store.last_error();
    write_text(writer.working_path() / "records.txt", "corrupt");

    EXPECT_FALSE(writer.publish(expect_marker("v2")));
    EXPECT_FALSE(writer.published());
    EXPECT_TRUE(writer.valid());
    EXPECT_EQ(store.current_generation(), before);

    auto reader = store.acquire_reader();
    ASSERT_TRUE(reader.valid()) << store.last_error();
    EXPECT_EQ(reader.generation(), before);
    EXPECT_EQ(read_text(reader.path() / "records.txt"), "v1");
    EXPECT_TRUE(writer.abort()) << writer.last_error();
    EXPECT_TRUE(writer.last_error().empty());
}

TEST_F(VersionedGdbStoreTest, WriterDestructionAbortsWorkingDirectoryAndReleasesGate) {
    VersionedGdbStore store(store_root_);
    initialize(store);

    fs::path abandoned;
    {
        auto writer = store.begin_write();
        ASSERT_TRUE(writer.valid()) << store.last_error();
        abandoned = writer.working_path();
        EXPECT_TRUE(fs::is_directory(abandoned));
    }
    EXPECT_FALSE(fs::exists(abandoned));

    auto next = store.begin_write();
    EXPECT_TRUE(next.valid()) << store.last_error();
}

TEST_F(VersionedGdbStoreTest, RecoveryRemovesWorkAndUnpublishedGenerations) {
    VersionedGdbStore store(store_root_);
    initialize(store);
    const std::string current = store.current_generation();

    const fs::path stale_work = store_root_ / "work" / "work-stale.gdb";
    const fs::path orphan = store_root_ / "generations" / "gen-orphan.gdb";
    write_text(stale_work / "partial", "partial");
    write_text(orphan / "partial", "validated-but-unpublished");
    write_text(store_root_ / "CURRENT.tmp-stale", "gen-orphan.gdb\n");

    ASSERT_TRUE(store.recover()) << store.last_error();
    EXPECT_EQ(store.current_generation(), current);
    EXPECT_FALSE(fs::exists(stale_work));
    EXPECT_FALSE(fs::exists(orphan));
    EXPECT_FALSE(fs::exists(store_root_ / "CURRENT.tmp-stale"));

    auto reader = store.acquire_reader();
    ASSERT_TRUE(reader.valid()) << store.last_error();
    EXPECT_EQ(read_text(reader.path() / "records.txt"), "v1");
}

TEST_F(VersionedGdbStoreTest, CorruptMultiLineCurrentManifestFailsClosed) {
    {
        VersionedGdbStore store(store_root_);
        initialize(store);
    }
    const std::string current = read_text(store_root_ / "CURRENT");
    write_text(store_root_ / "CURRENT", current + "gen-forged.gdb\n");

    VersionedGdbStore reopened(store_root_);
    EXPECT_FALSE(reopened.open());
    EXPECT_NE(reopened.last_error().find("more than one line"),
              std::string::npos);
}

}  // namespace
