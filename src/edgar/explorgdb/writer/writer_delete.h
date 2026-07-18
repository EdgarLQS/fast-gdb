// src/edgar/explorgdb/writer/writer_delete.h
// 删除编辑会话 — 在完整 staging 副本中按 FID 删除记录并保持其余 FID 稳定。

#ifndef EXPLORGDB_WRITER_DELETE_H
#define EXPLORGDB_WRITER_DELETE_H

#include "writer_session.h"

#include <cstdint>
#include <memory>
#include <string>

namespace explorgdb {
namespace writer {

/**
 * 现有 FileGDB 图层的 GDAL 单次删除会话。
 *
 * 删除只作用于 sibling staging 副本；commit() 前源目录不变。现有 FID 不重排，
 * 删除产生的空洞也不会被本会话复用。
 */
class WriterDeleteSession {
public:
    struct Impl;

    WriterDeleteSession();
    ~WriterDeleteSession();
    WriterDeleteSession(WriterDeleteSession&&) noexcept;
    WriterDeleteSession& operator=(WriterDeleteSession&&) noexcept;
    WriterDeleteSession(const WriterDeleteSession&) = delete;
    WriterDeleteSession& operator=(const WriterDeleteSession&) = delete;

    /** 创建 staging 副本并打开目标图层。 */
    bool open(const std::string& source_gdb_path,
              const std::string& layer_name);

    /** 在 staging 中删除指定稳定 FID；不存在或重复删除返回可诊断错误。 */
    bool delete_feature(int64_t fid);

    /** 验证 staging 后一次替换源目录。 */
    bool commit();

    /** 放弃删除并清理会话拥有的 staging 目录。 */
    bool abort();

    uint64_t original_row_count() const noexcept;
    uint64_t deleted_row_count() const noexcept;
    const std::string& staging_path() const noexcept;
    bool is_open() const noexcept;
    bool is_committed() const noexcept;
    bool is_aborted() const noexcept;
    const WriterError& error() const noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace writer
} // namespace explorgdb

#endif // EXPLORGDB_WRITER_DELETE_H
