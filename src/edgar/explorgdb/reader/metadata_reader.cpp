#include "metadata_reader.h"
#include "gdb_table.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace explorgdb {
namespace {
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
} // namespace

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

} // namespace explorgdb
