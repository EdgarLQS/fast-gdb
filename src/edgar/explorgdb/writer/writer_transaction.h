// src/edgar/explorgdb/writer/writer_transaction.h
// 组合事务 — 在同一私有工作 GDB 上串联 Append/Update/Delete 并一次发布。

#ifndef EXPLORGDB_WRITER_TRANSACTION_H
#define EXPLORGDB_WRITER_TRANSACTION_H

#include "writer_append.h"
#include "writer_delete.h"
#include "writer_update.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace explorgdb {
namespace writer {

/**
 * GDAL 单写者组合事务。
 *
 * open() 创建一份 transaction-owned working GDB。append/update/erase 回调
 * 复用该工作副本，不在每个操作后发布；commit() 只替换真实源一次。
 * abort() 或析构负责清理全部事务私有状态。
 */
class WriterTransaction {
public:
    using AppendEdit = std::function<bool(WriterAppendSession&)>;
    using UpdateEdit = std::function<bool(WriterUpdateSession&)>;
    using DeleteEdit = std::function<bool(WriterDeleteSession&)>;

    WriterTransaction();
    ~WriterTransaction();
    WriterTransaction(WriterTransaction&&) noexcept;
    WriterTransaction& operator=(WriterTransaction&&) noexcept;
    WriterTransaction(const WriterTransaction&) = delete;
    WriterTransaction& operator=(const WriterTransaction&) = delete;

    /** 创建事务工作副本并锁定目标图层的单写者上下文。 */
    bool open(const std::string& source_gdb_path,
              const std::string& layer_name);

    /** 在同一 working GDB 上执行一次追加编辑回调。 */
    bool append(const AppendEdit& edit);

    /** 在同一 working GDB 上执行一次更新编辑回调。 */
    bool update(const UpdateEdit& edit);

    /** 在同一 working GDB 上执行一次删除编辑回调。 */
    bool erase(const DeleteEdit& edit);

    /** 完整验证 working GDB 后一次发布；失败保留可恢复证据。 */
    bool commit();

    /** 放弃全部未发布操作并清理事务私有目录。 */
    bool abort();

    uint64_t operation_count() const noexcept;
    const std::string& working_path() const noexcept;
    bool is_open() const noexcept;
    bool is_committed() const noexcept;
    bool is_aborted() const noexcept;
    const WriterError& error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace writer
} // namespace explorgdb

#endif // EXPLORGDB_WRITER_TRANSACTION_H
