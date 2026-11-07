// src/edgar/explorgdb/reader/format/metadata_reader.cpp
// 元数据读取器 — 从 GDB_SystemCatalog 相关表解析图层元数据、空间参考、域、关系等。

#include "metadata_reader.h"
#include "gdb_table.h"
#include "pugixml.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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

bool lookup_has_value(const std::unordered_map<std::string, size_t>& columns,
                      const FeatureRecord& row,
                      const char* name) {
    const auto it = columns.find(lower(name));
    return it != columns.end() && it->second < row.field_values.size() &&
           !std::holds_alternative<std::nullptr_t>(row.field_values[it->second]);
}

std::optional<std::string> lookup_optional_text(
        const std::unordered_map<std::string, size_t>& columns,
        const FeatureRecord& row,
        const char* name) {
    if (!lookup_has_value(columns, row, name)) return std::nullopt;
    const auto it = columns.find(lower(name));
    return as_text(row.field_values[it->second]);
}

bool matches_name(const std::string& candidate, const std::string& requested) {
    return !candidate.empty() && lower(candidate) == lower(requested);
}

// ========== XML 解析辅助 ==========

std::string_view local_name(std::string_view qualified_name) {
    const auto separator = qualified_name.find_last_of(':');
    return separator == std::string_view::npos
        ? qualified_name : qualified_name.substr(separator + 1);
}

bool is_element_named(pugi::xml_node node, std::string_view name) {
    return node.type() == pugi::node_element &&
           (name.empty() || local_name(node.name()) == local_name(name));
}

void find_elements(pugi::xml_node parent, std::string_view name,
                   std::vector<pugi::xml_node>& result) {
    for (auto child : parent.children()) {
        if (child.type() != pugi::node_element) continue;
        if (is_element_named(child, name)) result.push_back(child);
        find_elements(child, name, result);
    }
}

pugi::xml_node find_element(pugi::xml_node parent, std::string_view name) {
    for (auto child : parent.children()) {
        if (child.type() != pugi::node_element) continue;
        if (is_element_named(child, name)) return child;
        const auto nested = find_element(child, name);
        if (nested) return nested;
    }
    return {};
}

void append_text(pugi::xml_node node, std::string& result) {
    for (auto child : node.children()) {
        if (child.type() == pugi::node_pcdata ||
            child.type() == pugi::node_cdata) {
            result += child.value();
        } else if (child.type() == pugi::node_element) {
            append_text(child, result);
        }
    }
}

std::string trim(std::string value);

std::string node_text(pugi::xml_node node) {
    std::string result;
    append_text(node, result);
    return trim(std::move(result));
}

std::string node_attribute(pugi::xml_node node, std::string_view wanted) {
    for (const auto attribute : node.attributes()) {
        const std::string_view actual(attribute.name());
        if (actual == wanted || local_name(actual) == local_name(wanted)) {
            return attribute.value();
        }
    }
    return {};
}

struct XmlStringWriter final : pugi::xml_writer {
    std::string value;
    void write(const void* data, std::size_t size) override {
        value.append(static_cast<const char*>(data), size);
    }
};

std::string serialize_node(pugi::xml_node node) {
    XmlStringWriter writer;
    node.print(writer, "", pugi::format_raw, pugi::encoding_utf8);
    return std::move(writer.value);
}

bool parse_xml(const std::string& xml, pugi::xml_document& document) {
    return document.load_buffer(xml.data(), xml.size(), pugi::parse_default,
                                pugi::encoding_auto).status == pugi::status_ok;
}

std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string normalize_uuid(std::string value) {
    value = lower(trim(std::move(value)));
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char byte : value) {
        if (byte == '{' || byte == '}' || byte == '-') continue;
        normalized.push_back(static_cast<char>(byte));
    }
    return normalized;
}

bool xml_boolean(const std::string& value) {
    const auto normalized = lower(trim(value));
    return normalized == "true" || normalized == "1" ||
           normalized == "yes";
}

std::optional<bool> optional_xml_boolean(const std::string& value) {
    const auto normalized = lower(trim(value));
    if (normalized == "true" || normalized == "1" ||
        normalized == "yes") {
        return true;
    }
    if (normalized == "false" || normalized == "0" ||
        normalized == "no") {
        return false;
    }
    return std::nullopt;
}

void assign_definition_summary(const std::string& xml, LayerMetadata& metadata) {
    pugi::xml_document document;
    if (!parse_xml(xml, document)) return;
    metadata.catalog_path = node_text(find_element(document, "CatalogPath"));
    metadata.dataset_type = node_text(find_element(document, "DatasetType"));
    const auto spatial_index = find_element(document, "HasSpatialIndex");
    if (spatial_index) {
        metadata.has_spatial_index =
            optional_xml_boolean(node_text(spatial_index));
    }
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
    out.rows.reserve(parser.active_feature_count());
    for (RowSlot fid : parser.active_feature_fids()) {
        FeatureRecord row;
        if (parser.read_record_by_fid(fid, row)) out.rows.push_back(std::move(row));
    }
    return true;
}

struct CatalogItemsRead {
    bool table_loaded = false;
    std::string diagnostic;
    std::vector<FieldDescriptor> fields;
    std::unordered_map<std::string, size_t> columns;
    std::vector<std::pair<RowSlot, FeatureRecord>> rows;
    RowSlot physical_row_slots = 0;
    size_t active_rows = 0;
    size_t decoded_rows = 0;
    std::vector<CatalogIntegrityIssue> issues;
};

CatalogItemsRead load_catalog_items(const CatalogResolver& resolver) {
    CatalogItemsRead result;
    const auto resolved = resolver.resolve("GDB_Items");
    if (!resolved || resolved->tablx_path.empty()) {
        result.diagnostic =
            "GDB_Items is missing from GDB_SystemCatalog or has no tablx";
        return result;
    }

    GdbTableParser parser(resolved->table_path);
    if (!parser.open() || !parser.load_tablx(resolved->tablx_path)) {
        result.diagnostic = "GDB_Items table or tablx could not be parsed";
        return result;
    }
    result.table_loaded = true;
    result.fields = parser.fields();
    for (size_t index = 0; index < result.fields.size(); ++index) {
        result.columns[lower(result.fields[index].name)] = index;
    }
    if (result.columns.find("uuid") == result.columns.end()) {
        result.issues.push_back({
            std::nullopt, {}, CatalogIntegrityIssueCode::MissingRequiredField,
            "GDB_Items schema does not contain the required UUID field"});
    }

    result.physical_row_slots = parser.feature_count();
    for (RowSlot fid : parser.active_feature_fids()) {
        const RowSlot slot = fid;
        ++result.active_rows;

        FeatureRecord row;
        if (!parser.read_record_by_fid(fid, row)) {
            result.issues.push_back({
                slot, {},
                CatalogIntegrityIssueCode::RowReadFailed,
                "active GDB_Items row could not be decoded"});
            continue;
        }
        if (row.field_values.size() != result.fields.size()) {
            result.issues.push_back({
                slot, lookup_text(
                    result.columns, row, "UUID"),
                CatalogIntegrityIssueCode::RowShapeMismatch,
                "decoded field count does not match the GDB_Items schema"});
            continue;
        }
        ++result.decoded_rows;
        result.rows.emplace_back(fid, std::move(row));
    }
    return result;
}

CatalogObjectSummary summarize_catalog_object(
        uint64_t row_slot,
        const std::unordered_map<std::string, size_t>& columns,
        const FeatureRecord& row) {
    CatalogObjectSummary summary;
    summary.row_slot = row_slot;
    summary.uuid = lookup_text(columns, row, "UUID");
    summary.name = lookup_text(columns, row, "Name");
    summary.physical_name = lookup_text(columns, row, "PhysicalName");
    summary.path = lookup_text(columns, row, "Path");
    summary.type = lookup_text(columns, row, "Type");
    summary.definition_present = lookup_has_value(columns, row, "Definition");
    if (summary.definition_present) {
        summary.definition_size =
            lookup_text(columns, row, "Definition").size();
    }
    return summary;
}

CatalogObject materialize_catalog_object(
        RowSlot row_slot,
        const std::vector<FieldDescriptor>& fields,
        const std::unordered_map<std::string, size_t>& columns,
        const FeatureRecord& row) {
    CatalogObject object;
    object.summary = summarize_catalog_object(row_slot, columns, row);
    object.definition_present = object.summary.definition_present;
    object.raw_definition = lookup_text(columns, row, "Definition");
    object.attributes.reserve(fields.size());
    for (size_t index = 0; index < fields.size(); ++index) {
        object.attributes.push_back({fields[index].name, fields[index].type,
                                     row.field_values[index]});
    }
    return object;
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
    metadata.definition_present = lookup_has_value(columns, row, "Definition");
    metadata.documentation = lookup_text(columns, row, "Documentation");
    metadata.uuid = uuid;
    metadata.type = lookup_text(columns, row, "Type");
    assign_definition_summary(metadata.definition, metadata);
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

std::unordered_map<std::string, std::string> load_item_type_names(
        const CatalogResolver& resolver) {
    ParsedTableRows rows;
    std::unordered_map<std::string, std::string> result;
    if (!load_table_rows(resolver, "GDB_ItemTypes", rows)) return result;
    for (const auto& row : rows.rows) {
        const auto uuid = lookup_text(rows.columns, row, "UUID");
        const auto name = lookup_text(rows.columns, row, "Name");
        if (!uuid.empty() && !name.empty()) {
            result.emplace(lower(uuid), name);
        }
    }
    return result;
}

void enrich_catalog_object_summary(
        CatalogObjectSummary& summary,
        const std::optional<std::string>& definition,
        const std::unordered_map<std::string, std::string>& item_type_names,
        const CatalogResolver& resolver) {
    const auto item_type = item_type_names.find(lower(summary.type));
    if (item_type != item_type_names.end()) summary.type_name = item_type->second;

    if (definition.has_value()) {
        pugi::xml_document document;
        if (parse_xml(*definition, document)) {
            summary.dataset_type =
                node_text(find_element(document, "DatasetType"));
            summary.geometry_type =
                node_text(find_element(document, "ShapeType"));
            if (summary.geometry_type.empty()) {
                summary.geometry_type =
                    node_text(find_element(document, "GeometryType"));
            }
        }
    }

    summary.advanced_kind = MetadataReader::classify_advanced_object(
        summary.type_name, summary.dataset_type, summary.geometry_type);
    summary.physical_table_resolves =
        resolver.resolve(summary.name).has_value() ||
        (!summary.physical_name.empty() &&
         resolver.resolve(summary.physical_name).has_value());
    if (summary.advanced_kind == AdvancedObjectKind::None) return;
    summary.semantic_status = definition.has_value()
        ? AdvancedObjectSemanticStatus::RawDefinitionOnly
        : AdvancedObjectSemanticStatus::Unsupported;
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
    metadata.definition_present = lookup_has_value(columns, row, "Definition");
    metadata.documentation = lookup_text(columns, row, "Documentation");
    metadata.uuid = lookup_text(columns, row, "UUID");
    metadata.type = lookup_text(columns, row, "Type");
    assign_definition_summary(metadata.definition, metadata);

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

    for (RowSlot fid : parser.active_feature_fids()) {
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
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
std::vector<DomainInfo> MetadataReader::decode_workspace_domains_xml(const std::string& xml) {
    std::vector<DomainInfo> domains;
    pugi::xml_document document;
    if (!parse_xml(xml, document)) return domains;
    std::vector<pugi::xml_node> domain_elements;
    find_elements(document, "Domain", domain_elements);
    if (domain_elements.empty()) {
        const auto root = document.document_element();
        const std::string_view root_name = local_name(root.name());
        if (root_name == "Domain2" || root_name == "GPCodedValueDomain2" ||
            root_name == "GPRangeDomain2" || root_name == "GPBooleanDomain2") {
            domain_elements.push_back(root);
        }
    }
    for (const auto element : domain_elements) {
        DomainInfo domain;
        domain.type = node_attribute(element, "xsi:type");
        if (domain.type.empty()) {
            domain.type = local_name(element.name());
        }
        domain.name = node_text(find_element(element, "DomainName"));
        if (domain.name.empty()) {
            domain.name = node_text(find_element(element, "Name"));
        }
        domain.field_type = node_text(find_element(element, "FieldType"));
        domain.description = node_text(find_element(element, "Description"));
        domain.merge_policy = node_text(find_element(element, "MergePolicy"));
        domain.split_policy = node_text(find_element(element, "SplitPolicy"));
        domain.min_value = node_text(find_element(element, "MinValue"));
        domain.max_value = node_text(find_element(element, "MaxValue"));
        domain.raw_xml = xml;
        std::vector<pugi::xml_node> coded_values;
        find_elements(element, "CodedValue", coded_values);
        for (const auto coded_value : coded_values) {
            DomainCodedValue value;
            value.code = node_text(find_element(coded_value, "Code"));
            value.name = node_text(find_element(coded_value, "Name"));
            if (!value.code.empty() || !value.name.empty()) {
                domain.coded_values.push_back(std::move(value));
            }
        }
        if (!domain.name.empty() || !domain.coded_values.empty() ||
            !domain.min_value.empty() || !domain.max_value.empty()) {
            domains.push_back(std::move(domain));
        }
    }
    return domains;
}

/** 读取工作空间层级的域定义。 */
std::vector<DomainInfo> MetadataReader::read_workspace_domains() const {
    return read_workspace_domains_result().domains;
}

// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
std::vector<SubtypeInfo> MetadataReader::decode_subtypes_xml(
        const std::string& xml,
        const std::vector<DomainInfo>& workspace_domains) {
    std::vector<SubtypeInfo> subtypes;
    pugi::xml_document document;
    if (!parse_xml(xml, document)) return subtypes;
    std::vector<pugi::xml_node> subtype_elements;
    find_elements(document, "Subtype", subtype_elements);
    for (const auto element : subtype_elements) {
        SubtypeInfo subtype;
        subtype.raw_xml = serialize_node(element);
        subtype.code = static_cast<int>(std::strtol(
            node_text(find_element(element, "SubtypeCode")).c_str(), nullptr, 10));
        subtype.name = node_text(find_element(element, "SubtypeName"));
        if (subtype.name.empty()) {
            subtype.name = node_text(find_element(element, "Name"));
        }
        subtype.default_value = node_text(find_element(element, "DefaultValue"));
        std::vector<pugi::xml_node> field_elements;
        find_elements(element, "SubtypeFieldInfo", field_elements);
        if (field_elements.empty()) {
            find_elements(element, "FieldInfo", field_elements);
        }
        for (const auto field_element : field_elements) {
            SubtypeFieldOverride field;
            field.field_name = node_text(find_element(field_element, "FieldName"));
            if (field.field_name.empty()) {
                field.field_name = node_text(find_element(field_element, "Name"));
            }
            const auto default_value =
                node_text(find_element(field_element, "DefaultValue"));
            if (!default_value.empty()) field.default_value = default_value;
            field.domain_name = node_text(find_element(field_element, "DomainName"));
            if (!field.domain_name.empty()) {
                const auto domain = find_domain_by_name(
                    workspace_domains, field.domain_name);
                if (domain != nullptr) field.domain = *domain;
            }
            if (!field.field_name.empty()) {
                if (!field.domain_name.empty()) {
                    FieldDomainBinding binding;
                    binding.field_name = field.field_name;
                    binding.domain_name = field.domain_name;
                    binding.domain = field.domain;
                    subtype.field_domains.push_back(std::move(binding));
                }
                subtype.field_overrides.push_back(std::move(field));
            }
        }
        if (subtype.code != 0 || !subtype.name.empty()) {
            subtypes.push_back(std::move(subtype));
        }
    }
    return subtypes;
}

std::vector<SubtypeInfo> MetadataReader::read_subtypes(
        const std::string& layer_name) const {
    const auto metadata = read_layer_metadata(layer_name);
    return metadata ? decode_subtypes_xml(
                          metadata->definition, read_workspace_domains())
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
    pugi::xml_document document;
    if (!parse_xml(xml, document)) return bindings;
    std::vector<pugi::xml_node> field_elements;
    find_elements(document, "GPFieldInfoEx", field_elements);
    if (field_elements.empty()) {
        find_elements(document, "FieldInfo", field_elements);
    }

    for (const auto element : field_elements) {
        FieldDomainBinding binding;
        binding.field_name = node_text(find_element(element, "Name"));
        binding.domain_name = node_text(find_element(element, "DomainName"));
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
    pugi::xml_document document;
    if (!parse_xml(xml, document)) return def;
    def.cardinality = node_text(find_element(document, "Cardinality"));
    def.notification = node_text(find_element(document, "Notification"));
    def.origin_primary_key = node_text(find_element(document, "OriginPrimaryKey"));
    if (def.origin_primary_key.empty()) {
        def.origin_primary_key = node_text(find_element(document, "OriginClassKeys"));
    }
    def.origin_foreign_key = node_text(find_element(document, "OriginForeignKey"));
    def.destination_primary_key =
        node_text(find_element(document, "DestinationPrimaryKey"));
    if (def.destination_primary_key.empty()) {
        def.destination_primary_key =
            node_text(find_element(document, "DestinationClassKeys"));
    }
    def.destination_foreign_key =
        node_text(find_element(document, "DestinationForeignKey"));
    return def;
}

RelationshipClassInfo MetadataReader::decode_relationship_class_definition_xml(
        const LayerMetadata& item) {
    RelationshipClassInfo result;
    result.uuid = item.uuid;
    result.name = item.name;
    result.path = item.catalog_path.empty() ? item.path : item.catalog_path;
    result.type = item.dataset_type;
    result.raw_definition = item.definition;
    pugi::xml_document document;
    if (!parse_xml(item.definition, document)) return result;
    result.cardinality = node_text(find_element(document, "Cardinality"));
    result.notification = node_text(find_element(document, "Notification"));
    result.key_type = node_text(find_element(document, "KeyType"));
    result.class_key = node_text(find_element(document, "ClassKey"));
    result.forward_path_label = node_text(find_element(document, "ForwardPathLabel"));
    result.backward_path_label = node_text(find_element(document, "BackwardPathLabel"));
    result.is_attributed = xml_boolean(node_text(find_element(document, "IsAttributed")));
    result.is_composite = xml_boolean(node_text(find_element(document, "IsComposite")));
    result.is_reflexive = xml_boolean(node_text(find_element(document, "IsReflexive")));
    result.is_attachment_relationship =
        xml_boolean(node_text(find_element(document, "IsAttachmentRelationship")));
    result.change_tracked = xml_boolean(node_text(find_element(document, "ChangeTracked")));
    result.replica_tracked = xml_boolean(node_text(find_element(document, "ReplicaTracked")));

    const auto names_in = [&document](const std::string& container_name) {
        std::vector<std::string> names;
        std::vector<pugi::xml_node> containers;
        find_elements(document, container_name, containers);
        for (const auto container : containers) {
            std::vector<pugi::xml_node> name_elements;
            find_elements(container, "Name", name_elements);
            for (const auto name_element : name_elements) {
                auto name = node_text(name_element);
                if (!name.empty()) names.push_back(std::move(name));
            }
        }
        return names;
    };
    result.origin_class_names = names_in("OriginClassNames");
    result.destination_class_names = names_in("DestinationClassNames");

    const auto append_keys = [&result, &document](const std::string& container_name) {
        std::vector<pugi::xml_node> containers;
        find_elements(document, container_name, containers);
        for (const auto container : containers) {
            std::vector<pugi::xml_node> key_elements;
            find_elements(container, "RelationshipClassKey", key_elements);
            for (const auto key_element : key_elements) {
                RelationshipClassKeyInfo key;
                key.object_key_name =
                    node_text(find_element(key_element, "ObjectKeyName"));
                key.class_key_name =
                    node_text(find_element(key_element, "ClassKeyName"));
                key.key_role = node_text(find_element(key_element, "KeyRole"));
                result.keys.push_back(std::move(key));
            }
        }
    };
    append_keys("OriginClassKeys");
    append_keys("DestinationClassKeys");
    return result;
}

LayerDefinitionDefaults MetadataReader::decode_layer_definition_defaults(
        const std::string& xml) {
    LayerDefinitionDefaults result;
    pugi::xml_document document;
    if (!parse_xml(xml, document)) return result;

    result.area_field = node_text(find_element(document, "AreaFieldName"));
    result.length_field = node_text(find_element(document, "LengthFieldName"));
    const auto shape_type = node_text(find_element(document, "ShapeType"));
    uint32_t base_type = 0;
    if (lower(shape_type) == "esrigeometrypoint") base_type = 1;
    if (lower(shape_type) == "esrigeometrymultipoint") base_type = 4;
    if (lower(shape_type) == "esrigeometrypolyline") base_type = 5;
    if (lower(shape_type) == "esrigeometrypolygon") base_type = 6;
    if (base_type != 0) {
        const bool has_z = xml_boolean(
            node_text(find_element(document, "HasZ")));
        const bool has_m = xml_boolean(
            node_text(find_element(document, "HasM")));
        result.geometry_type = base_type + (has_z && has_m
            ? 3000U : (has_z ? 1000U : (has_m ? 2000U : 0U)));
    }

    std::vector<pugi::xml_node> field_elements;
    find_elements(document, "GPFieldInfoEx", field_elements);
    for (const auto field : field_elements) {
        const auto name = node_text(find_element(field, "Name"));
        auto value = node_text(find_element(field, "DefaultValueNumeric"));
        if (value.empty()) value = node_text(find_element(field, "DefaultValue"));
        if (value.empty()) {
            value = node_text(find_element(field, "DefaultValueInteger"));
        }
        if (!name.empty() && !value.empty()) {
            result.field_defaults.emplace_back(name, value);
        }
    }
    return result;
}

MetadataXmlInspection MetadataReader::inspect_metadata_xml(
        std::optional<std::string_view> xml, MetadataXmlKind kind) {
    if (!xml) {
        return {MetadataXmlStatus::Missing, "XML payload is missing"};
    }
    if (xml->find_first_not_of(" \t\r\n") == std::string_view::npos) {
        return {MetadataXmlStatus::Empty, "XML payload is empty"};
    }

    pugi::xml_document document;
    const auto parsed = document.load_buffer(
        xml->data(), xml->size(), pugi::parse_default, pugi::encoding_auto);
    if (!parsed) {
        return {MetadataXmlStatus::Malformed,
                std::string("pugixml parse error: ") + parsed.description() +
                    " at byte " + std::to_string(parsed.offset)};
    }

    const auto root = document.document_element();
    if (!root) {
        return {MetadataXmlStatus::Malformed,
                "XML document has no root element"};
    }
    const std::string root_name(local_name(root.name()));
    const auto has = [&document](std::string_view name) {
        std::vector<pugi::xml_node> elements;
        find_elements(document, name, elements);
        return !elements.empty();
    };
    const auto known_layer_root = [&root_name]() {
        return root_name == "DEFeatureClassInfo" ||
               root_name == "DETableInfo" ||
               root_name == "DERelationshipClassInfo" ||
               root_name == "DEFeatureDataset" ||
               root_name == "DEFeatureDatasetInfo";
    };

    switch (kind) {
        case MetadataXmlKind::WorkspaceDomains:
            if (root_name == "Domain" || root_name == "Domain2" ||
                root_name == "GPCodedValueDomain2" ||
                root_name == "GPRangeDomain2" ||
                root_name == "GPBooleanDomain2") {
                return {MetadataXmlStatus::Available, {}};
            }
            if (root_name == "DEWorkspace" || root_name == "Domains" ||
                root_name == "ArrayOfDomain") {
                return has("Domain")
                    ? MetadataXmlInspection{MetadataXmlStatus::Available, {}}
                    : MetadataXmlInspection{MetadataXmlStatus::Empty,
                                            "workspace XML contains no domains"};
            }
            break;
        case MetadataXmlKind::LayerDefinition:
            if (known_layer_root() || root_name == "Domain2") {
                return {MetadataXmlStatus::Available, {}};
            }
            break;
        case MetadataXmlKind::FieldDomainBindings:
            if (root_name == "DEFeatureClassInfo" ||
                root_name == "DETableInfo") {
                std::vector<pugi::xml_node> fields;
                find_elements(document, "GPFieldInfoEx", fields);
                if (fields.empty()) find_elements(document, "FieldInfo", fields);
                if (fields.empty()) {
                    return {MetadataXmlStatus::Missing,
                            "field-domain binding elements are absent"};
                }
                for (const auto field : fields) {
                    if (!node_text(find_element(field, "DomainName")).empty()) {
                        return {MetadataXmlStatus::Available, {}};
                    }
                }
                return {MetadataXmlStatus::Empty,
                        "field metadata is present but no domain is bound"};
            }
            break;
        case MetadataXmlKind::Subtypes:
            if (root_name == "DEFeatureClassInfo" ||
                root_name == "DETableInfo") {
                if (has("Subtype")) {
                    return {MetadataXmlStatus::Available, {}};
                }
                if (has("Subtypes")) {
                    return {MetadataXmlStatus::Empty,
                            "subtype container is empty"};
                }
                return {MetadataXmlStatus::Missing,
                        "subtype definition is absent"};
            }
            break;
        case MetadataXmlKind::RelationshipClass:
            if (root_name == "DERelationshipClassInfo") {
                return {MetadataXmlStatus::Available, {}};
            }
            break;
        case MetadataXmlKind::FeatureDataset:
            if (root_name == "DEFeatureDataset" ||
                root_name == "DEFeatureDatasetInfo") {
                return {MetadataXmlStatus::Available, {}};
            }
            break;
    }

    return {MetadataXmlStatus::UnknownStructure,
            "well-formed XML has an unrecognized root element: " + root_name};
}

WorkspaceDomainsReadResult MetadataReader::read_workspace_domains_result() const {
    WorkspaceDomainsReadResult result;
    if (!resolver_.resolve("GDB_Items")) {
        result.inspection = {MetadataXmlStatus::Missing,
                             "GDB_Items is missing from GDB_SystemCatalog"};
        return result;
    }

    ParsedTableRows items;
    if (!load_table_rows(resolver_, "GDB_Items", items)) {
        result.inspection = {MetadataXmlStatus::Unavailable,
                             "GDB_Items could not be read to inspect domain definitions"};
        return result;
    }

    std::unordered_map<std::string, std::string> item_type_names;
    ParsedTableRows item_types;
    if (load_table_rows(resolver_, "GDB_ItemTypes", item_types)) {
        for (const auto& row : item_types.rows) {
            const auto uuid = lookup_text(item_types.columns, row, "UUID");
            const auto name = lookup_text(item_types.columns, row, "Name");
            if (!uuid.empty() && !name.empty()) {
                item_type_names.emplace(lower(uuid), name);
            }
        }
    }

    bool found_domain_item = false;
    MetadataXmlInspection issue;
    for (const auto& row : items.rows) {
        const auto type_uuid = lower(lookup_text(items.columns, row, "Type"));
        const auto type = item_type_names.find(type_uuid);
        const bool is_domain_item = type != item_type_names.end() &&
            lower(type->second).find("domain") != std::string::npos;
        if (!is_domain_item && !item_type_names.empty()) continue;
        const auto definition =
            lookup_optional_text(items.columns, row, "Definition");
        if (!definition) {
            if (is_domain_item) {
                found_domain_item = true;
                issue = {MetadataXmlStatus::Missing,
                         "a domain item has a NULL Definition field"};
            }
            continue;
        }
        const auto inspection = inspect_metadata_xml(
            std::string_view(*definition), MetadataXmlKind::WorkspaceDomains);
        if (inspection.status == MetadataXmlStatus::Available) {
            found_domain_item = true;
            auto domains = decode_workspace_domains_xml(*definition);
            if (domains.empty()) {
                issue = {MetadataXmlStatus::UnknownStructure,
                         "recognized domain XML has no supported domain values"};
            } else {
                result.domains.insert(result.domains.end(),
                                      std::make_move_iterator(domains.begin()),
                                      std::make_move_iterator(domains.end()));
            }
        } else if (is_domain_item) {
            found_domain_item = true;
            if (inspection.status == MetadataXmlStatus::Malformed ||
                inspection.status == MetadataXmlStatus::UnknownStructure) {
                if (issue.status != MetadataXmlStatus::Malformed ||
                    inspection.status == MetadataXmlStatus::Malformed) {
                    issue = inspection;
                }
            } else if (inspection.status == MetadataXmlStatus::Empty &&
                       issue.status == MetadataXmlStatus::Missing) {
                issue = inspection;
            }
        }
    }

    if (issue.status == MetadataXmlStatus::Malformed ||
        issue.status == MetadataXmlStatus::UnknownStructure) {
        result.inspection = std::move(issue);
        return result;
    }
    if (!result.domains.empty()) {
        result.inspection = {MetadataXmlStatus::Available, issue.diagnostic};
    } else if (found_domain_item &&
               (issue.status == MetadataXmlStatus::Missing ||
                issue.status == MetadataXmlStatus::Empty)) {
        result.inspection = std::move(issue);
    } else {
        result.inspection = {MetadataXmlStatus::Empty,
            found_domain_item ? "domain items have no XML definitions"
                              : "workspace contains no domain definitions"};
    }
    return result;
}

std::vector<RelationshipClassInfo>
MetadataReader::read_relationship_classes() const {
    return read_relationship_classes_result().relationships;
}

WorkspaceRelationshipClassesReadResult
MetadataReader::read_relationship_classes_result() const {
    WorkspaceRelationshipClassesReadResult result;
    if (!resolver_.resolve("GDB_Items")) {
        result.inspection = {MetadataXmlStatus::Missing,
                             "GDB_Items is missing from GDB_SystemCatalog"};
        return result;
    }
    ParsedTableRows items;
    if (!load_table_rows(resolver_, "GDB_Items", items)) {
        result.inspection = {MetadataXmlStatus::Unavailable,
                             "GDB_Items could not be read to inspect relationship definitions"};
        return result;
    }

    std::unordered_map<std::string, std::string> item_type_names;
    ParsedTableRows item_types;
    if (load_table_rows(resolver_, "GDB_ItemTypes", item_types)) {
        for (const auto& row : item_types.rows) {
            const auto uuid = lookup_text(item_types.columns, row, "UUID");
            const auto name = lookup_text(item_types.columns, row, "Name");
            if (!uuid.empty() && !name.empty()) {
                item_type_names.emplace(lower(uuid), name);
            }
        }
    }

    std::unordered_set<std::string> relationship_item_uuids;
    MetadataXmlInspection issue;
    for (const auto& row : items.rows) {
        const auto uuid = lower(lookup_text(items.columns, row, "UUID"));
        const auto type_uuid = lower(lookup_text(items.columns, row, "Type"));
        const auto type = item_type_names.find(type_uuid);
        const bool is_relationship_item = type != item_type_names.end() &&
            lower(type->second).find("relationship class") != std::string::npos;
        if (!is_relationship_item && !item_type_names.empty()) continue;

        const auto definition =
            lookup_optional_text(items.columns, row, "Definition");
        if (!definition) {
            if (is_relationship_item && issue.status == MetadataXmlStatus::Missing) {
                issue = {MetadataXmlStatus::Missing,
                         "a relationship class item has a NULL Definition field"};
            }
            continue;
        }
        const auto inspection = inspect_metadata_xml(
            std::string_view(*definition), MetadataXmlKind::RelationshipClass);
        if (inspection.status == MetadataXmlStatus::Available) {
            relationship_item_uuids.insert(uuid);
        } else if (is_relationship_item &&
                   (inspection.status == MetadataXmlStatus::Malformed ||
                    inspection.status == MetadataXmlStatus::UnknownStructure)) {
            if (issue.status != MetadataXmlStatus::Malformed ||
                inspection.status == MetadataXmlStatus::Malformed) {
                issue = inspection;
            }
        } else if (is_relationship_item &&
                   inspection.status == MetadataXmlStatus::Empty &&
                   issue.status == MetadataXmlStatus::Missing) {
            issue = inspection;
        }
    }

    std::unordered_map<std::string, LayerMetadata> items_by_name;
    std::vector<LayerMetadata> all_items;
    all_items.reserve(items.rows.size());
    for (const auto& row : items.rows) {
        const std::string uuid = lookup_text(items.columns, row, "UUID");
        std::optional<LayerMetadata> decoded;
        if (!uuid.empty()) {
            decoded = decode_layer_metadata_by_uuid(items.columns, row, uuid);
        }
        if (!decoded) {
            decoded = decode_layer_metadata_row(
                items.columns, row, lookup_text(items.columns, row, "Name"));
        }
        if (!decoded) continue;
        items_by_name.emplace(lower(decoded->name), *decoded);
        all_items.push_back(std::move(*decoded));
    }

    for (const auto& item : all_items) {
        if (relationship_item_uuids.find(lower(item.uuid)) ==
                relationship_item_uuids.end() &&
            item.dataset_type != "esriDTRelationshipClass") {
            continue;
        }
        auto relationship = decode_relationship_class_definition_xml(item);
        for (const auto& name : relationship.origin_class_names) {
            const auto found = items_by_name.find(lower(name));
            relationship.origin_class_paths.push_back(
                found == items_by_name.end()
                    ? std::string{}
                    : (found->second.catalog_path.empty()
                           ? found->second.path
                           : found->second.catalog_path));
            relationship.origin_class_uuids.push_back(
                found == items_by_name.end() ? std::string{}
                                             : found->second.uuid);
        }
        for (const auto& name : relationship.destination_class_names) {
            const auto found = items_by_name.find(lower(name));
            relationship.destination_class_paths.push_back(
                found == items_by_name.end()
                    ? std::string{}
                    : (found->second.catalog_path.empty()
                           ? found->second.path
                           : found->second.catalog_path));
            relationship.destination_class_uuids.push_back(
                found == items_by_name.end() ? std::string{}
                                             : found->second.uuid);
        }
        result.relationships.push_back(std::move(relationship));
    }

    if (issue.status == MetadataXmlStatus::Malformed ||
        issue.status == MetadataXmlStatus::UnknownStructure) {
        result.inspection = std::move(issue);
    } else if (!result.relationships.empty()) {
        result.inspection = {MetadataXmlStatus::Available, {}};
    } else if (issue.status == MetadataXmlStatus::Empty) {
        result.inspection = std::move(issue);
    } else {
        result.inspection = {MetadataXmlStatus::Empty,
                             "workspace contains no relationship class definitions"};
    }

    result.attachments = summarize_attachment_relationships(
        result.relationships, resolver_);
    if (result.attachments.empty() &&
        (result.inspection.status == MetadataXmlStatus::Available ||
         result.inspection.status == MetadataXmlStatus::Empty)) {
        result.attachments_inspection = {
            MetadataXmlStatus::Empty,
            "workspace contains no attachment relationships"};
    } else {
        result.attachments_inspection = result.inspection;
    }
    return result;
}

WorkspaceAttachmentsReadResult
MetadataReader::read_attachment_relationships_result() const {
    auto relationships = read_relationship_classes_result();
    WorkspaceAttachmentsReadResult result;
    result.attachments = std::move(relationships.attachments);
    result.inspection = std::move(relationships.attachments_inspection);
    return result;
}

std::vector<AttachmentRelationshipInfo>
MetadataReader::summarize_attachment_relationships(
        const std::vector<RelationshipClassInfo>& relationships,
        const CatalogResolver& resolver) {
    constexpr std::string_view attachment_suffix = "__attach";
    const auto has_attachment_suffix = [attachment_suffix](
            const std::string& name) {
        const auto normalized = lower(name);
        return normalized.size() >= attachment_suffix.size() &&
            normalized.compare(normalized.size() - attachment_suffix.size(),
                               attachment_suffix.size(), attachment_suffix) == 0;
    };
    const auto endpoint_value = [](const std::vector<std::string>& values,
                                   std::size_t index) -> std::string {
        return index < values.size() ? values[index] : std::string{};
    };

    std::vector<AttachmentRelationshipInfo> result;
    for (const auto& relationship : relationships) {
        if (!relationship.is_attachment_relationship) continue;

        bool table_is_origin = false;
        std::size_t table_index = 0;
        bool suffix_identified = false;
        for (std::size_t index = 0;
             index < relationship.destination_class_names.size(); ++index) {
            if (has_attachment_suffix(
                    relationship.destination_class_names[index])) {
                table_index = index;
                suffix_identified = true;
                break;
            }
        }
        if (!suffix_identified) {
            for (std::size_t index = 0;
                 index < relationship.origin_class_names.size(); ++index) {
                if (has_attachment_suffix(
                        relationship.origin_class_names[index])) {
                    table_is_origin = true;
                    table_index = index;
                    suffix_identified = true;
                    break;
                }
            }
        }

        AttachmentRelationshipInfo attachment;
        attachment.relationship_uuid = relationship.uuid;
        attachment.relationship_name = relationship.name;
        attachment.relationship_path = relationship.path;
        attachment.cardinality = relationship.cardinality;
        attachment.keys = relationship.keys;
        attachment.raw_definition = relationship.raw_definition;
        attachment.attachment_table_inferred = !suffix_identified;

        if (suffix_identified && table_is_origin) {
            attachment.attachment_table_name = endpoint_value(
                relationship.origin_class_names, table_index);
            attachment.attachment_table_uuid = endpoint_value(
                relationship.origin_class_uuids, table_index);
            attachment.attachment_table_path = endpoint_value(
                relationship.origin_class_paths, table_index);
            if (!relationship.destination_class_names.empty()) {
                attachment.origin_class_name =
                    relationship.destination_class_names.front();
                attachment.origin_class_uuid = endpoint_value(
                    relationship.destination_class_uuids, 0);
                attachment.origin_class_path = endpoint_value(
                    relationship.destination_class_paths, 0);
            }
        } else {
            if (relationship.destination_class_names.empty() &&
                !relationship.origin_class_names.empty()) {
                table_is_origin = true;
                table_index = 0;
            }
            const auto& table_names = table_is_origin
                ? relationship.origin_class_names
                : relationship.destination_class_names;
            const auto& table_uuids = table_is_origin
                ? relationship.origin_class_uuids
                : relationship.destination_class_uuids;
            const auto& table_paths = table_is_origin
                ? relationship.origin_class_paths
                : relationship.destination_class_paths;
            attachment.attachment_table_name =
                endpoint_value(table_names, table_index);
            attachment.attachment_table_uuid =
                endpoint_value(table_uuids, table_index);
            attachment.attachment_table_path =
                endpoint_value(table_paths, table_index);

            const auto& origin_names = table_is_origin
                ? relationship.destination_class_names
                : relationship.origin_class_names;
            const auto& origin_uuids = table_is_origin
                ? relationship.destination_class_uuids
                : relationship.origin_class_uuids;
            const auto& origin_paths = table_is_origin
                ? relationship.destination_class_paths
                : relationship.origin_class_paths;
            if (!origin_names.empty()) {
                attachment.origin_class_name = origin_names.front();
                attachment.origin_class_uuid = endpoint_value(origin_uuids, 0);
                attachment.origin_class_path = endpoint_value(origin_paths, 0);
            }
        }

        if (attachment.attachment_table_path.empty()) {
            attachment.attachment_table_path = attachment.attachment_table_name;
        }
        if (attachment.origin_class_path.empty()) {
            attachment.origin_class_path = attachment.origin_class_name;
        }
        attachment.attachment_table_present =
            !attachment.attachment_table_name.empty() &&
            resolver.resolve(attachment.attachment_table_name).has_value();
        result.push_back(std::move(attachment));
    }
    return result;
}

AdvancedObjectKind MetadataReader::classify_advanced_object(
        const std::string& type_name,
        const std::string& dataset_type,
        const std::string& geometry_type) {
    const std::string type = lower(type_name);
    const std::string dataset = lower(dataset_type);
    const std::string geometry = lower(geometry_type);
    const auto contains = [](const std::string& value,
                             std::string_view fragment) {
        return value.find(fragment) != std::string::npos;
    };
    if (contains(type, "annotation") || contains(dataset, "annotation") ||
        contains(geometry, "annotation")) {
        return AdvancedObjectKind::Annotation;
    }
    if (contains(type, "dimension") || contains(dataset, "dimension") ||
        contains(geometry, "dimension")) {
        return AdvancedObjectKind::Dimension;
    }
    if (contains(type, "topology") || contains(dataset, "topology")) {
        return AdvancedObjectKind::Topology;
    }
    if (contains(type, "geometric network") ||
        contains(dataset, "geometric network")) {
        return AdvancedObjectKind::GeometricNetwork;
    }
    if (contains(type, "network dataset") ||
        contains(dataset, "network dataset")) {
        return AdvancedObjectKind::NetworkDataset;
    }
    if (contains(type, "network diagram") ||
        contains(dataset, "network diagram")) {
        return AdvancedObjectKind::NetworkDiagram;
    }
    if (contains(type, "trace network") ||
        contains(dataset, "trace network")) {
        return AdvancedObjectKind::TraceNetwork;
    }
    if (contains(type, "utility network") ||
        contains(dataset, "utility network")) {
        return AdvancedObjectKind::UtilityNetwork;
    }
    if (contains(type, "parcel fabric") ||
        contains(dataset, "parcel fabric")) {
        return AdvancedObjectKind::ParcelFabric;
    }
    if (contains(type, "terrain") || contains(dataset, "terrain")) {
        return AdvancedObjectKind::Terrain;
    }
    if (contains(type, "mosaic dataset") ||
        contains(dataset, "mosaic dataset")) {
        return AdvancedObjectKind::MosaicDataset;
    }
    if (type == "tin" || contains(dataset, "tin")) {
        return AdvancedObjectKind::Tin;
    }
    return AdvancedObjectKind::None;
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
        if (layer.dataset_type == "esriDTFeatureDataset") {
            auto& summary = grouped[path];
            summary.group_path = path;
            summary.raw_definition = layer.definition;
            continue;
        }
        const std::string parent = parent_catalog_path(path);
        if (parent.empty()) continue;
        auto& summary = grouped[parent];
        summary.group_path = parent;
        summary.member_names.push_back(layer.name);
        summary.member_paths.push_back(path);
        summary.member_raw_definitions.push_back(layer.definition);
    }

    std::vector<DatasetGroupSummary> result;
    result.reserve(grouped.size());
    for (auto& entry : grouped) {
        if (entry.second.member_names.empty() &&
            entry.second.raw_definition.empty()) {
            continue;
        }
        result.push_back(std::move(entry.second));
    }
    return result;
}

/** 从 GDB_Items 表读取所有图层并按目录路径分组。 */
std::vector<DatasetGroupSummary> MetadataReader::read_dataset_group_summaries() const {
    return read_dataset_group_summaries_result().groups;
}

CatalogObjectInventory MetadataReader::read_catalog_object_inventory() const {
    CatalogObjectInventory inventory;
    auto table = load_catalog_items(resolver_);
    inventory.physical_row_slots = table.physical_row_slots;
    inventory.active_rows = table.active_rows;
    inventory.decoded_rows = table.decoded_rows;
    inventory.issues = std::move(table.issues);
    if (!table.table_loaded) {
        inventory.status = CatalogInventoryStatus::Unavailable;
        inventory.diagnostic = std::move(table.diagnostic);
        return inventory;
    }

    const auto item_type_names = load_item_type_names(resolver_);
    std::unordered_map<std::string, size_t> object_by_uuid;
    for (const auto& [row_slot, row] : table.rows) {
        auto summary = summarize_catalog_object(row_slot, table.columns, row);
        enrich_catalog_object_summary(
            summary,
            lookup_optional_text(table.columns, row, "Definition"),
            item_type_names, resolver_);
        const auto uuid_key = normalize_uuid(summary.uuid);
        if (uuid_key.empty()) {
            inventory.issues.push_back({
                row_slot, {}, CatalogIntegrityIssueCode::EmptyUuid,
                "active GDB_Items row has an empty or NULL UUID"});
        } else {
            const auto previous = object_by_uuid.find(uuid_key);
            if (previous != object_by_uuid.end()) {
                inventory.issues.push_back({
                    inventory.objects[previous->second].row_slot, summary.uuid,
                    CatalogIntegrityIssueCode::DuplicateUuid,
                    "UUID occurs in more than one active GDB_Items row"});
                inventory.issues.push_back({
                    row_slot, summary.uuid,
                    CatalogIntegrityIssueCode::DuplicateUuid,
                    "UUID occurs in more than one active GDB_Items row"});
            } else {
                object_by_uuid.emplace(uuid_key, inventory.objects.size());
            }
        }
        inventory.objects.push_back(std::move(summary));
    }

    if (!inventory.issues.empty()) {
        inventory.status = CatalogInventoryStatus::Partial;
        inventory.diagnostic = "GDB_Items inventory has " +
            std::to_string(inventory.issues.size()) +
            " integrity issue(s); inspect the issue list";
    } else if (inventory.active_rows == 0) {
        inventory.status = CatalogInventoryStatus::Empty;
    } else {
        inventory.status = CatalogInventoryStatus::Complete;
    }
    return inventory;
}

CatalogObjectReadResult MetadataReader::read_catalog_object_by_uuid(
        const std::string& uuid) const {
    CatalogObjectReadResult result;
    const std::string normalized_uuid = normalize_uuid(uuid);
    if (normalized_uuid.empty()) {
        result.status = CatalogObjectReadStatus::InvalidArgument;
        result.diagnostic = "catalog object UUID must not be empty";
        return result;
    }

    auto table = load_catalog_items(resolver_);
    result.integrity_issues = std::move(table.issues);
    if (!table.table_loaded) {
        result.status = CatalogObjectReadStatus::Unavailable;
        result.diagnostic = std::move(table.diagnostic);
        return result;
    }
    if (table.columns.find("uuid") == table.columns.end()) {
        result.status = CatalogObjectReadStatus::Unavailable;
        result.diagnostic =
            "GDB_Items schema has no UUID field; object lookup is unavailable";
        return result;
    }

    const std::pair<RowSlot, FeatureRecord>* match = nullptr;
    bool duplicate = false;
    std::unordered_map<std::string, uint32_t> seen_uuids;
    for (const auto& item : table.rows) {
        const auto row_uuid = lookup_text(table.columns, item.second, "UUID");
        if (row_uuid.empty()) {
            result.integrity_issues.push_back({
                item.first, {}, CatalogIntegrityIssueCode::EmptyUuid,
                "active GDB_Items row has an empty or NULL UUID"});
            continue;
        }
        const auto key = normalize_uuid(row_uuid);
        const auto [previous, inserted] = seen_uuids.emplace(key, item.first);
        if (!inserted) {
            result.integrity_issues.push_back({
                previous->second, row_uuid,
                CatalogIntegrityIssueCode::DuplicateUuid,
                "UUID occurs in more than one active GDB_Items row"});
            result.integrity_issues.push_back({
                item.first, row_uuid,
                CatalogIntegrityIssueCode::DuplicateUuid,
                "UUID occurs in more than one active GDB_Items row"});
            if (key == normalized_uuid) duplicate = true;
        }
        if (key == normalized_uuid) {
            if (match != nullptr) duplicate = true;
            else match = &item;
        }
    }

    if (duplicate) {
        result.status = CatalogObjectReadStatus::Ambiguous;
        result.diagnostic =
            "UUID matches multiple GDB_Items rows; no object was selected";
        return result;
    }
    if (match == nullptr) {
        if (!result.integrity_issues.empty()) {
            result.status = CatalogObjectReadStatus::Unavailable;
            result.diagnostic =
                "UUID was not found, but GDB_Items has unreadable or invalid rows";
        } else {
            result.status = CatalogObjectReadStatus::NotFound;
            result.diagnostic = "UUID does not occur in GDB_Items";
        }
        return result;
    }

    result.object = materialize_catalog_object(
        match->first, table.fields, table.columns, match->second);
    enrich_catalog_object_summary(
        result.object->summary,
        lookup_optional_text(table.columns, match->second, "Definition"),
        load_item_type_names(resolver_), resolver_);
    if (result.integrity_issues.empty()) {
        result.status = CatalogObjectReadStatus::Available;
    } else {
        result.status = CatalogObjectReadStatus::Partial;
        result.diagnostic =
            "object was read, but the GDB_Items inventory is incomplete";
    }
    return result;
}

WorkspaceDatasetGroupsReadResult
MetadataReader::read_dataset_group_summaries_result() const {
    WorkspaceDatasetGroupsReadResult result;
    if (!resolver_.resolve("GDB_Items")) {
        result.inspection = {MetadataXmlStatus::Missing,
                             "GDB_Items is missing from GDB_SystemCatalog"};
        return result;
    }

    ParsedTableRows items;
    if (!load_table_rows(resolver_, "GDB_Items", items)) {
        result.inspection = {MetadataXmlStatus::Unavailable,
                             "GDB_Items could not be read to inspect Feature Dataset definitions"};
        return result;
    }

    std::unordered_map<std::string, std::string> item_type_names;
    ParsedTableRows item_types;
    if (load_table_rows(resolver_, "GDB_ItemTypes", item_types)) {
        for (const auto& row : item_types.rows) {
            const auto uuid = lookup_text(item_types.columns, row, "UUID");
            const auto name = lookup_text(item_types.columns, row, "Name");
            if (!uuid.empty() && !name.empty()) {
                item_type_names.emplace(lower(uuid), name);
            }
        }
    }

    std::vector<LayerMetadata> layers;
    bool found_feature_dataset = false;
    bool found_valid_definition = false;
    MetadataXmlInspection issue;
    int issue_priority = 0;
    for (const auto& row : items.rows) {
        const auto uuid = lookup_text(items.columns, row, "UUID");
        auto decoded = uuid.empty()
            ? std::optional<LayerMetadata>{}
            : decode_layer_metadata_by_uuid(items.columns, row, uuid);
        LayerMetadata metadata = decoded.value_or(LayerMetadata{});
        if (!decoded) {
            metadata.name = lookup_text(items.columns, row, "Name");
            metadata.path = lookup_text(items.columns, row, "Path");
            metadata.physical_name = lookup_text(items.columns, row, "PhysicalName");
            metadata.definition = lookup_text(items.columns, row, "Definition");
            metadata.definition_present = lookup_has_value(
                items.columns, row, "Definition");
            metadata.uuid = uuid;
            metadata.type = lookup_text(items.columns, row, "Type");
        }
        assign_definition_summary(metadata.definition, metadata);
        const auto type_uuid = lower(lookup_text(items.columns, row, "Type"));
        const auto item_type = item_type_names.find(type_uuid);
        const bool item_type_is_feature_dataset =
            item_type != item_type_names.end() &&
            lower(item_type->second).find("feature dataset") != std::string::npos;
        const bool is_feature_dataset = item_type_is_feature_dataset ||
            metadata.dataset_type == "esriDTFeatureDataset";
        if (is_feature_dataset) {
            found_feature_dataset = true;
            const auto definition = lookup_optional_text(
                items.columns, row, "Definition");
            const auto inspection = inspect_metadata_xml(
                definition ? std::optional<std::string_view>(*definition)
                           : std::nullopt,
                MetadataXmlKind::FeatureDataset);
            if (inspection.status == MetadataXmlStatus::Available) {
                found_valid_definition = true;
            } else {
                const auto priority = [](MetadataXmlStatus status) {
                    switch (status) {
                        case MetadataXmlStatus::Malformed: return 5;
                        case MetadataXmlStatus::UnknownStructure: return 4;
                        case MetadataXmlStatus::Missing: return 3;
                        case MetadataXmlStatus::Empty: return 2;
                        default: return 0;
                    }
                };
                if (priority(inspection.status) > issue_priority) {
                    issue = inspection;
                    issue_priority = priority(inspection.status);
                }
            }
        }
        if (metadata.catalog_path.empty() || metadata.catalog_path == "\\") continue;
        if (metadata.dataset_type.empty()) continue;
        layers.push_back(std::move(metadata));
    }

    result.groups = summarize_dataset_groups(layers);
    if (issue_priority > 0) {
        result.inspection = std::move(issue);
    } else if (found_valid_definition) {
        result.inspection = {MetadataXmlStatus::Available, {}};
    } else if (found_feature_dataset) {
        result.inspection = {MetadataXmlStatus::Missing,
                             "Feature Dataset items have no readable definitions"};
    } else {
        result.inspection = {MetadataXmlStatus::Empty,
                             "workspace contains no Feature Dataset definitions"};
    }
    return result;
}

} // namespace explorgdb
