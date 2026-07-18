// src/edgar/explorgdb/writer/writer_update.h
// 更新编辑会话 — 在私有 staging 副本中按稳定 FID 修改现有记录。

#ifndef EXPLORGDB_WRITER_UPDATE_H
#define EXPLORGDB_WRITER_UPDATE_H

#include "writer_session.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace explorgdb {
namespace writer {

/**
 * 现有 FileGDB 图层的 GDAL 单次更新会话。
 *
 * begin_update(fid) 选择 staging 副本中的现有记录，setter 只覆盖显式指定的
 * 字段；end_update() 完成当前记录。源目录直到 commit() 成功前保持不变，
 * 更新不改变 FID，也不隐式追加或删除记录。
 */
class WriterUpdateSession {
public:
    struct Impl;

    WriterUpdateSession();
    ~WriterUpdateSession();
    WriterUpdateSession(WriterUpdateSession&&) noexcept;
    WriterUpdateSession& operator=(WriterUpdateSession&&) noexcept;
    WriterUpdateSession(const WriterUpdateSession&) = delete;
    WriterUpdateSession& operator=(const WriterUpdateSession&) = delete;

    /** 创建 staging 副本并打开目标图层。 */
    bool open(const std::string& source_gdb_path,
              const std::string& layer_name);

    /** 选择要更新的稳定 FID；下一次 end_update() 前不能切换记录。 */
    bool begin_update(int64_t fid);
    bool set_null(int field_index);
    bool set_i32(int field_index, int32_t value);
    bool set_i64(int field_index, int64_t value);
    bool set_f64(int field_index, double value);
    bool set_string(int field_index, const std::string& value);
    bool set_binary(int field_index, const std::vector<uint8_t>& value);
    bool set_point(const WriterCoordinate& point,
                   WriterGeometryType type = WriterGeometryType::Point);
    bool set_polyline(
        const std::vector<std::vector<WriterCoordinate>>& parts,
        WriterGeometryType type = WriterGeometryType::Polyline);
    bool set_polygon(
        const std::vector<std::vector<WriterCoordinate>>& rings,
        WriterGeometryType type = WriterGeometryType::Polygon);

    /** 写回当前 staging 记录并增加 updated_row_count。 */
    bool end_update();

    /** 重开验证 staging 后一次替换源目录。 */
    bool commit();

    /** 清理 staging 并终止会话。 */
    bool abort();

    uint64_t original_row_count() const noexcept;
    uint64_t updated_row_count() const noexcept;
    const std::string& staging_path() const noexcept;
    bool is_open() const noexcept;
    bool is_committed() const noexcept;
    bool is_aborted() const noexcept;
    const WriterError& error() const noexcept;

private:
    // 事务组合器通过 unchecked 方法复用同一工作副本和错误传播路径。
    bool end_update_unchecked();
    bool commit_unchecked();
    bool abort_unchecked();
    std::unique_ptr<Impl> impl_;
};

} // namespace writer
} // namespace explorgdb

#endif // EXPLORGDB_WRITER_UPDATE_H
