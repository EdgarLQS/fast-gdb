// src/edgar/explorgdb/writer/gdb_index_creator.h
// GDAL OpenFileGDB 索引创建器 — 封装 GDAL C API 创建空间索引和属性索引
//
// 功能：
//   - 创建空间索引（.spx）
//   - 创建单字段属性索引（.atx）
//   - 创建联合索引（多字段复合）
//   - 批量创建索引
//   - 删除索引
//   - 检测空间索引
//
// 依赖：GDAL 3.9.3+（OpenFileGDB 驱动）
//
// 使用示例：
//   #include "explorgdb/writer/gdb_index_creator.h"
//   using namespace explorgdb::writer;
//
//   // 创建空间索引
//   CreateSpatialIndex("/path/to/data.gdb", "cities");
//
//   // 创建属性索引
//   CreateAttributeIndex("/path/to/data.gdb", "cities", "name");
//
//   // 创建联合索引
//   CreateCompositeIndex("/path/to/data.gdb", "cities", {"province", "city"});
//
//   // 批量创建
//   std::vector<IndexDefinition> indexes = {
//       IndexDefinition::Spatial(),
//       IndexDefinition("name_idx", "name")
//   };
//   CreateIndexes("/path/to/data.gdb", "cities", indexes);

#ifndef EXPLORGDB_GDB_INDEX_CREATOR_H
#define EXPLORGDB_GDB_INDEX_CREATOR_H

#include <string>
#include <vector>

namespace explorgdb {
namespace writer {

/**
 * 索引定义
 */
struct IndexDefinition {
    std::string index_name;              // 索引名称（可选，为空则自动生成）
    std::vector<std::string> fields;     // 字段列表（单字段或多字段）
    bool is_spatial = false;             // 是否为空间索引

    // 默认构造
    IndexDefinition() = default;

    // 单字段索引
    IndexDefinition(const std::string& name, const std::string& field)
        : index_name(name), fields({field}), is_spatial(false) {}

    // 多字段联合索引
    IndexDefinition(const std::string& name, const std::vector<std::string>& field_list)
        : index_name(name), fields(field_list), is_spatial(false) {}

    // 空间索引
    static IndexDefinition Spatial() {
        IndexDefinition def;
        def.is_spatial = true;
        return def;
    }
};

/**
 * 创建空间索引
 *
 * @param gdb_path GDB 目录路径
 * @param layer_name 图层名称
 * @return true 成功，false 失败
 */
bool CreateSpatialIndex(const std::string& gdb_path,
                        const std::string& layer_name);

/**
 * 创建单字段属性索引
 *
 * @param gdb_path GDB 目录路径
 * @param layer_name 图层名称
 * @param field_name 字段名称
 * @param index_name 索引名称（可选，为空则自动生成）
 * @return true 成功，false 失败
 */
bool CreateAttributeIndex(const std::string& gdb_path,
                          const std::string& layer_name,
                          const std::string& field_name,
                          const std::string& index_name = "");

/**
 * 创建联合索引（多字段复合索引）
 *
 * @param gdb_path GDB 目录路径
 * @param layer_name 图层名称
 * @param field_names 字段列表（按顺序）
 * @param index_name 索引名称（可选，为空则自动生成）
 * @return true 成功，false 失败
 */
bool CreateCompositeIndex(const std::string& gdb_path,
                          const std::string& layer_name,
                          const std::vector<std::string>& field_names,
                          const std::string& index_name = "");

/**
 * 根据 IndexDefinition 创建索引（通用接口）
 */
bool CreateIndex(const std::string& gdb_path,
                 const std::string& layer_name,
                 const IndexDefinition& definition);

/**
 * 批量创建多个索引
 */
bool CreateIndexes(const std::string& gdb_path,
                   const std::string& layer_name,
                   const std::vector<IndexDefinition>& definitions);

/**
 * 删除索引
 */
bool DropIndex(const std::string& gdb_path,
               const std::string& index_name);

/**
 * 检查图层是否有空间索引
 */
bool HasSpatialIndex(const std::string& gdb_path,
                     const std::string& layer_name);

}  // namespace writer
}  // namespace explorgdb

#endif  // EXPLORGDB_GDB_INDEX_CREATOR_H
