// src/reference/usegdal/transaction/batch_writer.h
// GdbBatchWriter — GdbFeature 值对象到单个 OGRLayer 的有界缓冲写入器。

#ifndef GDB_BATCH_WRITER_H
#define GDB_BATCH_WRITER_H

#include "feature.h"

#include <string>
#include <vector>

class GdbDataset;

/**
 * 单图层批量写入辅助类。
 *
 * BatchWriter 只减少调用方逐条组织要素的开销，不提供数据库事务或原子发布：
 * flush() 成功的记录已经进入 OGRLayer；rollback() 只能丢弃尚未 flush 的缓冲。
 * 需要数据库级提交/回滚时，应在外层使用 GdbTransaction。
 *
 * 线程安全：
 * - 实例本身不线程安全，也不持有全局锁；
 * - OGRLayer::CreateFeature 不允许多个线程共享同一 layer；
 * - 并发写入应通过 GdbConnectionPool 为每个线程获取独立 Datasource。
 */
class GdbBatchWriter {
public:
    /**
     * @param dataset 非拥有目标图层视图，生命周期必须超过 BatchWriter。
     * @param batchSize 自动 flush 阈值；达到阈值时 addFeature() 同步写出。
     */
    explicit GdbBatchWriter(GdbDataset& dataset,
                            size_t batchSize = 1000);

    /**
     * 深拷贝要素到缓冲；达到阈值时自动 flush。
     *
     * @return 缓冲或自动 flush 成功时为 true。
     */
    bool addFeature(const GdbFeature& feature);

    /**
     * 将当前缓冲逐条转换为 OGRFeature 并调用 CreateFeature。
     *
     * @return 本次成功写入的要素数量。
     */
    size_t flush();

    /** 刷新剩余缓冲并返回累计写入数。
     * @return 从该 BatchWriter 成功写入的累计要素数；不代表事务提交。
     */
    size_t commit();

    /** 丢弃尚未刷出的缓冲并重置本对象计数。
     * @return 无返回值；不会撤销已写入图层的数据。
     */
    void rollback();

    /** 获取待写入缓冲区大小。
     * @return 尚未 flush 的要素数。
     */
    size_t getBufferSize() const { return m_buffer.size(); }
    /** 获取累计成功写入数。
     * @return 已成功调用 CreateFeature 的要素数。
     */
    size_t getTotalWritten() const { return m_totalWritten; }

private:
    GdbDataset& m_dataset;             // 非拥有目标图层视图
    std::vector<GdbFeature> m_buffer;  // 待写入要素的深拷贝
    size_t m_batchSize;                // 自动 flush 阈值
    bool m_inTransaction = false;      // 预留状态，当前不改变行为
    size_t m_totalWritten = 0;         // 已成功 CreateFeature 的累计数量
    std::string m_lastError;           // 最近一次写入错误文本
};

#endif // GDB_BATCH_WRITER_H
