// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <gtest/gtest.h>

#include "adaptive_reader.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace explorgdb;
using namespace std::chrono_literals;

namespace {

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = 500ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

QueryResult result_for(const QueryRequest& request,
                       uint32_t generation = 0) {
    QueryResult result;
    const auto kind = static_cast<uint32_t>(request.kind);
    result.matched_fids = {kind, kind + 100U, generation};
    result.execution_path = "fixture-kind-" + std::to_string(kind);
    return result;
}

BackendReadResult successful_result(const QueryRequest& request,
                                    uint32_t generation = 0) {
    return BackendReadResult::success(result_for(request, generation));
}

struct PendingDrainSignals {
    std::mutex mutex;
    std::condition_variable condition;
    bool fast_started = false;
    bool writer_requested = false;
    bool pending_observed = false;
    bool allow_fast_finish = false;
};

AdaptiveReadSession make_pending_drain_session(
    InProcessGdbCoordinator& coordinator,
    const std::string& path,
    PendingDrainSignals& signals) {
    return AdaptiveReadSession(
        coordinator, path,
        [&](const QueryRequest& request) {
            std::unique_lock<std::mutex> lock(signals.mutex);
            signals.fast_started = true;
            signals.condition.notify_all();
            signals.condition.wait(lock, [&] { return signals.writer_requested; });
            while (!coordinator.state(path).writer_pending &&
                   !signals.allow_fast_finish) {
                signals.condition.wait_for(lock, 1ms);
            }
            signals.pending_observed = coordinator.state(path).writer_pending;
            signals.condition.notify_all();
            signals.condition.wait(lock, [&] { return signals.allow_fast_finish; });
            return successful_result(request);
        },
        [](const QueryRequest& request) { return successful_result(request); });
}

std::array<QueryKind, 7> all_query_kinds() {
    return {
        QueryKind::ReadByFid,
        QueryKind::SequentialScan,
        QueryKind::SpatialBbox,
        QueryKind::AttributeDouble,
        QueryKind::AttributeString,
        QueryKind::WhereClause,
        QueryKind::SpatialWhere
    };
}

}  // namespace

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, StableSourceUsesFastVerified) {
    InProcessGdbCoordinator coordinator;
    std::atomic<int> fast_calls{0};
    std::atomic<int> gdal_calls{0};

    AdaptiveReadSession session(
        coordinator, "stable.gdb",
        [&](const QueryRequest& request) {
            ++fast_calls;
            return successful_result(request);
        },
        [&](const QueryRequest& request) {
            ++gdal_calls;
            return successful_result(request);
        });

    QueryRequest request;
    request.kind = QueryKind::SequentialScan;
    const auto result = session.read(request);

    EXPECT_EQ(result.status, AdaptiveReadStatus::Ok);
    EXPECT_EQ(result.backend, AdaptiveReadBackend::FastGdb);
    EXPECT_EQ(result.consistency, AdaptiveReadConsistency::Verified);
    EXPECT_EQ(result.generation_before, 0U);
    EXPECT_EQ(result.generation_after, 0U);
    EXPECT_EQ(fast_calls.load(), 1);
    EXPECT_EQ(gdal_calls.load(), 0);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, WriterPendingStopsNewFastReads) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "pending-blocks.gdb";
    auto existing = coordinator.try_acquire_fast_reader(path);
    ASSERT_TRUE(existing.valid());

    auto writer = std::async(std::launch::async, [&] {
        return coordinator.prepare_external_update(path, 1s);
    });

    ASSERT_TRUE(wait_until([&] {
        return coordinator.state(path).writer_pending;
    }));

    auto blocked = coordinator.try_acquire_fast_reader(path);
    EXPECT_FALSE(blocked.valid());
    EXPECT_EQ(coordinator.state(path).fast_reader_count, 1U);

    existing.release();
    auto prepared = writer.get();
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    EXPECT_EQ(prepared.token.cancel_before_update(), CoordinationStatus::Ok);
    EXPECT_FALSE(coordinator.state(path).writer_pending);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, DeterministicPendingDrainKeepsLeaseUntilMaterialized) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "deterministic-drain.gdb";
    PendingDrainSignals signals;
    auto session = make_pending_drain_session(coordinator, path, signals);

    auto read = std::async(std::launch::async, [&] {
        QueryRequest request;
        request.kind = QueryKind::SequentialScan;
        return session.read(request);
    });
    {
        std::unique_lock<std::mutex> lock(signals.mutex);
        EXPECT_TRUE(signals.condition.wait_for(
            lock, 1s, [&] { return signals.fast_started; }));
    }

    auto writer = std::async(std::launch::async, [&] {
        return coordinator.prepare_external_update(path, 1s);
    });
    {
        std::lock_guard<std::mutex> lock(signals.mutex);
        signals.writer_requested = true;
    }
    signals.condition.notify_all();
    {
        std::unique_lock<std::mutex> lock(signals.mutex);
        EXPECT_TRUE(signals.condition.wait_for(
            lock, 1s, [&] { return signals.pending_observed; }));
        EXPECT_EQ(coordinator.state(path).fast_reader_count, 1U);
        EXPECT_TRUE(coordinator.state(path).writer_pending);
        signals.allow_fast_finish = true;
    }
    signals.condition.notify_all();

    const auto result = read.get();
    auto prepared = writer.get();
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    EXPECT_TRUE(result.writer_pending_seen);
    EXPECT_EQ(result.status, AdaptiveReadStatus::Ok);
    EXPECT_EQ(result.consistency, AdaptiveReadConsistency::Verified);
    EXPECT_EQ(result.generation_before, 0U);
    EXPECT_EQ(result.generation_after, 0U);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_closed(true), CoordinationStatus::Ok);
    EXPECT_EQ(coordinator.state(path).generation, 1U);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, PendingCancellationIsRecordedByFastRead) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "pending-cancel-observed.gdb";
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool allow_finish = false;

    AdaptiveReadSession session(
        coordinator, path,
        [&](const QueryRequest& request) {
            std::unique_lock<std::mutex> lock(mutex);
            started = true;
            condition.notify_all();
            condition.wait(lock, [&] { return allow_finish; });
            return successful_result(request);
        },
        [](const QueryRequest& request) { return successful_result(request); });

    auto read = std::async(std::launch::async, [&] {
        QueryRequest request;
        return session.read(request);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(condition.wait_for(lock, 1s, [&] { return started; }));
    }

    const auto timed_out = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(timed_out.status, CoordinationStatus::ReadersActive);
    ASSERT_FALSE(coordinator.state(path).writer_pending);
    {
        std::lock_guard<std::mutex> lock(mutex);
        allow_finish = true;
    }
    condition.notify_all();

    const auto result = read.get();
    EXPECT_TRUE(result.writer_pending_seen);
    EXPECT_EQ(result.status, AdaptiveReadStatus::Ok);
    EXPECT_EQ(result.consistency, AdaptiveReadConsistency::Verified);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, FastCursorExpiresAtNextSafePoint) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "cursor-safe-point.gdb";

    std::mutex mutex;
    std::condition_variable condition;
    bool next_entered = false;
    bool allow_next_to_finish = false;

    AdaptiveReadSession session(
        coordinator, path,
        [](const QueryRequest& request) { return successful_result(request); },
        [](const QueryRequest& request) { return successful_result(request); },
        [&](const QueryRequest&) {
            auto emitted = std::make_shared<bool>(false);
            BackendCursor cursor;
            cursor.next = [&, emitted](QueryFeature& feature,
                                       std::string&) mutable {
                if (*emitted) return false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    next_entered = true;
                }
                condition.notify_all();
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    condition.wait(lock, [&] { return allow_next_to_finish; });
                }
                feature.fid = 7;
                *emitted = true;
                return true;
            };
            cursor.close = [] {};
            return cursor;
        });

    QueryRequest request;
    auto cursor = session.open_cursor(request);
    ASSERT_EQ(cursor.status(), AdaptiveReadStatus::Ok);

    auto first_next = std::async(std::launch::async, [&] {
        QueryFeature feature;
        return cursor.next(feature) && feature.fid == 7U;
    });

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(condition.wait_for(lock, 500ms,
                                       [&] { return next_entered; }));
    }

    auto writer = std::async(std::launch::async, [&] {
        return coordinator.prepare_external_update(path, 1s);
    });
    ASSERT_TRUE(wait_until([&] {
        return coordinator.state(path).writer_pending;
    }));

    {
        std::lock_guard<std::mutex> lock(mutex);
        allow_next_to_finish = true;
    }
    condition.notify_all();
    EXPECT_TRUE(first_next.get());

    QueryFeature second;
    EXPECT_FALSE(cursor.next(second));
    EXPECT_EQ(cursor.status(), AdaptiveReadStatus::ReaderExpired);

    auto prepared = writer.get();
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    EXPECT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);
    EXPECT_EQ(prepared.token.notify_update_closed(true), CoordinationStatus::Ok);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, PendingTimeoutClearsPendingAndRecovers) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "pending-timeout.gdb";
    auto existing = coordinator.try_acquire_fast_reader(path);
    ASSERT_TRUE(existing.valid());

    auto timed_out = coordinator.prepare_external_update(path, 2ms);
    EXPECT_EQ(timed_out.status, CoordinationStatus::ReadersActive);
    EXPECT_EQ(timed_out.active_readers, 1U);
    EXPECT_FALSE(coordinator.state(path).writer_pending);

    auto recovered_reader = coordinator.try_acquire_fast_reader(path);
    EXPECT_TRUE(recovered_reader.valid());
    recovered_reader.release();
    existing.release();

    auto recovered_writer = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(recovered_writer.status, CoordinationStatus::Ok);
    EXPECT_EQ(recovered_writer.token.cancel_before_update(),
              CoordinationStatus::Ok);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, UpdatePermitRequiresFastReadersDrained) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "drain-required.gdb";
    auto lease = coordinator.try_acquire_fast_reader(path);
    ASSERT_TRUE(lease.valid());

    auto writer = std::async(std::launch::async, [&] {
        return coordinator.prepare_external_update(path, 1s);
    });
    ASSERT_TRUE(wait_until([&] {
        return coordinator.state(path).writer_pending;
    }));
    EXPECT_EQ(writer.wait_for(5ms), std::future_status::timeout);

    lease.release();
    auto prepared = writer.get();
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    EXPECT_EQ(prepared.active_readers, 0U);
    EXPECT_EQ(prepared.token.cancel_before_update(), CoordinationStatus::Ok);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, UpdateOpenFailureCancelsPending) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "open-failure.gdb";

    auto prepared = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    ASSERT_TRUE(coordinator.state(path).writer_pending);

    EXPECT_EQ(prepared.token.cancel_before_update(), CoordinationStatus::Ok);
    const auto state = coordinator.state(path);
    EXPECT_FALSE(state.writer_pending);
    EXPECT_FALSE(state.writer_active);
    EXPECT_EQ(state.generation, 0U);
    EXPECT_TRUE(coordinator.try_acquire_fast_reader(path).valid());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, AbandonedActiveTokenRemainsFailClosed) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "abandoned-active.gdb";

    {
        auto prepared = coordinator.prepare_external_update(path, 0ms);
        ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
        ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);
    }

    const auto state = coordinator.state(path);
    EXPECT_TRUE(state.writer_active);
    EXPECT_FALSE(coordinator.try_acquire_fast_reader(path).valid());

    auto second = coordinator.prepare_external_update(path, 0ms);
    EXPECT_EQ(second.status, CoordinationStatus::WriterAlreadyActive);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, DefaultPolicyReturnsBusyWithoutCallingBackends) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "default-busy.gdb";
    auto prepared = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);

    std::atomic<int> fast_calls{0};
    std::atomic<int> gdal_calls{0};
    AdaptiveReadSession session(
        coordinator, path,
        [&](const QueryRequest& request) {
            ++fast_calls;
            return successful_result(request);
        },
        [&](const QueryRequest& request) {
            ++gdal_calls;
            return successful_result(request);
        });

    QueryRequest request;
    const auto result = session.read(request);
    EXPECT_EQ(result.status, AdaptiveReadStatus::SourceBusy);
    EXPECT_EQ(result.backend, AdaptiveReadBackend::None);
    EXPECT_EQ(result.consistency, AdaptiveReadConsistency::NotApplicable);
    EXPECT_EQ(fast_calls.load(), 0);
    EXPECT_EQ(gdal_calls.load(), 0);

    EXPECT_EQ(prepared.token.notify_update_closed(true), CoordinationStatus::Ok);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, ExplicitPolicyUsesFreshGdalUnverified) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "explicit-gdal.gdb";
    auto prepared = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);

    std::atomic<int> gdal_calls{0};
    AdaptiveReadSession session(
        coordinator, path,
        [](const QueryRequest& request) { return successful_result(request); },
        [&](const QueryRequest& request) {
            ++gdal_calls;
            return successful_result(request);
        });

    QueryRequest request;
    const auto first = session.read(
        request, ConcurrentReadPolicy::GdalUnverified);
    const auto second = session.read(
        request, ConcurrentReadPolicy::GdalUnverified);

    for (const auto* result : {&first, &second}) {
        EXPECT_EQ(result->status, AdaptiveReadStatus::Ok);
        EXPECT_EQ(result->backend, AdaptiveReadBackend::GdalOpenFileGDB);
        EXPECT_EQ(result->consistency,
                  AdaptiveReadConsistency::UnverifiedConcurrentRead);
        EXPECT_TRUE(result->writer_active_seen);
    }
    EXPECT_EQ(gdal_calls.load(), 2);
    EXPECT_EQ(prepared.token.notify_update_closed(true), CoordinationStatus::Ok);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, UnverifiedResultIsNeverReportedVerified) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "never-upgrade.gdb";
    auto old_reader = coordinator.try_acquire_fast_reader(path);
    ASSERT_TRUE(old_reader.valid());

    auto writer = std::async(std::launch::async, [&] {
        auto prepared = coordinator.prepare_external_update(path, 1s);
        if (prepared.status != CoordinationStatus::Ok) {
            return std::make_tuple(prepared.status,
                                   CoordinationStatus::InvalidCoordinationToken,
                                   CoordinationStatus::InvalidCoordinationToken);
        }
        const auto opened = prepared.token.notify_update_opened();
        const auto closed = opened == CoordinationStatus::Ok
            ? prepared.token.notify_update_closed(true)
            : CoordinationStatus::InvalidCoordinationToken;
        return std::make_tuple(prepared.status, opened, closed);
    });

    ASSERT_TRUE(wait_until([&] {
        return coordinator.state(path).writer_pending;
    }));

    AdaptiveReadSession session(
        coordinator, path,
        [](const QueryRequest& request) { return successful_result(request); },
        [&](const QueryRequest& request) {
            old_reader.release();
            const bool writer_finished = wait_until([&] {
                const auto state = coordinator.state(path);
                return !state.writer_pending && !state.writer_active &&
                       state.generation == 1U;
            }, 1s);
            if (!writer_finished) {
                return BackendReadResult::read_failure(
                    "writer did not finish during GDAL read");
            }
            return successful_result(request, 1U);
        });

    QueryRequest request;
    const auto result = session.read(
        request, ConcurrentReadPolicy::GdalUnverified);

    const auto statuses = writer.get();
    EXPECT_EQ(std::get<0>(statuses), CoordinationStatus::Ok);
    EXPECT_EQ(std::get<1>(statuses), CoordinationStatus::Ok);
    EXPECT_EQ(std::get<2>(statuses), CoordinationStatus::Ok);
    EXPECT_EQ(result.status, AdaptiveReadStatus::Ok);
    EXPECT_EQ(result.backend, AdaptiveReadBackend::GdalOpenFileGDB);
    EXPECT_EQ(result.consistency,
              AdaptiveReadConsistency::UnverifiedConcurrentRead);
    EXPECT_EQ(result.generation_before, 0U);
    EXPECT_EQ(result.generation_after, 1U);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, GdalFailureKeepsDiagnosticAndConsistency) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "gdal-failure.gdb";
    auto prepared = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);

    AdaptiveReadSession session(
        coordinator, path,
        [](const QueryRequest& request) { return successful_result(request); },
        [](const QueryRequest&) {
            return BackendReadResult::read_failure("OGRLayer read failed");
        });

    QueryRequest request;
    const auto result = session.read(
        request, ConcurrentReadPolicy::GdalUnverified);
    EXPECT_EQ(result.status, AdaptiveReadStatus::GdalReadFailed);
    EXPECT_EQ(result.backend, AdaptiveReadBackend::GdalOpenFileGDB);
    EXPECT_EQ(result.consistency,
              AdaptiveReadConsistency::UnverifiedConcurrentRead);
    EXPECT_EQ(result.gdal_error, "OGRLayer read failed");
    EXPECT_TRUE(result.writer_active_seen);

    EXPECT_EQ(prepared.token.notify_update_closed(true), CoordinationStatus::Ok);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, ClosedUpdateNotificationIncrementsGeneration) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "generation.gdb";
    EXPECT_EQ(coordinator.state(path).generation, 0U);

    auto prepared = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_closed(true), CoordinationStatus::Ok);

    const auto state = coordinator.state(path);
    EXPECT_FALSE(state.writer_pending);
    EXPECT_FALSE(state.writer_active);
    EXPECT_TRUE(state.source_verified);
    EXPECT_EQ(state.generation, 1U);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, OldReaderExpiresAfterClosedUpdateNotification) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "old-reader.gdb";
    auto old_reader = coordinator.try_acquire_fast_reader(path);
    ASSERT_TRUE(old_reader.valid());
    EXPECT_FALSE(old_reader.expired_at_safe_point());
    old_reader.release();

    auto prepared = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_closed(true), CoordinationStatus::Ok);

    EXPECT_EQ(old_reader.generation(), 0U);
    EXPECT_TRUE(old_reader.expired_at_safe_point());
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, PostWriteRebuildReturnsFastVerified) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "post-write-rebuild.gdb";

    AdaptiveReadSession session(
        coordinator, path,
        [&](const QueryRequest& request) {
            return successful_result(
                request,
                static_cast<uint32_t>(coordinator.state(path).generation));
        },
        [](const QueryRequest& request) { return successful_result(request); });

    QueryRequest request;
    const auto before = session.read(request);
    ASSERT_EQ(before.status, AdaptiveReadStatus::Ok);
    ASSERT_EQ(before.result.matched_fids.back(), 0U);

    auto prepared = coordinator.prepare_external_update(path, 0ms);
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_closed(true), CoordinationStatus::Ok);

    const auto after = session.read(request);
    EXPECT_EQ(after.status, AdaptiveReadStatus::Ok);
    EXPECT_EQ(after.backend, AdaptiveReadBackend::FastGdb);
    EXPECT_EQ(after.consistency, AdaptiveReadConsistency::Verified);
    EXPECT_EQ(after.generation_before, 1U);
    EXPECT_EQ(after.generation_after, 1U);
    ASSERT_FALSE(after.result.matched_fids.empty());
    EXPECT_EQ(after.result.matched_fids.back(), 1U);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, AllQueryKindsMatchOnStableSource) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "stable-parity.gdb";

    const AdaptiveReadSession::ReadExecutor fast_executor =
        [](const QueryRequest& request) { return successful_result(request); };
    const AdaptiveReadSession::ReadExecutor fresh_gdal_executor =
        [](const QueryRequest& request) { return successful_result(request); };

    AdaptiveReadSession session(
        coordinator, path, fast_executor, fresh_gdal_executor);

    for (const QueryKind kind : all_query_kinds()) {
        QueryRequest request;
        request.kind = kind;

        const auto fast = session.read(request);
        const auto gdal = fresh_gdal_executor(request);
        ASSERT_EQ(fast.status, AdaptiveReadStatus::Ok);
        ASSERT_TRUE(gdal.ok);
        EXPECT_EQ(fast.result.matched_fids, gdal.result.matched_fids);
        EXPECT_EQ(fast.result.execution_path, gdal.result.execution_path);
    }
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, RepackNeverOverlapsFastMmap) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "repack.gdb";
    auto first = coordinator.try_acquire_fast_reader(path);
    auto second = coordinator.try_acquire_fast_reader(path);
    ASSERT_TRUE(first.valid());
    ASSERT_TRUE(second.valid());

    auto repack = std::async(std::launch::async, [&] {
        return coordinator.prepare_external_update(path, 1s);
    });
    ASSERT_TRUE(wait_until([&] {
        return coordinator.state(path).writer_pending;
    }));
    EXPECT_FALSE(coordinator.try_acquire_fast_reader(path).valid());

    first.release();
    EXPECT_EQ(repack.wait_for(5ms), std::future_status::timeout);
    second.release();

    auto prepared = repack.get();
    ASSERT_EQ(prepared.status, CoordinationStatus::Ok);
    ASSERT_EQ(prepared.token.notify_update_opened(), CoordinationStatus::Ok);
    const auto active = coordinator.state(path);
    EXPECT_TRUE(active.writer_active);
    EXPECT_EQ(active.fast_reader_count, 0U);
    EXPECT_EQ(prepared.token.notify_update_closed(true), CoordinationStatus::Ok);
}

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(AdaptiveReaderTest, MultipleReadersSingleWriterStress) {
    InProcessGdbCoordinator coordinator;
    const std::string path = "reader-writer-stress.gdb";
    constexpr int kReaderThreads = 8;
    constexpr int kWriterCycles = 40;

    std::atomic<bool> stop{false};
    std::atomic<int> overlap_violations{0};
    std::vector<std::thread> readers;
    readers.reserve(kReaderThreads);

    for (int index = 0; index < kReaderThreads; ++index) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto lease = coordinator.try_acquire_fast_reader(path);
                if (!lease.valid()) {
                    std::this_thread::yield();
                    continue;
                }
                if (coordinator.state(path).writer_active) {
                    ++overlap_violations;
                }
                std::this_thread::sleep_for(50us);
                lease.release();
            }
        });
    }

    bool writer_ok = true;
    for (int cycle = 0; cycle < kWriterCycles; ++cycle) {
        auto prepared = coordinator.prepare_external_update(path, 1s);
        if (prepared.status != CoordinationStatus::Ok) {
            writer_ok = false;
            break;
        }
        if (prepared.token.notify_update_opened() != CoordinationStatus::Ok) {
            writer_ok = false;
            break;
        }
        const auto active = coordinator.state(path);
        if (!active.writer_active || active.fast_reader_count != 0U) {
            ++overlap_violations;
        }
        std::this_thread::sleep_for(25us);
        if (prepared.token.notify_update_closed(true) !=
            CoordinationStatus::Ok) {
            writer_ok = false;
            break;
        }
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& reader : readers) reader.join();

    EXPECT_TRUE(writer_ok);
    EXPECT_EQ(overlap_violations.load(), 0);
    EXPECT_EQ(coordinator.state(path).generation,
              static_cast<uint64_t>(kWriterCycles));
}
