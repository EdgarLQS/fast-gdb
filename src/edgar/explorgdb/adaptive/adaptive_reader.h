// src/edgar/explorgdb/adaptive/adaptive_reader.h
// 同进程 GDAL Writer 协调与 Adaptive Reader 路由公共接口。

#ifndef EXPLORGDB_ADAPTIVE_READER_H
#define EXPLORGDB_ADAPTIVE_READER_H

#include "query_engine.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace explorgdb {

namespace detail {
struct CoordinatorRegistry;
}

enum class ConcurrentReadPolicy {
    SourceBusy,
    GdalUnverified
};

enum class AdaptiveReadBackend {
    None,
    FastGdb,
    GdalOpenFileGDB
};

enum class AdaptiveReadConsistency {
    NotApplicable,
    Verified,
    UnverifiedConcurrentRead
};

enum class AdaptiveReadStatus {
    Ok,
    SourceBusy,
    ReaderExpired,
    FastBackendReadFailed,
    GdalOpenFailed,
    GdalReadFailed
};

enum class CoordinationStatus {
    Ok,
    ReadersActive,
    WriterAlreadyPending,
    WriterAlreadyActive,
    InvalidCoordinationToken,
    ExternalUpdateNotClosed
};

struct CoordinatedSourceState {
    bool writer_pending = false;
    bool writer_active = false;
    bool source_verified = true;
    uint64_t generation = 0;
    size_t fast_reader_count = 0;
};

class InProcessGdbCoordinator;

/**
 * 一个 fast Reader 的进程内租约。
 *
 * 租约持有期间，协调 Writer 可以发布 WriterPending，但不能进入 WriterActive。
 * release() 只释放活动计数；对象仍保留 generation 快照，可用于判断旧 Reader
 * 对象图是否已经过期。
 */
class FastReaderLease {
public:
    FastReaderLease() = default;
    FastReaderLease(FastReaderLease&& other) noexcept;
    FastReaderLease& operator=(FastReaderLease&& other) noexcept;
    FastReaderLease(const FastReaderLease&) = delete;
    FastReaderLease& operator=(const FastReaderLease&) = delete;
    ~FastReaderLease();

    bool valid() const noexcept { return counted_; }
    uint64_t generation() const noexcept { return generation_; }
    bool expired_at_safe_point() const;
    void release();

private:
    FastReaderLease(std::shared_ptr<detail::CoordinatorRegistry> registry,
                    std::string normalized_path,
                    uint64_t generation);

    std::shared_ptr<detail::CoordinatorRegistry> registry_;
    std::string normalized_path_;
    uint64_t generation_ = 0;
    bool counted_ = false;

    friend class InProcessGdbCoordinator;
};

/**
 * 调用方外部 GDAL update 生命周期的无 GDAL 类型令牌。
 *
 * Pending 状态析构会撤销 Pending。Active 状态析构不会猜测外部 Dataset 已关闭，
 * 协调器保持 fail-closed。调用方应在打开 update Dataset 前保存 coordination_id；
 * 若 Active 令牌丢失，可在确认 Dataset 已关闭后通过协调器显式恢复。
 *
 * notify_update_closed(false) 表示 Dataset 尚未确认关闭：WriterActive、token id 和
 * fail-closed 状态全部保留。调用方之后必须使用同一令牌或保存的 coordination_id
 * 再次报告 true；每个 Writer 生命周期最多递增一次 generation。
 */
class ExternalUpdateToken {
public:
    ExternalUpdateToken() = default;
    ExternalUpdateToken(ExternalUpdateToken&& other) noexcept;
    ExternalUpdateToken& operator=(ExternalUpdateToken&& other) noexcept;
    ExternalUpdateToken(const ExternalUpdateToken&) = delete;
    ExternalUpdateToken& operator=(const ExternalUpdateToken&) = delete;
    ~ExternalUpdateToken();

    bool valid() const noexcept;
    bool pending() const noexcept;
    bool active() const noexcept;
    uint64_t coordination_id() const noexcept { return token_id_; }

    CoordinationStatus notify_update_opened();
    CoordinationStatus cancel_before_update();
    CoordinationStatus notify_update_closed(bool close_succeeded);

private:
    enum class Phase {
        Invalid,
        Pending,
        Active,
        Closed
    };

    ExternalUpdateToken(
        std::shared_ptr<detail::CoordinatorRegistry> registry,
        std::string normalized_path,
        uint64_t token_id);

    void abandon_current_state() noexcept;

    std::shared_ptr<detail::CoordinatorRegistry> registry_;
    std::string normalized_path_;
    uint64_t token_id_ = 0;
    Phase phase_ = Phase::Invalid;

    friend class InProcessGdbCoordinator;
};

struct PrepareExternalUpdateResult {
    CoordinationStatus status = CoordinationStatus::InvalidCoordinationToken;
    ExternalUpdateToken token;
    size_t active_readers = 0;
    std::chrono::milliseconds waited{0};

    explicit operator bool() const noexcept {
        return status == CoordinationStatus::Ok && token.valid();
    }
};

/**
 * 按规范化 GDB 路径维护 WriterPending、WriterActive、generation 和 fast 租约。
 * 所有默认构造对象共享同一进程级注册表；复制对象也共享该注册表，因此同一进程中
 * 不同组件无法通过创建新的 coordinator 绕过同一路径的互斥状态。
 */
class InProcessGdbCoordinator {
public:
    InProcessGdbCoordinator();

    FastReaderLease try_acquire_fast_reader(const std::string& gdb_path) const;

    PrepareExternalUpdateResult prepare_external_update(
        const std::string& gdb_path,
        std::chrono::milliseconds drain_timeout) const;

    /**
     * Active 令牌丢失后的显式关闭报告入口。
     *
     * 传入打开 update Dataset 前保存的 coordination_id。close_succeeded=false 会使
     * generation 失效一次，但保持 WriterActive 并阻断 fast Reader 与新 Writer；
     * 只有同一 id 后续报告 true 才会清除 Active 并恢复 Verified fast 读取。
     */
    CoordinationStatus notify_external_update_closed(
        const std::string& gdb_path,
        uint64_t coordination_id,
        bool close_succeeded) const;

    CoordinatedSourceState state(const std::string& gdb_path) const;

    static std::string normalize_path(const std::string& gdb_path);

private:
    std::shared_ptr<detail::CoordinatorRegistry> registry_;
};

enum class BackendFailureKind {
    None,
    Open,
    Read
};

/** 单次后端读取的完整物化结果。 */
struct BackendReadResult {
    bool ok = false;
    QueryResult result;
    BackendFailureKind failure = BackendFailureKind::Read;
    std::string error;

    static BackendReadResult success(QueryResult result);
    static BackendReadResult open_failure(std::string error);
    static BackendReadResult read_failure(std::string error);
};

struct AdaptiveReadResult {
    AdaptiveReadStatus status = AdaptiveReadStatus::SourceBusy;
    AdaptiveReadBackend backend = AdaptiveReadBackend::None;
    AdaptiveReadConsistency consistency =
        AdaptiveReadConsistency::NotApplicable;
    QueryResult result;
    uint64_t generation_before = 0;
    uint64_t generation_after = 0;
    bool writer_pending_seen = false;
    bool writer_active_seen = false;
    std::string fast_error;
    std::string gdal_error;
};

/**
 * 后端 cursor 的拥有型适配器。
 * next 返回 false 且 error 为空表示正常结束；error 非空表示后端失败。
 */
struct BackendCursor {
    std::function<bool(QueryFeature& feature, std::string& error)> next;
    std::function<void()> close;
};

class AdaptiveFeatureCursor {
public:
    AdaptiveFeatureCursor() = default;
    AdaptiveFeatureCursor(AdaptiveFeatureCursor&& other) noexcept;
    AdaptiveFeatureCursor& operator=(AdaptiveFeatureCursor&& other) noexcept;
    AdaptiveFeatureCursor(const AdaptiveFeatureCursor&) = delete;
    AdaptiveFeatureCursor& operator=(const AdaptiveFeatureCursor&) = delete;
    ~AdaptiveFeatureCursor();

    bool next(QueryFeature& feature);
    bool done() const noexcept { return done_; }
    AdaptiveReadStatus status() const noexcept { return status_; }
    AdaptiveReadBackend backend() const noexcept { return backend_; }
    AdaptiveReadConsistency consistency() const noexcept {
        return consistency_;
    }
    const std::string& error() const noexcept { return error_; }

private:
    AdaptiveFeatureCursor(AdaptiveReadStatus status,
                          AdaptiveReadBackend backend,
                          AdaptiveReadConsistency consistency,
                          FastReaderLease fast_lease,
                          BackendCursor backend_cursor);

    void close();

    AdaptiveReadStatus status_ = AdaptiveReadStatus::SourceBusy;
    AdaptiveReadBackend backend_ = AdaptiveReadBackend::None;
    AdaptiveReadConsistency consistency_ =
        AdaptiveReadConsistency::NotApplicable;
    FastReaderLease fast_lease_;
    BackendCursor backend_cursor_;
    bool done_ = true;
    bool backend_closed_ = false;
    std::string error_;

    friend class AdaptiveReadSession;
};

/**
 * Reader-only 编排层。
 *
 * fast_executor 必须调用 fast-gdb Reader；gdal_executor 必须为每次调用创建、物化并
 * 关闭一个 fresh 官方 GDAL 只读 Dataset。协调器只负责路由和一致性标记。
 */
class AdaptiveReadSession {
public:
    using ReadExecutor =
        std::function<BackendReadResult(const QueryRequest& request)>;
    using CursorFactory =
        std::function<BackendCursor(const QueryRequest& request)>;

    AdaptiveReadSession(InProcessGdbCoordinator coordinator,
                        std::string gdb_path,
                        ReadExecutor fast_executor,
                        ReadExecutor gdal_executor,
                        CursorFactory fast_cursor_factory = {},
                        CursorFactory gdal_cursor_factory = {});

    AdaptiveReadResult read(
        const QueryRequest& request,
        ConcurrentReadPolicy policy = ConcurrentReadPolicy::SourceBusy) const;

    AdaptiveFeatureCursor open_cursor(
        const QueryRequest& request,
        ConcurrentReadPolicy policy = ConcurrentReadPolicy::SourceBusy) const;

private:
    InProcessGdbCoordinator coordinator_;
    std::string gdb_path_;
    ReadExecutor fast_executor_;
    ReadExecutor gdal_executor_;
    CursorFactory fast_cursor_factory_;
    CursorFactory gdal_cursor_factory_;
};

}  // namespace explorgdb

#endif  // EXPLORGDB_ADAPTIVE_READER_H
