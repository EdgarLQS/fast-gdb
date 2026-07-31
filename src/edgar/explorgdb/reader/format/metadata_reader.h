// 文件说明：fast-gdb 源码实现。
// 实现职责：承载对应模块的内部逻辑，具体接口和边界以头文件及项目文档为准。

#ifndef EXPLORGDB_METADATA_READER_H
#define EXPLORGDB_METADATA_READER_H

#include "catalog_resolver.h"
#include "explorgdb_types.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace explorgdb {

struct SpatialReferenceInfo {
    int wkid = 0;
    int latest_wkid = 0;
    std::string name;
    std::string wkt;
};

struct LayerMetadata {
    std::string name;
    std::string path;
    std::string physical_name;
    std::string definition;
    std::string documentation;
    std::string uuid;
    std::string type;
    std::string catalog_path;
    std::string dataset_type;
    std::unordered_map<std::string, std::string> items;
    std::unordered_map<std::string, std::vector<uint8_t>> raw_items;
};

struct MetadataTableAudit {
    std::string table_name;
    std::string status;
    std::string diagnostic;
    size_t row_count = 0;
    std::string digest;
};

struct DomainCodedValue {
    std::string code;
    std::string name;
};

struct DomainInfo {
    std::string name;
    std::string type;
    std::string field_type;
    std::string description;
    std::string merge_policy;
    std::string split_policy;
    std::string min_value;
    std::string max_value;
    std::vector<DomainCodedValue> coded_values;
};

struct FieldDomainBinding {
    std::string field_name;
    std::string domain_name;
    std::optional<DomainInfo> domain;
};

struct SubtypeInfo {
    int code = 0;
    std::string name;
    std::string default_value;
    std::vector<FieldDomainBinding> field_domains;
};

struct RelationshipSummary {
    std::string uuid;
    std::string type_uuid;
    std::string type_name;
    std::string forward_label;
    std::string backward_label;
    bool is_containment = false;
    std::string origin_uuid;
    std::string origin_name;
    std::string origin_path;
    std::string destination_uuid;
    std::string destination_name;
    std::string destination_path;
};

struct RelationshipClassDefinition {
    std::string relationship_uuid;
    std::string relationship_type_uuid;
    std::string relationship_type_name;
    std::string origin_item_type_uuid;
    std::string origin_item_type_name;
    std::string destination_item_type_uuid;
    std::string destination_item_type_name;
    std::string forward_label;
    std::string backward_label;
    std::string cardinality;
    std::string notification;
    std::string origin_primary_key;
    std::string origin_foreign_key;
    std::string destination_primary_key;
    std::string destination_foreign_key;
    bool is_containment = false;
    std::string origin_item_uuid;
    std::string origin_item_name;
    std::string origin_item_path;
    std::string destination_item_uuid;
    std::string destination_item_name;
    std::string destination_item_path;
    std::string attributes;
    int properties = 0;
};

struct DatasetGroupSummary {
    std::string group_path;
    std::vector<std::string> member_names;
    std::vector<std::string> member_paths;
};

class MetadataReader {
public:
    /** 创建元数据读取器。
     * @param resolver 已加载的系统目录解析器；其生命周期必须覆盖本对象。
     */
    explicit MetadataReader(const CatalogResolver& resolver) : resolver_(resolver) {}
    /** 按 WKID 读取空间参考。
     * @param wkid 空间参考标识。
     * @return 空间参考信息；不存在时返回空值。
     */
    std::optional<SpatialReferenceInfo> read_spatial_reference(int wkid) const;
    /** 读取指定图层的元数据。
     * @param layer_name 图层名称。
     * @return 图层元数据；不存在时返回空值。
     */
    std::optional<LayerMetadata> read_layer_metadata(const std::string& layer_name) const;
    /** 读取指定图层的单项元数据。
     * @param layer_name 图层名称。
     * @param key 元数据键。
     * @return 元数据文本；不存在时返回空值。
     */
    std::optional<std::string> read_metadata_item(const std::string& layer_name,
                                                  const std::string& key) const;
    /** 读取工作空间域定义。
     * @return 域信息列表。
     */
    std::vector<DomainInfo> read_workspace_domains() const;
    /** 读取指定图层的字段域绑定。
     * @param layer_name 图层名称。
     * @return 字段域绑定列表。
     */
    std::vector<FieldDomainBinding> read_field_domain_bindings(
        const std::string& layer_name) const;
    /** 读取关系摘要列表。
     * @return 关系摘要列表。
     */
    std::vector<RelationshipSummary> read_relationship_summaries() const;
    /** 读取关系类完整定义。
     * @return 关系类定义列表。
     */
    std::vector<RelationshipClassDefinition> read_relationship_class_definitions() const;
    /** 读取数据集分组摘要。
     * @return 分组摘要列表。
     */
    std::vector<DatasetGroupSummary> read_dataset_group_summaries() const;
    /** 审计系统表是否可读取及字段是否符合预期。
     * @return 系统表审计结果列表。
     */
    std::vector<MetadataTableAudit> audit_system_tables() const;
    /** 读取指定图层的子类型。
     * @param layer_name 图层名称。
     * @return 子类型信息列表。
     */
    std::vector<SubtypeInfo> read_subtypes(const std::string& layer_name) const;
    /** 从 XML 解码子类型信息。
     * @param xml 子类型 XML 文本。
     * @return 解码后的子类型列表。
     */
    static std::vector<SubtypeInfo> decode_subtypes_xml(const std::string& xml);

    // Public, side-effect-free seam used by tests and alternate catalog readers.
    /** 从已解码系统表行构造空间参考信息。
     * @param columns 列名到字段索引的映射。
     * @param row 系统表记录。
     * @param requested_wkid 请求的 WKID。
     * @return 匹配时返回空间参考信息，否则返回空值。
     */
    static std::optional<SpatialReferenceInfo> decode_spatial_reference_row(
        const std::unordered_map<std::string, size_t>& columns,
        const FeatureRecord& row,
        int requested_wkid);
    /** 从已解码系统表行构造图层元数据。
     * @param columns 列名到字段索引的映射。
     * @param row 系统表记录。
     * @param requested_name 请求的图层名称。
     * @return 匹配时返回元数据，否则返回空值。
     */
    static std::optional<LayerMetadata> decode_layer_metadata_row(
        const std::unordered_map<std::string, size_t>& columns,
        const FeatureRecord& row,
        const std::string& requested_name);
    /** 从 XML 解码工作空间域。
     * @param xml 域定义 XML 文本。
     * @return 域信息列表。
     */
    static std::vector<DomainInfo> decode_workspace_domains_xml(const std::string& xml);
    static std::vector<FieldDomainBinding> decode_field_domain_bindings_xml(
        const std::string& xml,
        const std::vector<DomainInfo>& workspace_domains);
    static RelationshipClassDefinition decode_relationship_class_attributes_xml(
        const std::string& xml);
    static std::vector<DatasetGroupSummary> summarize_dataset_groups(
        const std::vector<LayerMetadata>& layers);

private:
    const CatalogResolver& resolver_;
};

} // namespace explorgdb

#endif // EXPLORGDB_METADATA_READER_H
