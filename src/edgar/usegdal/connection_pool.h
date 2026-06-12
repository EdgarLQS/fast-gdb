// src/connection_pool.h — GdbConnectionPool 连接池（线程安全）

#ifndef GDB_CONNECTION_POOL_H
#define GDB_CONNECTION_POOL_H

#include "connection_info.h"
#include "datasource.h"
#include <memory>
#include <mutex>
#include <vector>

/**
 * GdbConnectionPool — 线程安全连接池，组件库唯一线程安全对象。
 *
 * 核心职责：
 * 1. 管理多个 GdbDatasource 连接的生命周期
 * 2. 通过 acquire()/release() 分配/回收连接
 * 3. 限制最大连接数，防止资源耗尽
 *
 * 线程安全模型：
 * - acquire()/release() 有 mutex 保护，可安全跨线程调用
 * - 返回的 GdbDatasource 不线程安全：同一连接不可跨线程共享
 * - 多线程模式：每个线程 acquire() 获取独立连接，各自使用，release() 归还
 *
 * 池满行为：acquire() 在池满时返回 nullptr（不阻塞、不等待）。
 * 调用方应处理 nullptr 返回值。
 */
class GdbConnectionPool {
public:
    /**
     * 构造连接池。
     *
     * @param info 连接配置（所有连接共享相同配置）
     * @param maxSize 最大连接数。超过此数量时 acquire() 返回 nullptr
     */
    GdbConnectionPool(const GdbConnectionInfo& info, size_t maxSize);

    ~GdbConnectionPool();

    // 不可拷贝、不可移动
    GdbConnectionPool(const GdbConnectionPool&) = delete;
    GdbConnectionPool& operator=(const GdbConnectionPool&) = delete;

    // ========== 获取/归还连接 ==========

    /**
     * 获取一个连接。
     *
     * 实现策略（线程安全，有 mutex 保护）：
     * 1. 优先从空闲池（m_idle）中取出一个已有连接
     * 2. 如果空闲池为空且 m_active < m_maxSize，创建新连接
     *    （open 失败时不计入 m_active）
     * 3. 如果 m_active >= m_maxSize，返回 nullptr（不阻塞）
     *
     * @return 独立 GdbDatasource，调用方负责通过 release() 归还
     */
    std::unique_ptr<GdbDatasource> acquire();

    /**
     * 归还连接。
     *
     * 实现策略（线程安全，有 mutex 保护）：
     * 1. 如果连接仍然打开，放回空闲池（m_idle）供下次 acquire() 复用
     * 2. 如果连接已关闭（如异常导致），仅减少 m_active 计数
     *
     * @param ds acquire() 返回的 GdbDatasource（必须来自本池）
     */
    void release(std::unique_ptr<GdbDatasource> ds);

    // ========== 状态 ==========

    /** 当前活跃连接数（空闲池 + 已分配）。 */
    size_t getActiveCount() const { return m_active; }

    /** 空闲池中可复用的连接数。 */
    size_t getIdleCount() const { return m_idle.size(); }

    /** 最大连接数。 */
    size_t getMaxSize() const { return m_maxSize; }

private:
    /** 连接配置（创建新连接时使用）。 */
    GdbConnectionInfo m_info;

    /** 保护空闲池和活跃计数的互斥锁。 */
    mutable std::mutex m_mutex;

    /** 空闲连接池（可复用的已打开连接）。 */
    std::vector<std::unique_ptr<GdbDatasource>> m_idle;

    /** 最大连接数。 */
    size_t m_maxSize;

    /** 当前活跃连接总数（空闲池 + 已分配）。 */
    size_t m_active = 0;
};

#endif // GDB_CONNECTION_POOL_H
