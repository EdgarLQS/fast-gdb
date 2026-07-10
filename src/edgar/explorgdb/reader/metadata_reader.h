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
    explicit MetadataReader(const CatalogResolver& resolver) : resolver_(resolver) {}
    std::optional<SpatialReferenceInfo> read_spatial_reference(int wkid) const;
    std::optional<LayerMetadata> read_layer_metadata(const std::string& layer_name) const;
    std::optional<std::string> read_metadata_item(const std::string& layer_name,
                                                  const std::string& key) const;
    std::vector<DomainInfo> read_workspace_domains() const;
    std::vector<FieldDomainBinding> read_field_domain_bindings(
        const std::string& layer_name) const;
    std::vector<RelationshipSummary> read_relationship_summaries() const;
    std::vector<RelationshipClassDefinition> read_relationship_class_definitions() const;
    std::vector<DatasetGroupSummary> read_dataset_group_summaries() const;

    // Public, side-effect-free seam used by tests and alternate catalog readers.
    static std::optional<SpatialReferenceInfo> decode_spatial_reference_row(
        const std::unordered_map<std::string, size_t>& columns,
        const FeatureRecord& row,
        int requested_wkid);
    static std::optional<LayerMetadata> decode_layer_metadata_row(
        const std::unordered_map<std::string, size_t>& columns,
        const FeatureRecord& row,
        const std::string& requested_name);
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
