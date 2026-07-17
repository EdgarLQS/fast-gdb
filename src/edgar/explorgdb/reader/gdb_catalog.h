// src/edgar/explorgdb/gdb_catalog.h
// 目录扫描器 — 枚举 .gdb 目录中的所有文件，读取 magic 和 timestamps
//
// GdbCatalog 是 explorgdb 的入口类。调用 scan(gdb_path) 后：
//   1. 遍历目录，匹配 aXXXXXXXX.<ext> 格式的文件
//   2. 从文件名提取 numeric_id（如 a00000001.gdbtable → id=1）
//   3. 读取 "gdb" 文件的 8 字节头部（version + magic）
//   4. 读取 "timestamps" 文件的 384 字节
//
// 通过 find_table(id) / find_tablx(id) 可按 ID 查找配对文件，
// 再用 GdbTableParser / GdbTablxParser 解析具体内容。

#ifndef EXPLORGDB_GDB_CATALOG_H
#define EXPLORGDB_GDB_CATALOG_H

#include "explorgdb_types.h"
#include <string>
#include <vector>
#include <shared_mutex>

namespace explorgdb {

class GdbCatalog {
public:
    bool scan(const std::string& gdb_path);
    bool read_magic();
    bool read_timestamps();

    const std::string& path() const { return gdb_path_; }
    const std::vector<CatalogEntry>& entries() const { return entries_; }
    const GdbDirectoryHeader& magic() const { return magic_; }
    const GdbTimestamps& timestamps() const { return timestamps_; }

    std::vector<const CatalogEntry*> find_by_extension(const std::string& ext) const;
    const CatalogEntry* find_table(uint32_t id) const;
    const CatalogEntry* find_tablx(uint32_t id) const;
    const CatalogEntry* find_spx(uint32_t id) const;
    const CatalogEntry* find_indexes(uint32_t id) const;
    const CatalogEntry* find_atx(uint32_t id, const std::string& index_name) const;
    std::vector<const CatalogEntry*> find_all_atx(uint32_t id) const;

private:
    std::string gdb_path_;
    std::vector<CatalogEntry> entries_;
    GdbDirectoryHeader magic_;
    GdbTimestamps timestamps_;
    bool has_magic_ = false;
    bool has_timestamps_ = false;

    mutable std::shared_mutex mutex_;
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_CATALOG_H
