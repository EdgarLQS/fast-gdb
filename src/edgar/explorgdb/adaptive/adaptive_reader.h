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

// 当 unified runtime 启用时，协调类由 fast_gdb_runtime.dll 导出；
// 静态库模式（FAST_GDB_BUILD_UNIFIED=OFF）则不需要 DLL 修饰。
// 此宏确保两种模式下均正确链接。
#if defined(_WIN32) && defined(FAST_GDB_RUNTIME_BUILD)
#  define EXPLORGDB_ADAPTIVE_API __declspec(dllexport)
#elif defined(_WIN32) && defined(FAST_GDB_RUNTIME_API)
#  define EXPLORGDB_ADAPTIVE_API FAST_GDB_RUNTIME_API
#else
#  define EXPLORGDB_ADAPTIVE_API
#endif

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
    uint64_t pending_events = 0;
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
class EXPLORGDB_ADAPTIVE_API FastReaderLease {
public:
    FastReaderLease() = default;
    FastReaderLease(FastReaderLease&& other) noexcept;
    FastReaderLease& operator=(FastReaderLease&& other) noexcept;
    FastReaderLease(const FastReaderLease&) = delete;
    FastReaderLease& operator=(const FastReaderLease&) = delete;
    ~FastReaderLease();

    /** 判断租约是否仍计入活动 Reader。
     * @return 租约有效时返回 true。
     */
    bool valid() const noexcept { return counted_; }
    /** 获取租约创建时的源代次。
     * @return generation 快照值。
     */
    uint64_t generation() const noexcept { return generation_; }
    /** 判断租约在安全检查点是否已过期。
     * @return 源代次变化或租约失效时返回 true。
     */
    bool expired_at_safe_point() const;
    /** 判断租约期间是否观察到 WriterPending。
     * @return 观察到待写状态时返回 true。
     */
    bool writer_pending_observed() const;
    /** 释放活动 Reader 计数。
     * @return 无返回值；重复调用安全。
     */
    void release();

private:
    FastReaderLease(std::shared_ptr<detail::CoordinatorRegistry> registry,
                    std::string normalized_path,
                    uint64_t generation,
                    uint64_t pending_events);

    // 后端 close 抛异常时无法证明 mmap/句柄已经释放。此时故意把注册表中的
    // reader 计数留在原位并丢弃本地释放能力，使后续 Writer 永久 fail closed。
    // 这是灾难恢复语义，不是正常资源释放路径。
    void abandon_fail_closed() noexcept {
        counted_ = false;
        registry_.reset();
        normalized_path_.clear();
    }

    std::shared_ptr<detail::CoordinatorRegistry> registry_;
    std::string normalized_path_;
    uint64_t generation_ = 0;
    uint64_t pending_events_ = 0;
    bool counted_ = false;

    friend class InProcessGdbCoordinator;
    friend class AdaptiveFeatureCursor;
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
class EXPLORGDB_ADAPTIVE_API ExternalUpdateToken {
public:
    ExternalUpdateToken() = default;
    ExternalUpdateToken(ExternalUpdateToken&& other) noexcept;
    ExternalUpdateToken& operator=(ExternalUpdateToken&& other) noexcept;
    ExternalUpdateToken(const ExternalUpdateToken&) = delete;
    ExternalUpdateToken& operator=(const ExternalUpdateToken&) = delete;
    ~ExternalUpdateToken();

    /** 判断令牌是否仍绑定到有效协调状态。
     * @return 令牌有效时返回 true。
     */
    bool valid() const noexcept;
    /** 判断外部更新是否处于 Pending 阶段。
     * @return 处于 Pending 时返回 true。
     */
    bool pending() const noexcept;
    /** 判断外部更新是否处于 Active 阶段。
     * @return 处于 Active 时返回 true。
     */
    bool active() const noexcept;
    /** 获取协调令牌 ID。
     * @return 可用于恢复关闭报告的令牌 ID。
     */
    uint64_t coordination_id() const noexcept { return token_id_; }

    /** 通知协调器外部 update Dataset 已打开。
     * @return 状态转换结果。
     */
    CoordinationStatus notify_update_opened();
    /** 在真正打开 Dataset 前取消 Pending 更新。
     * @return 状态转换结果。
     */
    CoordinationStatus cancel_before_update();
    /** 通知协调器外部 update Dataset 已关闭。
     * @param close_succeeded 是否已确认 Dataset 成功关闭。
     * @return 状态转换结果。
     */
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

struct EXPLORGDB_ADAPTIVE_API PrepareExternalUpdateResult {
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
class EXPLORGDB_ADAPTIVE_API InProcessGdbCoordinator {
public:
    InProcessGdbCoordinator();

    /** 尝试为本地 fast Reader 获取活动租约。
     * @param gdb_path 待读取的 GDB 路径。
     * @return 成功时返回有效租约；Writer 活动或路径无效时返回无效租约。
     */
    FastReaderLease try_acquire_fast_reader(const std::string& gdb_path) const;

    /** 为外部 GDAL update 准备协调令牌并等待 Reader 排空。
     * @param gdb_path 待更新的 GDB 路径。
     * @param drain_timeout 等待活动 Reader 退出的最长时间。
     * @return 包含状态、令牌、活动 Reader 数和等待耗时的结果。
     */
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
    /** 报告丢失令牌的外部更新已关闭。
     * @param gdb_path 外部更新对应的 GDB 路径。
     * @param coordination_id 打开更新前保存的协调 ID。
     * @param close_succeeded 是否已确认数据集关闭。
     * @return 状态转换结果。
     */
    CoordinationStatus notify_external_update_closed(
        const std::string& gdb_path,
        uint64_t coordination_id,
        bool close_succeeded) const;

    /** 获取指定路径的协调状态快照。
     * @param gdb_path 要检查的 GDB 路径。
     * @return 当前状态、代次和活动 Reader 数。
     */
    CoordinatedSourceState state(const std::string& gdb_path) const;

    /** 规范化 GDB 路径以统一协调键。
     * @param gdb_path 原始路径。
     * @return 规范化后的绝对路径文本。
     */
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

    /** 构造成功的后端读取结果。
     * @param result 已物化的查询结果。
     * @return 标记为成功的结果对象。
     */
    static BackendReadResult success(QueryResult result);
    /** 构造后端打开失败结果。
     * @param error 打开失败信息。
     * @return 标记为 Open 失败的结果对象。
     */
    static BackendReadResult open_failure(std::string error);
    /** 构造后端读取失败结果。
     * @param error 读取失败信息。
     * @return 标记为 Read 失败的结果对象。
     */
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

    /** 读取下一条后端结果。
     * @param feature 接收查询要素的输出对象。
     * @return 成功读取时返回 true，正常结束或失败时返回 false。
     */
    bool next(QueryFeature& feature);
    /** 判断游标是否已经结束。
     * @return 已结束时返回 true。
     */
    bool done() const noexcept { return done_; }
    /** 获取自适应读取状态。
     * @return 当前读取状态。
     */
    AdaptiveReadStatus status() const noexcept { return status_; }
    /** 获取实际使用的后端。
     * @return fast、GDAL 或无后端标识。
     */
    AdaptiveReadBackend backend() const noexcept { return backend_; }
    AdaptiveReadConsistency consistency() const noexcept {
        return consistency_;
    }
    /** 获取游标错误信息。
     * @return 错误文本的只读引用；正常结束时为空。
     */
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

    /** 创建自适应读取会话。
     * @param coordinator 负责路径状态和 Reader 租约的协调器。
     * @param gdb_path GDB 数据源路径。
     * @param fast_executor fast Reader 查询执行器。
     * @param gdal_executor 官方 GDAL 查询执行器。
     * @param fast_cursor_factory 可选 fast 游标工厂。
     * @param gdal_cursor_factory 可选 GDAL 游标工厂。
     */
    AdaptiveReadSession(InProcessGdbCoordinator coordinator,
                        std::string gdb_path,
                        ReadExecutor fast_executor,
                        ReadExecutor gdal_executor,
                        CursorFactory fast_cursor_factory = {},
                        CursorFactory gdal_cursor_factory = {});

    /** 按并发策略路由并执行一次查询。
     * @param request 查询请求。
     * @param policy 源忙时的路由策略。
     * @return 后端、状态、一致性和查询结果。
     */
    AdaptiveReadResult read(
        const QueryRequest& request,
        ConcurrentReadPolicy policy = ConcurrentReadPolicy::SourceBusy) const;

    /** 按并发策略创建自适应流式游标。
     * @param request 查询请求。
     * @param policy 源忙时的路由策略。
     * @return 自适应游标；无法打开时返回失败游标。
     */
    AdaptiveFeatureCursor open_cursor(
        const QueryRequest& request,
        ConcurrentReadPolicy policy = ConcurrentReadPolicy::SourceBusy) const;

private:
    static AdaptiveFeatureCursor failed_cursor(
        AdaptiveReadStatus status,
        AdaptiveReadBackend backend,
        AdaptiveReadConsistency consistency,
        std::string error);

    static AdaptiveFeatureCursor open_fast_cursor_path(
        const CursorFactory& factory,
        const QueryRequest& request,
        FastReaderLease lease);

    static AdaptiveFeatureCursor open_gdal_cursor_path(
        const CursorFactory& factory,
        const QueryRequest& request);

    InProcessGdbCoordinator coordinator_;
    std::string gdb_path_;
    ReadExecutor fast_executor_;
    ReadExecutor gdal_executor_;
    CursorFactory fast_cursor_factory_;
    CursorFactory gdal_cursor_factory_;
};

}  // namespace explorgdb

#endif  // EXPLORGDB_ADAPTIVE_READER_H
