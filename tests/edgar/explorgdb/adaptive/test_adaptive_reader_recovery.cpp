// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include "adaptive_reader.h"

#include <chrono>
#include <cstdint>
#include <string>

using namespace explorgdb;
using namespace std::chrono_literals;

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderRecoveryTest,
     ReportedCloseFailureKeepsWriterActiveUntilConfirmedClosed) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "failed-close-recovery.gdb";

    auto old_reader = coordinator.try_acquire_fast_reader(path);
    ASSERT_TRUE(old_reader.valid());
    old_reader.release();

    auto prepared = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    const uint64_t coordination_id = prepared.token.coordination_id();
    ASSERT_NE(coordination_id, 0U);
    ASSERT_EQ(prepared.token.notify_update_opened(),
              CoordinationStatus::Ok);
    EXPECT_EQ(prepared.token.notify_update_closed(false),
              CoordinationStatus::ExternalUpdateNotClosed);
    EXPECT_TRUE(prepared.token.active());

    const auto failed = coordinator.state(path);
    EXPECT_TRUE(failed.writer_active);
    EXPECT_FALSE(failed.source_verified);
    EXPECT_EQ(failed.generation, 1U);
    EXPECT_TRUE(old_reader.expired_at_safe_point());
    EXPECT_FALSE(coordinator.try_acquire_fast_reader(path).valid());

    auto blocked_writer = coordinator.prepare_external_update(path, 0ms);
    EXPECT_EQ(blocked_writer.status,
              CoordinationStatus::WriterAlreadyActive);

    EXPECT_EQ(coordinator.notify_external_update_closed(
                  path, coordination_id, false),
              CoordinationStatus::ExternalUpdateNotClosed);
    EXPECT_EQ(coordinator.state(path).generation, 1U);
    EXPECT_TRUE(coordinator.state(path).writer_active);

    ASSERT_EQ(prepared.token.notify_update_closed(true),
              CoordinationStatus::Ok);
    EXPECT_FALSE(prepared.token.active());

    const auto recovered = coordinator.state(path);
    EXPECT_FALSE(recovered.writer_active);
    EXPECT_TRUE(recovered.source_verified);
    EXPECT_EQ(recovered.generation, 1U);
    EXPECT_TRUE(coordinator.try_acquire_fast_reader(path).valid());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
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
