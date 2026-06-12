// src/batch_writer.h — GdbBatchWriter 批量写入器

#ifndef GDB_BATCH_WRITER_H
#define GDB_BATCH_WRITER_H

#include "feature.h"
#include <string>
#include <vector>

class GdbDataset;

/**
 * 线程安全模型：
 * - GdbBatchWriter 本身不线程安全，不持有全局锁。
 * - 多线程并发写入同一图层时，应为每个线程创建独立的
 *   GdbDatasource（通过 GdbConnectionPool acquire），
 *   各自持有独立的 GdbBatchWriter 实例。
 * - OGRLayer::CreateFeature 不线程安全，多线程不可共享
 *   同一个 OGRLayer 指针。
 *
 * 正确用法：
 *   GdbConnectionPool pool(info, N);
 *   // 线程 A:
 *   auto connA = pool.acquire();
 *   GdbBatchWriter writerA(connA->getDatasets().get("layer"));
 *   // 线程 B:
 *   auto connB = pool.acquire();
 *   GdbBatchWriter writerB(connB->getDatasets().get("layer"));
 */
class GdbBatchWriter {
public:
    /**
     * 构造批量写入器。
     *
     * @param dataset 目标图层视图（非拥有，生命周期须超过 BatchWriter）
     * @param batchSize 自动 flush 阈值。默认 1000，即缓冲区达到 1000 条时自动写入
     */
    explicit GdbBatchWriter(GdbDataset& dataset, size_t batchSize = 1000);

    /**
     * 添加要素到缓冲区。
     *
     * 实现策略：
     * 1. 将要素追加到 m_buffer
     * 2. 如果缓冲区大小 >= batchSize，自动调用 flush() 写入
     *
     * @param feature 要写入的要素（深拷贝到缓冲区）
     * @return true 成功（或 auto-flush 成功），false flush 失败
     */
    bool addFeature(const GdbFeature& feature);

    /**
     * 将缓冲区中所有要素写入图层。
     *
     * 实现策略：
     * 1. 遍历 m_buffer，每个 GdbFeature 调用 toNative() 转为 OGRFeature
     * 2. 设置几何对象
     * 3. CreateFeature 写入，DestroyFeature 释放原生要素
     * 4. 清空缓冲区，累加 m_totalWritten
     *
     * @return 本次成功写入的要素数量
     */
    size_t flush();

    /**
     * 提交写入。
     *
     * 等价于 flush() + 返回总写入数。
     * 不调用事务提交，如需在事务中批量写入，调用方应手动管理事务。
     *
     * @return 累计成功写入的要素总数
     */
    size_t commit();

    /**
     * 回滚写入。
     *
     * 丢弃缓冲区中全部未写入要素，重置总写入计数为 0。
     * 已写入图层的要素不受影响（不撤销）。
     */
    void rollback();

    /** 当前缓冲区中的要素数量。 */
    size_t getBufferSize() const { return m_buffer.size(); }

    /** 累计成功写入的要素总数（包含已 flush 的）。 */
    size_t getTotalWritten() const { return m_totalWritten; }

private:
    /** 目标图层视图（非拥有）。 */
    GdbDataset& m_dataset;

    /** 缓冲区，存储待写入的 GdbFeature 深拷贝。 */
    std::vector<GdbFeature> m_buffer;

    /** 自动 flush 阈值。 */
    size_t m_batchSize;

    /** 是否处于事务中（预留，当前未使用）。 */
    bool m_inTransaction = false;

    /** 累计成功写入的要素总数。 */
    size_t m_totalWritten = 0;

    /** 最新错误信息。 */
    std::string m_lastError;
};

#endif // GDB_BATCH_WRITER_H
