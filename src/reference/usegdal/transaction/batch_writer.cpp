// src/batch_writer.cpp — GdbBatchWriter 批量写入器实现
//
// 核心策略：
// 1. 缓冲 + 阈值自动 flush：减少逐条 CreateFeature 的开销
// 2. GdbFeature → OGRFeature 转换：toNative() 创建临时 OGRFeature，
//    写入后立即 DestroyFeature 释放
// 3. 不自动管理事务：OpenFileGDB 不原生支持事务，
//    如需事务保护，调用方应在事务中创建 BatchWriter
//
// 线程安全：不线程安全。每个线程应有独立实例和独立 OGRLayer。

#include "batch_writer.h"
#include "datasets.h"
#include "datasource.h"
#include "feature.h"

/**
 * 构造批量写入器。
 *
 * 不尝试开启事务（OpenFileGDB 不原生支持）。
 * 如需事务保护，支持事务的驱动应由调用方手动开启。
 */
GdbBatchWriter::GdbBatchWriter(GdbDataset& dataset, size_t batchSize)
    : m_dataset(dataset), m_batchSize(batchSize) {
    // OpenFileGDB 不原生支持事务，这里不尝试开启
    // 如需事务保护，支持事务的驱动应由调用方手动开启
}

/**
 * addFeature() — 添加要素到缓冲区。
 *
 * 实现策略：
 * 1. 深拷贝要素到 m_buffer（GdbFeature 是值类型，自动深拷贝几何）
 * 2. 检查是否达到 batchSize 阈值
 * 3. 达到阈值时自动 flush()，将缓冲区全部写入
 *
 * 注意：auto-flush 只写入当前缓冲区，新要素不受影响。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbBatchWriter::addFeature(const GdbFeature& feature) {
    m_buffer.push_back(feature);

    if (m_buffer.size() >= m_batchSize) {
        return flush();
    }
    return true;
}

/**
 * flush() — 将缓冲区中所有要素写入图层。
 *
 * 实现策略：
 * 1. 遍历 m_buffer 中的每个 GdbFeature
 * 2. feat.toNative() 创建临时 OGRFeature（基于 LayerDefn）
 * 3. 如果要素有几何，SetGeometry 设置
 * 4. layer->CreateFeature() 写入图层
 * 5. OGRFeature::DestroyFeature() 释放临时 OGRFeature
 * 6. 累加成功计数，清空缓冲区
 *
 * 内存管理：toNative() 创建的 OGRFeature 必须手动 DestroyFeature。
 * 几何通过 GetGeometry() 获取（const 指针），SetGeometry 不拷贝。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
size_t GdbBatchWriter::flush() {
    size_t written = 0;
    for (const auto& feat : m_buffer) {
        OGRLayer* layer = m_dataset.getNative();
        if (!layer) continue;

        // 将 GdbFeature 转为 OGRFeature 后写入
        OGRFeature* native = feat.toNative(layer->GetLayerDefn());
        if (!native) continue;

        if (feat.getGeometry()) {
            native->SetGeometry(const_cast<OGRGeometry*>(feat.getGeometry()));
        }

        OGRErr err = layer->CreateFeature(native);
        OGRFeature::DestroyFeature(native);

        if (err == OGRERR_NONE) {
            written++;
        }
    }

    m_totalWritten += written;
    m_buffer.clear();
    return written;
}

/**
 * commit() — 提交写入。
 *
 * 等价于 flush() + 返回累计写入总数。
 * 不调用事务提交（OpenFileGDB 不支持，其他驱动由调用方管理）。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
size_t GdbBatchWriter::commit() {
    flush();
    return m_totalWritten;
}

/**
 * rollback() — 回滚写入。
 *
 * 丢弃缓冲区中全部要素，重置总写入计数为 0。
 * 已写入图层的要素不受影响（不撤销已持久化的数据）。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
void GdbBatchWriter::rollback() {
    m_buffer.clear();
    m_totalWritten = 0;
}
