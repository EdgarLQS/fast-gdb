// src/edgar/explorgdb/reader/metadata_reader.cpp
// 元数据读取器 — 从 GDB_SystemCatalog 相关表解析图层元数据、空间参考、域、关系等。

#include "metadata_reader.h"
#include "gdb_table.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>
#include <unordered_map>

namespace explorgdb {
namespace {

// ========== 内部记录表抽象 ==========

struct ParsedTableRows {
    std::unordered_map<std::string, size_t> columns;  // 小写列名 → 索引
    std::vector<FeatureRecord> rows;
};

// ========== 字段值辅助 ==========

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

int as_int(const FieldValue& value) {
    if (const auto* v = std::get_if<int32_t>(&value)) return *v;
    if (const auto* v = std::get_if<int64_t>(&value)) return static_cast<int>(*v);
    return 0;
}

std::string as_string(const FieldValue& value) {
    if (const auto* v = std::get_if<std::string>(&value)) return *v;
    return {};
}

std::string as_text(const FieldValue& value) {
    if (const auto* v = std::get_if<std::string>(&value)) return *v;
    if (const auto* v = std::get_if<int32_t>(&value)) return std::to_string(*v);
    if (const auto* v = std::get_if<int64_t>(&value)) return std::to_string(*v);
    if (const auto* v = std::get_if<double>(&value)) return std::to_string(*v);
    return {};
}

std::string digest_rows(const ParsedTableRows& table) {
    uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](uint8_t byte) { hash = (hash ^ byte) * 1099511628211ULL; };
    std::vector<std::string> column_names;
    column_names.reserve(table.columns.size());
    for (const auto& column : table.columns) column_names.push_back(column.first);
    std::sort(column_names.begin(), column_names.end());
    for (const auto& column : column_names) {
        for (const auto byte : column) mix(static_cast<uint8_t>(byte));
    }
    for (const auto& row : table.rows) {
        for (const auto& value : row.field_values) {
            const auto text = as_text(value);
            for (const auto byte : text) mix(static_cast<uint8_t>(byte));
            if (const auto* bytes = std::get_if<std::vector<uint8_t>>(&value)) {
                for (const auto byte : *bytes) mix(byte);
            }
            mix(0xffU);
        }
    }
    std::ostringstream result;
    result << std::hex << hash;
    return result.str();
}

/** 从行记录的指定列名查询文本值。 */
std::string lookup_text(const std::unordered_map<std::string, size_t>& columns,
                        const FeatureRecord& row,
                        const char* name) {
    const auto it = columns.find(lower(name));
    if (it == columns.end() || it->second >= row.field_values.size()) return {};
    return as_text(row.field_values[it->second]);
}

bool matches_name(const std::string& candidate, const std::string& requested) {
    return !candidate.empty() && lower(candidate) == lower(requested);
}

// ========== XML 解析辅助 ==========

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

/** 提取 XML 标签内的文本内容（如 <Name>xxx</Name> → "xxx"）。 */
std::string extract_tag_text(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    const auto start = xml.find(open);
    if (start == std::string::npos) return {};
    const auto value_start = start + open.size();
    const auto end = xml.find(close, value_start);
    if (end == std::string::npos) return {};
    return trim(xml.substr(value_start, end - value_start));
}

/** 提取 XML 属性值（如 xsi:type="..." → "..."）。 */
std::string extract_attribute(const std::string& xml, const std::string& key) {
    const std::string double_token = key + "=\"";
    const std::string single_token = key + "='";
    const auto double_start = xml.find(double_token);
    const auto single_start = xml.find(single_token);
    const bool use_single = single_start != std::string::npos &&
                            (double_start == std::string::npos ||
                             single_start < double_start);
    const std::string token = use_single ? single_token : double_token;
    const auto start = use_single ? single_start : double_start;
    if (start == std::string::npos) return {};
    const auto value_start = start + token.size();
    const auto end = xml.find(use_single ? '\'' : '"', value_start);
    if (end == std::string::npos) return {};
    return xml.substr(value_start, end - value_start);
}

/** 从 XML 中提取指定标签的多个块（如 <Domain ...> ... </Domain>）。 */
std::vector<std::string> extract_blocks(const std::string& xml,
                                        const std::string& open_tag_prefix,
                                        const std::string& close_tag) {
    std::vector<std::string> blocks;
    size_t offset = 0;
    while (true) {
        const auto start = xml.find(open_tag_prefix, offset);
        if (start == std::string::npos) break;
        const auto open_end = xml.find('>', start);
        if (open_end == std::string::npos) break;
        const auto end = xml.find(close_tag, open_end + 1);
        if (end == std::string::npos) break;
        blocks.push_back(xml.substr(start, end + close_tag.size() - start));
        offset = end + close_tag.size();
    }
    return blocks;
}

/** 从域 XML 定义中解析 CodedValue 列表。 */
std::vector<DomainCodedValue> decode_coded_values(const std::string& xml) {
    std::vector<DomainCodedValue> values;
    for (const auto& block : extract_blocks(xml, "<CodedValue", "</CodedValue>")) {
        DomainCodedValue value;
        value.code = extract_tag_text(block, "Code");
        value.name = extract_tag_text(block, "Name");
        if (!value.code.empty() || !value.name.empty()) values.push_back(std::move(value));
    }
    return values;
}

// ========== 域名查找 ==========

const DomainInfo* find_domain_by_name(const std::vector<DomainInfo>& domains,
                                      const std::string& domain_name) {
    for (const auto& domain : domains) {
        if (lower(domain.name) == lower(domain_name)) return &domain;
    }
    return nullptr;
}

// ========== 系统表读取 ==========

/** 加载指定系统表的所有记录并建立列名索引。 */
bool load_table_rows(const CatalogResolver& resolver,
                     const std::string& table_name,
                     ParsedTableRows& out) {
    const auto resolved = resolver.resolve(table_name);
    if (!resolved || resolved->tablx_path.empty()) return false;

    GdbTableParser parser(resolved->table_path);
    if (!parser.open() || !parser.load_tablx(resolved->tablx_path)) return false;

    out.columns.clear();
    out.rows.clear();
    for (size_t i = 0; i < parser.fields().size(); ++i) {
        out.columns[lower(parser.fields()[i].name)] = i;
    }
    out.rows.reserve(parser.feature_count());
    for (uint32_t fid = 0; fid < parser.feature_count(); ++fid) {
        FeatureRecord row;
        if (parser.read_record_by_fid(fid, row)) out.rows.push_back(std::move(row));
    }
    return true;
}

/** 按 UUID 从 GDB_Items 表解码单行 LayerMetadata。 */
std::optional<LayerMetadata> decode_layer_metadata_by_uuid(
        const std::unordered_map<std::string, size_t>& columns,
        const FeatureRecord& row,
        const std::string& requested_uuid) {
    const std::string uuid = lookup_text(columns, row, "UUID");
    if (uuid.empty() || lower(uuid) != lower(requested_uuid)) return std::nullopt;

    LayerMetadata metadata;
    metadata.name = lookup_text(columns, row, "Name");
    metadata.path = lookup_text(columns, row, "Path");
    metadata.physical_name = lookup_text(columns, row, "PhysicalName");
    metadata.definition = lookup_text(columns, row, "Definition");
    metadata.documentation = lookup_text(columns, row, "Documentation");
    metadata.uuid = uuid;
    metadata.type = lookup_text(columns, row, "Type");
    metadata.catalog_path = extract_tag_text(metadata.definition, "CatalogPath");
    metadata.dataset_type = extract_tag_text(metadata.definition, "DatasetType");
    for (const auto& entry : columns) {
        if (entry.second >= row.field_values.size()) continue;
        metadata.items.emplace(entry.first, as_text(row.field_values[entry.second]));
        if (const auto* bytes = std::get_if<std::vector<uint8_t>>(
                &row.field_values[entry.second])) {
            metadata.raw_items.emplace(entry.first, *bytes);
        }
    }
    return metadata;
}

/** 获取目录路径的父路径（\ 分隔）。 */
std::string parent_catalog_path(const std::string& path) {
    if (path.empty() || path == "\\") return {};
    const auto pos = path.find_last_of('\\');
    if (pos == std::string::npos || pos == 0) return {};
    return path.substr(0, pos);
}

} // namespace

// ========== 公开方法 ==========

std::optional<SpatialReferenceInfo> MetadataReader::decode_spatial_reference_row(
        const std::unordered_map<std::string, size_t>& columns,
        const FeatureRecord& row,
        int requested_wkid) {
    const auto wkid_it = columns.find("wkid");
    if (wkid_it == columns.end() || wkid_it->second >= row.field_values.size()) return std::nullopt;
    if (as_int(row.field_values[wkid_it->second]) != requested_wkid) return std::nullopt;

    SpatialReferenceInfo info;
    info.wkid = requested_wkid;
    const auto assign_int = [&](const char* name, int& target) {
        const auto it = columns.find(lower(name));
        if (it != columns.end() && it->second < row.field_values.size())
            target = as_int(row.field_values[it->second]);
    };
    const auto assign_string = [&](const char* name, std::string& target) {
        const auto it = columns.find(lower(name));
        if (it != columns.end() && it->second < row.field_values.size())
            target = as_string(row.field_values[it->second]);
    };
    assign_int("LatestWKID", info.latest_wkid);
    assign_string("SRSName", info.name);
    assign_string("WKT", info.wkt);
    if (info.wkt.empty()) assign_string("Definition", info.wkt);
    return info;
}

std::optional<LayerMetadata> MetadataReader::decode_layer_metadata_row(
        const std::unordered_map<std::string, size_t>& columns,
        const FeatureRecord& row,
        const std::string& requested_name) {
    const std::string name = lookup_text(columns, row, "Name");
    const std::string physical_name = lookup_text(columns, row, "PhysicalName");
    if (!matches_name(name, requested_name) && !matches_name(physical_name, requested_name)) {
        return std::nullopt;
    }

    LayerMetadata metadata;
    metadata.name = name;
    metadata.path = lookup_text(columns, row, "Path");
    metadata.physical_name = physical_name;
    metadata.definition = lookup_text(columns, row, "Definition");
    metadata.documentation = lookup_text(columns, row, "Documentation");
    metadata.uuid = lookup_text(columns, row, "UUID");
    metadata.type = lookup_text(columns, row, "Type");
    metadata.catalog_path = extract_tag_text(metadata.definition, "CatalogPath");
    metadata.dataset_type = extract_tag_text(metadata.definition, "DatasetType");

    for (const auto& entry : columns) {
        if (entry.second >= row.field_values.size()) continue;
        metadata.items.emplace(entry.first, as_text(row.field_values[entry.second]));
        if (const auto* bytes = std::get_if<std::vector<uint8_t>>(
                &row.field_values[entry.second])) {
            metadata.raw_items.emplace(entry.first, *bytes);
        }
    }
    return metadata;
}

/**
 * 从 GDB_SpatialRefs 表按 WKID 查询空间参考定义。
 *
 * 返回 WKT + LatestWKID（用于 WKB SRID 到最新 EPSG 编码的映射）。
 */
std::optional<SpatialReferenceInfo> MetadataReader::read_spatial_reference(int requested_wkid) const {
    const auto resolved = resolver_.resolve("GDB_SpatialRefs");
    if (!resolved || resolved->tablx_path.empty()) return std::nullopt;

    GdbTableParser parser(resolved->table_path);
    if (!parser.open() || !parser.load_tablx(resolved->tablx_path)) return std::nullopt;

    std::unordered_map<std::string, size_t> columns;
    for (size_t i = 0; i < parser.fields().size(); ++i)
        columns[lower(parser.fields()[i].name)] = i;

    for (uint32_t fid = 0; fid < parser.feature_count(); ++fid) {
        FeatureRecord row;
        if (!parser.read_record_by_fid(fid, row)) continue;
        const auto decoded = decode_spatial_reference_row(columns, row, requested_wkid);
        if (decoded) return decoded;
    }
    return std::nullopt;
}

/** 从 GDB_Items 表按图层名查找 LayerMetadata。 */
std::optional<LayerMetadata> MetadataReader::read_layer_metadata(const std::string& layer_name) const {
    ParsedTableRows items;
    if (!load_table_rows(resolver_, "GDB_Items", items)) return std::nullopt;

    for (const auto& row : items.rows) {
        const auto decoded = decode_layer_metadata_row(items.columns, row, layer_name);
        if (decoded) return decoded;
    }
    return std::nullopt;
}

/** 从 GDB_Items 表查询指定图层的指定元数据项。 */
std::optional<std::string> MetadataReader::read_metadata_item(const std::string& layer_name,
                                                              const std::string& key) const {
    const auto metadata = read_layer_metadata(layer_name);
    if (!metadata) return std::nullopt;

    const auto it = metadata->items.find(lower(key));
    if (it == metadata->items.end()) return std::nullopt;
    return it->second;
}

/** 从工作空间 XML Definition 中解析域（Domain）定义。 */
std::vector<DomainInfo> MetadataReader::decode_workspace_domains_xml(const std::string& xml) {
    std::vector<DomainInfo> domains;
    auto blocks = extract_blocks(xml, "<Domain ", "</Domain>");
    if (blocks.empty()) blocks = extract_blocks(xml, "<Domain>", "</Domain>");
    if (blocks.empty() && (xml.find("Domain2") != std::string::npos ||
                           xml.find("<DomainName>") != std::string::npos)) {
        blocks.push_back(xml);
    }
    for (const auto& block : blocks) {
        DomainInfo domain;
        domain.type = extract_attribute(block, "xsi:type");
        if (domain.type.empty()) domain.type = "Domain";
        domain.name = extract_tag_text(block, "DomainName");
        if (domain.name.empty()) domain.name = extract_tag_text(block, "Name");
        domain.field_type = extract_tag_text(block, "FieldType");
        domain.description = extract_tag_text(block, "Description");
        domain.merge_policy = extract_tag_text(block, "MergePolicy");
        domain.split_policy = extract_tag_text(block, "SplitPolicy");
        domain.min_value = extract_tag_text(block, "MinValue");
        domain.max_value = extract_tag_text(block, "MaxValue");
        domain.coded_values = decode_coded_values(block);
        if (!domain.name.empty() || !domain.coded_values.empty() ||
            !domain.min_value.empty() || !domain.max_value.empty()) {
            domains.push_back(std::move(domain));
        }
    }
    return domains;
}

/** 读取工作空间层级的域定义。 */
std::vector<DomainInfo> MetadataReader::read_workspace_domains() const {
    ParsedTableRows items;
    if (!load_table_rows(resolver_, "GDB_Items", items)) return {};
    std::vector<DomainInfo> domains;
    for (const auto& row : items.rows) {
        const std::string definition = lookup_text(items.columns, row, "Definition");
        if (definition.find("Domain2") == std::string::npos &&
            definition.find("<DomainName>") == std::string::npos) {
            continue;
        }
        auto decoded = decode_workspace_domains_xml(definition);
        domains.insert(domains.end(), decoded.begin(), decoded.end());
    }
    return domains;
}

std::vector<SubtypeInfo> MetadataReader::decode_subtypes_xml(const std::string& xml) {
    std::vector<SubtypeInfo> subtypes;
    for (const auto& block : extract_blocks(xml, "<Subtype", "</Subtype>")) {
        SubtypeInfo subtype;
        subtype.code = static_cast<int>(std::strtol(
            extract_tag_text(block, "SubtypeCode").c_str(), nullptr, 10));
        subtype.name = extract_tag_text(block, "SubtypeName");
        if (subtype.name.empty()) subtype.name = extract_tag_text(block, "Name");
        subtype.default_value = extract_tag_text(block, "DefaultValue");
        if (subtype.code != 0 || !subtype.name.empty()) {
            subtypes.push_back(std::move(subtype));
        }
    }
    return subtypes;
}

std::vector<SubtypeInfo> MetadataReader::read_subtypes(
        const std::string& layer_name) const {
    const auto metadata = read_layer_metadata(layer_name);
    return metadata ? decode_subtypes_xml(metadata->definition)
                    : std::vector<SubtypeInfo>{};
}

std::vector<MetadataTableAudit> MetadataReader::audit_system_tables() const {
    const std::vector<std::string> names = {
        "GDB_Items", "GDB_ItemRelationships", "GDB_ItemRelationshipTypes",
        "GDB_ItemTypes", "GDB_Datasets", "GDB_DatasetRelationships"};
    std::vector<MetadataTableAudit> audits;
    audits.reserve(names.size());
    for (const auto& name : names) {
        MetadataTableAudit audit;
        audit.table_name = name;
        const auto resolved = resolver_.resolve(name);
        if (!resolved) {
            audit.status = "missing";
            audit.diagnostic = "system table is not present in GDB_SystemCatalog";
            audits.push_back(std::move(audit));
            continue;
        }
        ParsedTableRows rows;
        if (!load_table_rows(resolver_, name, rows)) {
            audit.status = "unreadable";
            audit.diagnostic = "system table or tablx could not be parsed";
        } else {
            audit.status = "ok";
            audit.row_count = rows.rows.size();
            audit.digest = digest_rows(rows);
            if (rows.rows.empty()) audit.status = "empty";
        }
        audits.push_back(std::move(audit));
    }
    return audits;
}

/** 从图层 XML Definition 中解析字段-域绑定关系。 */
std::vector<FieldDomainBinding> MetadataReader::decode_field_domain_bindings_xml(
        const std::string& xml,
        const std::vector<DomainInfo>& workspace_domains) {
    std::vector<FieldDomainBinding> bindings;
    auto field_blocks = extract_blocks(xml, "<GPFieldInfoEx", "</GPFieldInfoEx>");
    if (field_blocks.empty()) field_blocks = extract_blocks(xml, "<FieldInfo", "</FieldInfo>");

    for (const auto& block : field_blocks) {
        FieldDomainBinding binding;
        binding.field_name = extract_tag_text(block, "Name");
        binding.domain_name = extract_tag_text(block, "DomainName");
        if (binding.field_name.empty() || binding.domain_name.empty()) continue;

        if (const auto* domain = find_domain_by_name(workspace_domains, binding.domain_name)) {
            binding.domain = *domain;
        }
        bindings.push_back(std::move(binding));
    }
    return bindings;
}

/** 从关系类 XML Attributes 中解析基数、外键等定义。 */
RelationshipClassDefinition MetadataReader::decode_relationship_class_attributes_xml(
        const std::string& xml) {
    RelationshipClassDefinition def;
    def.cardinality = extract_tag_text(xml, "Cardinality");
    def.notification = extract_tag_text(xml, "Notification");
    def.origin_primary_key = extract_tag_text(xml, "OriginPrimaryKey");
    if (def.origin_primary_key.empty()) {
        def.origin_primary_key = extract_tag_text(xml, "OriginClassKeys");
    }
    def.origin_foreign_key = extract_tag_text(xml, "OriginForeignKey");
    def.destination_primary_key = extract_tag_text(xml, "DestinationPrimaryKey");
    if (def.destination_primary_key.empty()) {
        def.destination_primary_key = extract_tag_text(xml, "DestinationClassKeys");
    }
    def.destination_foreign_key = extract_tag_text(xml, "DestinationForeignKey");
    return def;
}

/** 读取指定图层的字段-域绑定。 */
std::vector<FieldDomainBinding> MetadataReader::read_field_domain_bindings(
        const std::string& layer_name) const {
    const auto metadata = read_layer_metadata(layer_name);
    if (!metadata) return {};
    const auto domains = read_workspace_domains();
    return decode_field_domain_bindings_xml(metadata->definition, domains);
}

/**
 * 读取 GDB_Items / GDB_ItemRelationships / GDB_ItemRelationshipTypes 三表
 * 关联数据，生成关系摘要列表。
 */
std::vector<RelationshipSummary> MetadataReader::read_relationship_summaries() const {
    ParsedTableRows items;
    ParsedTableRows relationships;
    ParsedTableRows relationship_types;
    if (!load_table_rows(resolver_, "GDB_Items", items)) return {};
    if (!load_table_rows(resolver_, "GDB_ItemRelationships", relationships)) return {};
    if (!load_table_rows(resolver_, "GDB_ItemRelationshipTypes", relationship_types)) return {};

    std::unordered_map<std::string, LayerMetadata> items_by_uuid;
    for (const auto& row : items.rows) {
        if (const auto decoded = decode_layer_metadata_by_uuid(items.columns, row,
                                                               lookup_text(items.columns, row, "UUID"))) {
            items_by_uuid.emplace(lower(decoded->uuid), *decoded);
        }
    }

    struct RelationshipTypeSummary {
        std::string name;
        std::string forward_label;
        std::string backward_label;
        bool is_containment = false;
    };
    std::unordered_map<std::string, RelationshipTypeSummary> type_by_uuid;
    for (const auto& row : relationship_types.rows) {
        const std::string uuid = lookup_text(relationship_types.columns, row, "UUID");
        if (uuid.empty()) continue;
        RelationshipTypeSummary type;
        type.name = lookup_text(relationship_types.columns, row, "Name");
        type.forward_label = lookup_text(relationship_types.columns, row, "ForwardLabel");
        type.backward_label = lookup_text(relationship_types.columns, row, "BackwardLabel");
        type.is_containment = as_int(row.field_values[relationship_types.columns["iscontainment"]]) != 0;
        type_by_uuid.emplace(lower(uuid), std::move(type));
    }

    std::vector<RelationshipSummary> summaries;
    summaries.reserve(relationships.rows.size());
    for (const auto& row : relationships.rows) {
        RelationshipSummary summary;
        summary.uuid = lookup_text(relationships.columns, row, "UUID");
        summary.origin_uuid = lookup_text(relationships.columns, row, "OriginID");
        summary.destination_uuid = lookup_text(relationships.columns, row, "DestID");
        summary.type_uuid = lookup_text(relationships.columns, row, "Type");

        const auto type_it = type_by_uuid.find(lower(summary.type_uuid));
        if (type_it != type_by_uuid.end()) {
            summary.type_name = type_it->second.name;
            summary.forward_label = type_it->second.forward_label;
            summary.backward_label = type_it->second.backward_label;
            summary.is_containment = type_it->second.is_containment;
        }

        const auto origin_it = items_by_uuid.find(lower(summary.origin_uuid));
        if (origin_it != items_by_uuid.end()) {
            summary.origin_name = origin_it->second.name;
            summary.origin_path = origin_it->second.catalog_path.empty()
                ? origin_it->second.path : origin_it->second.catalog_path;
        }
        const auto dest_it = items_by_uuid.find(lower(summary.destination_uuid));
        if (dest_it != items_by_uuid.end()) {
            summary.destination_name = dest_it->second.name;
            summary.destination_path = dest_it->second.catalog_path.empty()
                ? dest_it->second.path : dest_it->second.catalog_path;
        }
        summaries.push_back(std::move(summary));
    }
    return summaries;
}

/**
 * 读取关系类定义（包括基数、外键映射、类型层级）。
 *
 * 联合 GDB_Items / GDB_ItemTypes / GDB_ItemRelationships / GDB_ItemRelationshipTypes
 * 四表数据，构建完整的关系类信息。
 */
std::vector<RelationshipClassDefinition> MetadataReader::read_relationship_class_definitions() const {
    ParsedTableRows items;
    ParsedTableRows item_types;
    ParsedTableRows relationships;
    ParsedTableRows relationship_types;
    if (!load_table_rows(resolver_, "GDB_Items", items)) return {};
    if (!load_table_rows(resolver_, "GDB_ItemTypes", item_types)) return {};
    if (!load_table_rows(resolver_, "GDB_ItemRelationships", relationships)) return {};
    if (!load_table_rows(resolver_, "GDB_ItemRelationshipTypes", relationship_types)) return {};

    std::unordered_map<std::string, LayerMetadata> items_by_uuid;
    for (const auto& row : items.rows) {
        const std::string uuid = lookup_text(items.columns, row, "UUID");
        if (uuid.empty()) continue;
        if (const auto decoded = decode_layer_metadata_by_uuid(items.columns, row, uuid)) {
            items_by_uuid.emplace(lower(decoded->uuid), *decoded);
        }
    }

    struct ItemTypeInfo {
        std::string name;
        std::string parent_type_uuid;
    };
    std::unordered_map<std::string, ItemTypeInfo> item_types_by_uuid;
    for (const auto& row : item_types.rows) {
        const std::string uuid = lookup_text(item_types.columns, row, "UUID");
        if (uuid.empty()) continue;
        ItemTypeInfo info;
        info.name = lookup_text(item_types.columns, row, "Name");
        info.parent_type_uuid = lookup_text(item_types.columns, row, "ParentTypeID");
        item_types_by_uuid.emplace(lower(uuid), std::move(info));
    }

    struct RelationshipTypeInfo {
        std::string name;
        std::string origin_item_type_uuid;
        std::string destination_item_type_uuid;
        std::string forward_label;
        std::string backward_label;
        bool is_containment = false;
    };
    std::unordered_map<std::string, RelationshipTypeInfo> relationship_types_by_uuid;
    for (const auto& row : relationship_types.rows) {
        const std::string uuid = lookup_text(relationship_types.columns, row, "UUID");
        if (uuid.empty()) continue;
        RelationshipTypeInfo info;
        info.name = lookup_text(relationship_types.columns, row, "Name");
        info.origin_item_type_uuid = lookup_text(relationship_types.columns, row, "OrigItemTypeID");
        info.destination_item_type_uuid = lookup_text(relationship_types.columns, row, "DestItemTypeID");
        info.forward_label = lookup_text(relationship_types.columns, row, "ForwardLabel");
        info.backward_label = lookup_text(relationship_types.columns, row, "BackwardLabel");
        info.is_containment =
            as_int(row.field_values[relationship_types.columns["iscontainment"]]) != 0;
        relationship_types_by_uuid.emplace(lower(uuid), std::move(info));
    }

    std::vector<RelationshipClassDefinition> definitions;
    definitions.reserve(relationships.rows.size());
    for (const auto& row : relationships.rows) {
        RelationshipClassDefinition def;
        def.relationship_uuid = lookup_text(relationships.columns, row, "UUID");
        def.relationship_type_uuid = lookup_text(relationships.columns, row, "Type");
        def.origin_item_uuid = lookup_text(relationships.columns, row, "OriginID");
        def.destination_item_uuid = lookup_text(relationships.columns, row, "DestID");
        def.attributes = lookup_text(relationships.columns, row, "Attributes");
        if (!def.attributes.empty()) {
            const auto attrs = decode_relationship_class_attributes_xml(def.attributes);
            def.cardinality = attrs.cardinality;
            def.notification = attrs.notification;
            def.origin_primary_key = attrs.origin_primary_key;
            def.origin_foreign_key = attrs.origin_foreign_key;
            def.destination_primary_key = attrs.destination_primary_key;
            def.destination_foreign_key = attrs.destination_foreign_key;
        }
        const auto properties_it = relationships.columns.find("properties");
        if (properties_it != relationships.columns.end() &&
            properties_it->second < row.field_values.size()) {
            def.properties = as_int(row.field_values[properties_it->second]);
        }

        const auto rel_type_it = relationship_types_by_uuid.find(lower(def.relationship_type_uuid));
        if (rel_type_it != relationship_types_by_uuid.end()) {
            def.relationship_type_name = rel_type_it->second.name;
            def.origin_item_type_uuid = rel_type_it->second.origin_item_type_uuid;
            def.destination_item_type_uuid = rel_type_it->second.destination_item_type_uuid;
            def.forward_label = rel_type_it->second.forward_label;
            def.backward_label = rel_type_it->second.backward_label;
            def.is_containment = rel_type_it->second.is_containment;
        }

        const auto origin_type_it = item_types_by_uuid.find(lower(def.origin_item_type_uuid));
        if (origin_type_it != item_types_by_uuid.end()) {
            def.origin_item_type_name = origin_type_it->second.name;
        }
        const auto dest_type_it = item_types_by_uuid.find(lower(def.destination_item_type_uuid));
        if (dest_type_it != item_types_by_uuid.end()) {
            def.destination_item_type_name = dest_type_it->second.name;
        }

        const auto origin_item_it = items_by_uuid.find(lower(def.origin_item_uuid));
        if (origin_item_it != items_by_uuid.end()) {
            def.origin_item_name = origin_item_it->second.name;
            def.origin_item_path = origin_item_it->second.catalog_path.empty()
                ? origin_item_it->second.path : origin_item_it->second.catalog_path;
        }
        const auto dest_item_it = items_by_uuid.find(lower(def.destination_item_uuid));
        if (dest_item_it != items_by_uuid.end()) {
            def.destination_item_name = dest_item_it->second.name;
            def.destination_item_path = dest_item_it->second.catalog_path.empty()
                ? dest_item_it->second.path : dest_item_it->second.catalog_path;
        }
        definitions.push_back(std::move(def));
    }
    return definitions;
}

/** 对图层列表按目录路径分组，生成数据集组摘要。 */
std::vector<DatasetGroupSummary> MetadataReader::summarize_dataset_groups(
        const std::vector<LayerMetadata>& layers) {
    std::map<std::string, DatasetGroupSummary> grouped;
    for (const auto& layer : layers) {
        const std::string path = layer.catalog_path.empty() ? layer.path : layer.catalog_path;
        const std::string parent = parent_catalog_path(path);
        if (parent.empty()) continue;
        auto& summary = grouped[parent];
        summary.group_path = parent;
        summary.member_names.push_back(layer.name);
        summary.member_paths.push_back(path);
    }

    std::vector<DatasetGroupSummary> result;
    result.reserve(grouped.size());
    for (auto& entry : grouped) {
        if (entry.second.member_names.empty()) continue;
        result.push_back(std::move(entry.second));
    }
    return result;
}

/** 从 GDB_Items 表读取所有图层并按目录路径分组。 */
std::vector<DatasetGroupSummary> MetadataReader::read_dataset_group_summaries() const {
    ParsedTableRows items;
    if (!load_table_rows(resolver_, "GDB_Items", items)) return {};

    std::vector<LayerMetadata> layers;
    for (const auto& row : items.rows) {
        LayerMetadata metadata;
        metadata.name = lookup_text(items.columns, row, "Name");
        metadata.path = lookup_text(items.columns, row, "Path");
        metadata.physical_name = lookup_text(items.columns, row, "PhysicalName");
        metadata.definition = lookup_text(items.columns, row, "Definition");
        metadata.catalog_path = extract_tag_text(metadata.definition, "CatalogPath");
        metadata.dataset_type = extract_tag_text(metadata.definition, "DatasetType");
        if (metadata.catalog_path.empty() || metadata.catalog_path == "\\") continue;
        if (metadata.dataset_type.empty()) continue;
        layers.push_back(std::move(metadata));
    }
    return summarize_dataset_groups(layers);
}

} // namespace explorgdb
