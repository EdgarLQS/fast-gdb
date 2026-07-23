#include <gtest/gtest.h>

#include "adaptive_reader.h"

#include <chrono>
#include <cstdint>
#include <string>

using namespace explorgdb;
using namespace std::chrono_literals;

TEST(AdaptiveReaderRecoveryTest,
     AbandonedActiveTokenRecoversOnlyWithSavedCoordinationId) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "abandoned-active-recovery.gdb";
    uint64_t coordination_id = 0;

    {
        auto prepared = coordinator.prepare_external_update(path, 0ms);
        ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
        coordination_id = prepared.token.coordination_id();
        ASSERT_NE(coordination_id, 0U);
        ASSERT_EQ(prepared.token.notify_update_opened(),
                  CoordinationStatus::Ok);
    }

    const auto abandoned = coordinator.state(path);
    EXPECT_TRUE(abandoned.writer_active);
    EXPECT_FALSE(coordinator.try_acquire_fast_reader(path).valid());

    EXPECT_EQ(coordinator.notify_external_update_closed(
                  path, coordination_id + 1U, true),
              CoordinationStatus::InvalidCoordinationToken);
    EXPECT_TRUE(coordinator.state(path).writer_active);

    EXPECT_EQ(coordinator.notify_external_update_closed(
                  path, coordination_id, true),
              CoordinationStatus::Ok);

    const auto recovered = coordinator.state(path);
    EXPECT_FALSE(recovered.writer_pending);
    EXPECT_FALSE(recovered.writer_active);
    EXPECT_TRUE(recovered.source_verified);
    EXPECT_EQ(recovered.generation, 1U);
    EXPECT_TRUE(coordinator.try_acquire_fast_reader(path).valid());
}

TEST(AdaptiveReaderRecoveryTest,
     ReportedCloseFailureInvalidatesGenerationAndStaysFailClosed) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "failed-close-recovery.gdb";

    auto old_reader = coordinator.try_acquire_fast_reader(path);
    ASSERT_TRUE(old_reader.valid());
    old_reader.release();

    auto prepared = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_opened(),
              CoordinationStatus::Ok);
    EXPECT_EQ(prepared.token.notify_update_closed(false),
              CoordinationStatus::ExternalUpdateNotClosed);

    const auto failed = coordinator.state(path);
    EXPECT_FALSE(failed.writer_active);
    EXPECT_FALSE(failed.source_verified);
    EXPECT_EQ(failed.generation, 1U);
    EXPECT_TRUE(old_reader.expired_at_safe_point());
    EXPECT_FALSE(coordinator.try_acquire_fast_reader(path).valid());

    auto repair = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(repair.status, CoordinationStatus::Ok);
    ASSERT_EQ(repair.token.notify_update_opened(), CoordinationStatus::Ok);
    ASSERT_EQ(repair.token.notify_update_closed(true), CoordinationStatus::Ok);

    const auto repaired = coordinator.state(path);
    EXPECT_TRUE(repaired.source_verified);
    EXPECT_EQ(repaired.generation, 2U);
    EXPECT_TRUE(coordinator.try_acquire_fast_reader(path).valid());
}

TEST(AdaptiveReaderRecoveryTest,
     IndependentCoordinatorObjectsShareTheProcessRegistry) {
    InProcessGdbCoordinator reader_coordinator;
    InProcessGdbCoordinator writer_coordinator;
    const std::string path = "process-wide-registry.gdb";

    auto lease = reader_coordinator.try_acquire_fast_reader(path);
    ASSERT_TRUE(lease.valid());
    EXPECT_EQ(writer_coordinator.state(path).fast_reader_count, 1U);

    auto blocked = writer_coordinator.prepare_external_update(path, 2ms);
    EXPECT_EQ(blocked.status, CoordinationStatus::ReadersActive);
    EXPECT_FALSE(reader_coordinator.state(path).writer_pending);

    lease.release();
    auto prepared = writer_coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    EXPECT_TRUE(reader_coordinator.state(path).writer_pending);
    EXPECT_FALSE(reader_coordinator.try_acquire_fast_reader(path).valid());
    EXPECT_EQ(prepared.token.cancel_before_update(), CoordinationStatus::Ok);
}
