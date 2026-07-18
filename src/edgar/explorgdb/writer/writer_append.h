// src/edgar/explorgdb/writer/writer_append.h
// 追加编辑会话 — 在完整 sibling staging 副本上向现有图层追加记录。

#ifndef EXPLORGDB_WRITER_APPEND_H
#define EXPLORGDB_WRITER_APPEND_H

#include "writer_session.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace explorgdb {
namespace writer {

/**
 * 现有非空 FileGDB 图层的 GDAL 单次追加会话。
 *
 * open() 复制源 GDB 到同目录私有 staging；所有编辑只作用于副本，commit()
 * 在重开验证后一次替换源目录。该会话不支持 Update/Delete，也不复用 FID 空洞。
 */
class WriterAppendSession {
public:
    // Impl 声明公开仅允许实现文件辅助函数命名该类型，不暴露任何字段。
    struct Impl;

    WriterAppendSession();
    ~WriterAppendSession();
    WriterAppendSession(WriterAppendSession&&) noexcept;
    WriterAppendSession& operator=(WriterAppendSession&&) noexcept;
    WriterAppendSession(const WriterAppendSession&) = delete;
    WriterAppendSession& operator=(const WriterAppendSession&) = delete;

    /** 创建 staging 副本并打开目标图层。 */
    bool open(const std::string& source_gdb_path,
              const std::string& layer_name);

    /** 开始一条追加记录；字段 setter 只修改当前行。 */
    bool begin_row();
    bool set_null(int field_index);
    bool set_i32(int field_index, int32_t value);
    bool set_i64(int field_index, int64_t value);
    bool set_f64(int field_index, double value);
    bool set_string(int field_index, const std::string& value);
    bool set_binary(int field_index, const std::vector<uint8_t>& value);

    /** 设置当前行几何；几何类型必须与目标图层合同一致。 */
    bool set_point(const WriterCoordinate& point,
                   WriterGeometryType type = WriterGeometryType::Point);
    bool set_polyline(
        const std::vector<std::vector<WriterCoordinate>>& parts,
        WriterGeometryType type = WriterGeometryType::Polyline);
    bool set_polygon(
        const std::vector<std::vector<WriterCoordinate>>& rings,
        WriterGeometryType type = WriterGeometryType::Polygon);

    /** 完成当前行并交给 staging 图层；失败时不计入 appended_row_count。 */
    bool end_row();

    /** 验证 staging 后一次发布到原源路径。 */
    bool commit();

    /** 放弃编辑并清理会话拥有的 staging 目录。 */
    bool abort();

    uint64_t original_row_count() const noexcept;
    uint64_t appended_row_count() const noexcept;
    int64_t original_max_fid() const noexcept;
    const std::string& staging_path() const noexcept;
    bool is_open() const noexcept;
    bool is_committed() const noexcept;
    bool is_aborted() const noexcept;
    const WriterError& error() const noexcept;

private:
    // unchecked 入口供事务组合器复用，公开方法负责状态/参数 guard。
    bool open_unchecked(const std::string& source_gdb_path,
                        const std::string& layer_name);
    bool set_null_unchecked(int field_index);
    bool set_i32_unchecked(int field_index, int32_t value);
    bool set_i64_unchecked(int field_index, int64_t value);
    bool set_f64_unchecked(int field_index, double value);
    bool set_string_unchecked(int field_index, const std::string& value);
    bool set_binary_unchecked(int field_index,
                              const std::vector<uint8_t>& value);
    bool set_point_unchecked(const WriterCoordinate& point,
                             WriterGeometryType type);
    bool set_polyline_unchecked(
        const std::vector<std::vector<WriterCoordinate>>& parts,
        WriterGeometryType type);
    bool set_polygon_unchecked(
        const std::vector<std::vector<WriterCoordinate>>& rings,
        WriterGeometryType type);
    bool end_row_unchecked();

    std::unique_ptr<Impl> impl_;
};

} // namespace writer
} // namespace explorgdb

#endif // EXPLORGDB_WRITER_APPEND_H
