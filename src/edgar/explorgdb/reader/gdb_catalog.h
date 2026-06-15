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
    // 扫描 .gdb 目录，填充所有文件条目
    // 返回 true 表示目录存在且至少包含一个匹配文件
    bool scan(const std::string& gdb_path);

    // 读取 "gdb" 文件中的 8 字节头部
    // 预期: version=5, magic=0xDEADBEEF（小端读取为 0xEFBEADDE）
    bool read_magic();

    // 读取 "timestamps" 文件的 384 字节原始数据
    // 具体结构尚未完全解析
    bool read_timestamps();

    // ── 访问器 ──

    const std::string& path() const { return gdb_path_; }
    const std::vector<CatalogEntry>& entries() const { return entries_; }
    const GdbDirectoryHeader& magic() const { return magic_; }
    const GdbTimestamps& timestamps() const { return timestamps_; }

    // 按扩展名筛选文件条目（返回指针列表，不拥有数据）
    std::vector<const CatalogEntry*> find_by_extension(const std::string& ext) const;

    // 按 ID 查找 .gdbtable 文件（如 find_table(1) → a00000001.gdbtable）
    const CatalogEntry* find_table(uint32_t id) const;

    // 按 ID 查找 .gdbtablx 文件（如 find_tablx(1) → a00000001.gdbtablx）
    const CatalogEntry* find_tablx(uint32_t id) const;

    // 按 ID 查找 .spx 空间索引文件
    const CatalogEntry* find_spx(uint32_t id) const;

    // 按 ID + 索引名查找 .atx 属性索引文件
    // index_name 来自 .gdbindexes 中的名称（如 "MyIndex"）
    const CatalogEntry* find_atx(uint32_t id, const std::string& index_name) const;

    // 查找某个表的所有 .atx 文件（按 ID 匹配前缀）
    std::vector<const CatalogEntry*> find_all_atx(uint32_t id) const;

private:
    std::string gdb_path_;
    std::vector<CatalogEntry> entries_;      // 所有 aXXXXXXXX.* 文件
    GdbDirectoryHeader magic_;               // gdb 文件头部
    GdbTimestamps timestamps_;               // timestamps 文件原始数据
    bool has_magic_ = false;
    bool has_timestamps_ = false;

    mutable std::shared_mutex mutex_;  // 保护 scan 操作的线程安全
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_CATALOG_H
