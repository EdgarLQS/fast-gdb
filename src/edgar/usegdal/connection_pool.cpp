// src/connection_pool.cpp — GdbConnectionPool 连接池实现
//
// 线程安全策略：
// - acquire()/release() 通过 std::mutex 保护共享状态（m_idle, m_active）
// - 返回的 GdbDatasource 不持有锁，调用方独立使用
// - 连接池满时返回 nullptr（不阻塞、不等待、不重试）
//
// 连接生命周期：
// - acquire() 优先复用空闲池中的连接，复用失败才创建新连接
// - release() 将已打开连接放回空闲池，已关闭连接仅减少计数
// - 析构时关闭所有空闲连接

#include "connection_pool.h"
#include <algorithm>

/** 初始化连接池配置。不预先创建连接（懒创建）。 */
GdbConnectionPool::GdbConnectionPool(const GdbConnectionInfo& info, size_t maxSize)
    : m_info(info), m_maxSize(maxSize) {}

/** 析构。关闭所有空闲连接，释放 GDAL 资源。 */
GdbConnectionPool::~GdbConnectionPool() {
    for (auto& ds : m_idle) {
        if (ds) ds->close();
    }
}

/**
 * acquire() — 获取一个连接。
 *
 * 实现策略（线程安全）：
 * 1. 空闲池非空：取出最后一个连接（LIFO 策略，提高缓存局部性）
 * 2. 空闲池空 + 未达上限：创建新 GdbDatasource，open 成功后 m_active++
 *    （open 失败时不增加 m_active，避免无效连接占用名额）
 * 3. 空闲池空 + 已达上限：返回 nullptr（不阻塞）
 */
std::unique_ptr<GdbDatasource> GdbConnectionPool::acquire() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 先从空闲池取（LIFO：取最后一个，提高缓存局部性）
    if (!m_idle.empty()) {
        auto ds = std::move(m_idle.back());
        m_idle.pop_back();
        return ds;
    }

    // 如果未达上限，创建新连接
    if (m_active < m_maxSize) {
        auto ds = std::make_unique<GdbDatasource>();
        if (ds->open(m_info)) {
            m_active++;
            return ds;
        }
        // open 失败不计入 active，避免无效连接占用名额
        return nullptr;
    }

    // 池已满，返回 nullptr（不阻塞、不等待）
    return nullptr;
}

/**
 * release() — 归还连接。
 *
 * 实现策略（线程安全）：
 * 1. 如果连接仍然打开：放回空闲池，供下次 acquire() 复用
 * 2. 如果连接已关闭（如异常/错误导致）：仅减少 m_active 计数
 *    （不再放回空闲池，防止无效连接被复用）
 */
void GdbConnectionPool::release(std::unique_ptr<GdbDatasource> ds) {
    if (!ds) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    if (ds->isOpen()) {
        m_idle.push_back(std::move(ds));
    } else {
        // 连接已关闭，不再复用，仅减少活跃计数
        m_active--;
    }
}
