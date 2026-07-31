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
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace explorgdb {

class GdbCatalog {
public:
    /** 扫描 .gdb 目录并填充文件条目。
     * @param gdb_path FileGDB 目录路径。
     * @return 目录存在且至少找到一个匹配文件时返回 true。
     */
    bool scan(const std::string& gdb_path);

    /** 读取 gdb 文件的 8 字节目录头并校验 magic。
     * @return 读取并校验成功时返回 true。
     */
    bool read_magic();

    /** 读取 timestamps 文件的原始时间戳数据。
     * @return 文件读取成功时返回 true。
     */
    bool read_timestamps();

    /** 获取当前扫描的 FileGDB 目录路径。
     * @return 目录路径的只读引用。
     */
    const std::string& path() const { return gdb_path_; }
    /** 获取当前扫描得到的全部目录条目。
     * @return 目录条目数组的只读引用。
     */
    const std::vector<CatalogEntry>& entries() const { return entries_; }
    /** 获取 gdb 文件目录头。
     * @return 目录头的只读引用。
     */
    const GdbDirectoryHeader& magic() const { return magic_; }
    /** 获取 timestamps 文件解析结果。
     * @return 时间戳数据的只读引用。
     */
    const GdbTimestamps& timestamps() const { return timestamps_; }

    /** 按扩展名筛选目录条目。
     * @param ext 目标扩展名，例如 ".gdbtable"。
     * @return 指向内部条目的指针列表；返回值不拥有条目生命周期。
     */
    std::vector<const CatalogEntry*> find_by_extension(const std::string& ext) const;

    /** 按数字 ID 查找 gdbtable 文件。
     * @param id 表的数字 ID。
     * @return 找到时返回内部条目指针，否则返回 nullptr。
     */
    const CatalogEntry* find_table(uint32_t id) const;

    /** 按数字 ID 查找 gdbtablx 文件。
     * @param id 表的数字 ID。
     * @return 找到时返回内部条目指针，否则返回 nullptr。
     */
    const CatalogEntry* find_tablx(uint32_t id) const;

    /** 按数字 ID 查找空间索引文件。
     * @param id 表的数字 ID。
     * @return 找到时返回内部条目指针，否则返回 nullptr。
     */
    const CatalogEntry* find_spx(uint32_t id) const;

    /** 按数字 ID 查找属性索引元数据文件。
     * @param id 表的数字 ID。
     * @return 找到时返回内部条目指针，否则返回 nullptr。
     */
    const CatalogEntry* find_indexes(uint32_t id) const;

    // 解析并缓存某张表的 .gdbindexes 条目。缓存绑定到本次 scan()
    // 快照；再次 scan() 会为当前对象创建新的 cache state。复制 catalog
    // 会共享不可变快照的缓存，但任一副本重新 scan() 不影响其他副本。
    bool read_index_metadata(uint32_t id,
                             std::vector<IndexEntry>& entries) const;

    /** 按表 ID 和索引名称查找属性索引文件。
     * @param id 表的数字 ID。
     * @param index_name gdbindexes 中记录的索引名称。
     * @return 找到时返回内部条目指针，否则返回 nullptr。
     */
    const CatalogEntry* find_atx(uint32_t id, const std::string& index_name) const;

    /** 查找某张表对应的全部属性索引文件。
     * @param id 表的数字 ID。
     * @return 指向内部条目的指针列表；返回值不拥有条目生命周期。
     */
    std::vector<const CatalogEntry*> find_all_atx(uint32_t id) const;

private:
    struct IndexMetadataCacheEntry {
        bool parsed = false;
        std::vector<IndexEntry> entries;
    };

    struct IndexMetadataCacheState {
        mutable std::shared_mutex mutex;
        std::unordered_map<uint32_t, IndexMetadataCacheEntry> entries;
    };

    std::string gdb_path_;
    std::vector<CatalogEntry> entries_;      // 所有 aXXXXXXXX.* 文件
    GdbDirectoryHeader magic_;               // gdb 文件头部
    GdbTimestamps timestamps_;               // timestamps 文件原始数据
    bool has_magic_ = false;
    bool has_timestamps_ = false;

    mutable std::shared_ptr<IndexMetadataCacheState> index_metadata_cache_ =
        std::make_shared<IndexMetadataCacheState>();
};

} // namespace explorgdb

#endif // EXPLORGDB_GDB_CATALOG_H
