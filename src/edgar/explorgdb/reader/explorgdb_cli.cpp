// src/edgar/explorgdb/explorgdb_cli.cpp -- CLI entry point for GDB binary exploration

#include "gdb_catalog.h"
#include "gdb_table.h"
#include "gdb_tablx.h"
#include "gdb_indexes.h"
#include "gdb_spatial_index.h"
#include "gdb_attribute_index.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void print_usage(const char* prog) {
    printf("Usage: %s <command> [args]\n\n", prog);
    printf("Commands:\n");
    printf("  explore <gdb_path>              Scan .gdb directory and show overview\n");
    printf("  dump-table <gdbtable_path>      Parse .gdbtable header + fields\n");
    printf("  dump-tablx <gdbtablx_path>      Parse .gdbtablx offset index\n");
    printf("  dump-indexes <gdbindexes_path>  Parse .gdbindexes metadata\n");
    printf("  dump-records <gdbtable_path>    Parse .gdbtable header + fields + records (needs .gdbtablx)\n");
    printf("  dump-spx <spx_path>             Parse .spx spatial index\n");
    printf("  dump-atx <atx_path>             Parse .atx attribute index\n");
    printf("  query-spx <spx_path> <xmin> <ymin> <xmax> <ymax> <grid_step_x> <grid_step_y>\n");
    printf("                                   Query .spx with bbox (FID list)\n");
    printf("  query-atx <atx_path> <op> <value>\n");
    printf("                                   Query .atx with operator (=,<,>,<=,>=,!=)\n");
    printf("  query <gdb_path> <table_id> <spatial|attr> <args...>\n");
    printf("      spatial: <xmin> <ymin> <xmax> <ymax> <grid_step_x> <grid_step_y>\n");
    printf("      attr: <index_name> <op> <value>\n");
    printf("  bench-spatial <gdb_path> <table_id>   Run spatial query benchmark\n");
}

static void cmd_explore(const std::string& gdb_path) {
    explorgdb::GdbCatalog catalog;
    if (!catalog.scan(gdb_path)) {
        std::cerr << "Failed to scan: " << gdb_path << "\n";
        return;
    }

    printf("=== GDB Directory: %s ===\n\n", gdb_path.c_str());

    // Magic
    printf("Magic file: version=%u, magic=0x%08X", catalog.magic().version, catalog.magic().magic);
    if (catalog.magic().version == 5 && catalog.magic().magic == 0xEFBEADDE) {
        printf(" (valid)\n");
    } else {
        printf(" (UNEXPECTED)\n");
    }

    // Timestamps
    if (catalog.timestamps().raw[0] != 0) {
        printf("Timestamps file: 384 bytes\n");
    }

    // File inventory
    printf("\nFile inventory (%zu files):\n", catalog.entries().size());
    printf("%-20s %10s %10s %s\n", "Extension", "Count", "Size(KB)", "IDs");

    // Group by extension
    std::vector<std::string> exts;
    std::vector<std::vector<const explorgdb::CatalogEntry*>> groups;

    for (const auto& e : catalog.entries()) {
        bool found = false;
        for (size_t i = 0; i < exts.size(); ++i) {
            if (exts[i] == e.extension) {
                groups[i].push_back(&e);
                found = true;
                break;
            }
        }
        if (!found) {
            exts.push_back(e.extension);
            groups.push_back({&e});
        }
    }

    for (size_t i = 0; i < exts.size(); ++i) {
        uint64_t total_size = 0;
        std::string ids;
        for (const auto* e : groups[i]) {
            total_size += e->file_size;
            if (!ids.empty()) ids += ",";
            ids += std::to_string(e->numeric_id);
            if (ids.size() > 60) { ids += "..."; break; }
        }
        printf("%-20s %10zu %10.1f %s\n", exts[i].c_str(), groups[i].size(),
               total_size / 1024.0, ids.c_str());
    }

    // Detailed table info
    auto tables = catalog.find_by_extension(".gdbtable");
    if (!tables.empty()) {
        printf("\n=== .gdbtable details ===\n");
        for (const auto* te : tables) {
            std::string table_path = (fs::path(gdb_path) / te->filename).string();
            explorgdb::GdbTableParser parser(table_path);
            if (parser.parse_header()) {
                printf("\n  File: %s (id=%u, size=%lu bytes)\n",
                       te->filename.c_str(), te->numeric_id, static_cast<unsigned long long>(te->file_size));
                const auto& hdr = parser.header();
                printf("  Version: %u\n", hdr.version);
                if (hdr.version == 3) {
                    printf("  Features: %u\n", hdr.nfeatures_v3);
                } else {
                    printf("  Features: %lu (has_deleted=%u)\n",
                           hdr.nfeatures_v4, hdr.has_deleted_features);
                }
                printf("  Largest record: %u bytes\n", hdr.largest_size_record);
                printf("  Field section offset: %lu\n", hdr.field_desc_offset);

                if (parser.parse_fields()) {
                    printf("  Fields (%zu):\n", parser.fields().size());
                    for (const auto& fd : parser.fields()) {
                        printf("    %s (%s)", fd.name.c_str(),
                               explorgdb::field_type_name(fd.type));
                        if (fd.type == explorgdb::FieldType::Geometry) {
                            printf(" WKT: %s", fd.wkt.c_str());
                        }
                        printf("\n");
                    }
                }
            }
        }
    }

    // Tablx info
    auto tablxes = catalog.find_by_extension(".gdbtablx");
    if (!tablxes.empty()) {
        printf("\n=== .gdbtablx details ===\n");
        for (const auto* te : tablxes) {
            std::string tablx_path = (fs::path(gdb_path) / te->filename).string();
            explorgdb::GdbTablxParser parser(tablx_path);
            if (parser.parse()) {
                printf("\n  File: %s (id=%u, size=%lu bytes)\n",
                       te->filename.c_str(), te->numeric_id, static_cast<unsigned long long>(te->file_size));
                const auto& hdr = parser.header();
                printf("  Version: %u, offset_size: %u bytes\n", hdr.version, hdr.size_tablx_offsets);
                printf("  Entries: %zu, Valid FIDs: %zu\n",
                       parser.offsets().size(), parser.valid_fids().size());
                if (!parser.block_bitmap().empty()) {
                    int active = 0;
                    for (bool b : parser.block_bitmap()) if (b) active++;
                    printf("  Sparse bitmap: %d/%d blocks active\n",
                           active, static_cast<int>(parser.block_bitmap().size()));
                }
            }
        }
    }

    // Index info
    auto indexes = catalog.find_by_extension(".gdbindexes");
    if (!indexes.empty()) {
        printf("\n=== .gdbindexes details ===\n");
        for (const auto* ie : indexes) {
            std::string idx_path = (fs::path(gdb_path) / ie->filename).string();
            explorgdb::GdbIndexesParser parser(idx_path);
            if (parser.parse()) {
                printf("\n  File: %s (id=%u, size=%lu bytes)\n",
                       ie->filename.c_str(), ie->numeric_id, ie->file_size);
                printf("  Indexes (%zu):\n", parser.index_count());
                for (const auto& entry : parser.entries()) {
                    printf("    name=%s, col=%s, magic=(%u,%d,%u)\n",
                           entry.name.c_str(), entry.column_name.c_str(),
                           entry.magic1, entry.magic2, entry.magic3);
                }
            }
        }
    }

    // Other file types
    auto spx_files = catalog.find_by_extension(".spx");
    if (!spx_files.empty()) {
        printf("\n=== .spx spatial index files ===\n");
        for (const auto* se : spx_files) {
            std::string spx_path = (fs::path(gdb_path) / se->filename).string();
            explorgdb::GdbSpatialIndexParser parser(spx_path);
            if (parser.parse()) {
                const auto& tr = parser.trailer();
                printf("  %s (id=%u, %lu bytes) depth=%u entries=%u\n",
                       se->filename.c_str(), se->numeric_id, se->file_size,
                       tr.tree_depth, tr.total_value_count);
            } else {
                printf("  %s (%lu bytes) [parse failed]\n", se->filename.c_str(), se->file_size);
            }
        }
    }

    auto atx_files = catalog.find_by_extension(".atx");
    if (!atx_files.empty()) {
        printf("\n=== .atx attribute index files ===\n");
        for (const auto* ae : atx_files) {
            std::string atx_path = (fs::path(gdb_path) / ae->filename).string();
            explorgdb::GdbAttributeIndexParser parser(atx_path);
            if (parser.parse()) {
                const auto& tr = parser.trailer();
                const char* type_str = tr.is_string ? "string" : "numeric";
                printf("  %s (id=%u, %lu bytes) type=%s depth=%u entries=%u value_size=%u\n",
                       ae->filename.c_str(), ae->numeric_id, ae->file_size,
                       type_str, tr.tree_depth, tr.total_value_count, tr.value_size);
            } else {
                printf("  %s (%lu bytes) [parse failed]\n", ae->filename.c_str(), ae->file_size);
            }
        }
    }
}

static void cmd_dump_table(const std::string& path) {
    explorgdb::GdbTableParser parser(path);
    if (!parser.load_file()) {
        std::cerr << "Failed to load: " << path << "\n";
        return;
    }
    if (!parser.parse_header()) {
        std::cerr << "Failed to parse header\n";
        return;
    }

    const auto& hdr = parser.header();
    printf("=== .gdbtable: %s ===\n", path.c_str());
    printf("Version: %u\n", hdr.version);
    if (hdr.version == 3) {
        printf("Features: %u\n", hdr.nfeatures_v3);
    } else {
        printf("Features: %lu\n", hdr.nfeatures_v4);
    }
    printf("Largest record: %u bytes\n", hdr.largest_size_record);
    printf("File size: %lu bytes\n", hdr.file_size);
    printf("Field section offset: %lu\n", hdr.field_desc_offset);

    if (!parser.parse_fields()) {
        std::cerr << "Failed to parse fields\n";
        return;
    }

    printf("\nFields (%zu):\n", parser.fields().size());
    for (size_t i = 0; i < parser.fields().size(); ++i) {
        const auto& fd = parser.fields()[i];
        printf("  [%zu] %s (%s) alias=\"%s\" width=%u flag=0x%02x",
               i, fd.name.c_str(), explorgdb::field_type_name(fd.type),
               fd.alias.c_str(), fd.width, fd.flag);
        if (fd.type == explorgdb::FieldType::Geometry) {
            printf(" WKT=%s", fd.wkt.c_str());
            printf(" bbox=[%.6f,%.6f,%.6f,%.6f]", fd.xmin, fd.ymin, fd.xmax, fd.ymax);
            printf(" scale=%.1f", fd.xyscale);
        }
        printf("\n");
    }
}

static void cmd_dump_tablx(const std::string& path) {
    explorgdb::GdbTablxParser parser(path);
    if (!parser.parse()) {
        std::cerr << "Failed to parse: " << path << "\n";
        return;
    }

    const auto& hdr = parser.header();
    printf("=== .gdbtablx: %s ===\n", path.c_str());
    printf("Version: %u, offset_size: %u bytes\n", hdr.version, hdr.size_tablx_offsets);
    printf("Total entries: %zu\n", parser.offsets().size());
    printf("Valid FIDs: %zu\n", parser.valid_fids().size());

    if (!parser.block_bitmap().empty()) {
        int active = 0;
        for (bool b : parser.block_bitmap()) if (b) active++;
        printf("Sparse bitmap: %d/%d blocks active\n",
               active, static_cast<int>(parser.block_bitmap().size()));
    }

    // Show first 10 offsets
    printf("\nFirst 10 offsets:\n");
    for (size_t i = 0; i < std::min(parser.offsets().size(), (size_t)10); ++i) {
        printf("  FID %zu -> offset %lu\n", i, parser.offsets()[i]);
    }
}

static void cmd_dump_indexes(const std::string& path) {
    explorgdb::GdbIndexesParser parser(path);
    if (!parser.parse()) {
        std::cerr << "Failed to parse: " << path << "\n";
        return;
    }

    printf("=== .gdbindexes: %s ===\n", path.c_str());
    printf("Index count: %zu\n", parser.index_count());

    for (size_t i = 0; i < parser.entries().size(); ++i) {
        const auto& e = parser.entries()[i];
        printf("  [%zu] name=%s col=%s magic=(%u,%d,%u,%d) magic5=%u\n",
               i, e.name.c_str(), e.column_name.c_str(),
               e.magic1, e.magic2, e.magic3, e.magic4, e.magic5);
    }
}

static void cmd_dump_records(const std::string& table_path) {
    explorgdb::GdbTableParser parser(table_path);
    if (!parser.load_file()) {
        std::cerr << "Failed to load: " << table_path << "\n";
        return;
    }
    if (!parser.parse_header()) {
        std::cerr << "Failed to parse header\n";
        return;
    }
    if (!parser.parse_fields()) {
        std::cerr << "Failed to parse fields\n";
        return;
    }

    // Try to load companion .gdbtablx
    std::string tablx_path = table_path;
    size_t dot_pos = tablx_path.rfind(".gdbtable");
    if (dot_pos != std::string::npos) {
        tablx_path = tablx_path.substr(0, dot_pos) + ".gdbtablx";
    } else {
        std::cerr << "Cannot find companion .gdbtablx for: " << table_path << "\n";
        return;
    }

    if (!parser.load_tablx(tablx_path)) {
        std::cerr << "Failed to load .gdbtablx: " << tablx_path << "\n";
        return;
    }

    if (!parser.parse_records()) {
        std::cerr << "Failed to parse records\n";
        return;
    }

    printf("=== Records: %s ===\n", table_path.c_str());
    printf("Total records: %zu\n\n", parser.records().size());

    // Print first 5 records
    size_t limit = std::min(parser.records().size(), (size_t)5);
    for (size_t i = 0; i < limit; ++i) {
        const auto& rec = parser.records()[i];
        printf("FID %u (blob_len=%u):\n", rec.fid, rec.blob_len);
        for (size_t j = 0; j < rec.field_values.size() && j < parser.fields().size(); ++j) {
            const auto& fd = parser.fields()[j];
            const auto& val = rec.field_values[j];
            printf("  %s: ", fd.name.c_str());
            if (std::holds_alternative<std::nullptr_t>(val)) {
                printf("NULL");
            } else if (std::holds_alternative<int32_t>(val)) {
                printf("%d", std::get<int32_t>(val));
            } else if (std::holds_alternative<int64_t>(val)) {
                printf("%ld", std::get<int64_t>(val));
            } else if (std::holds_alternative<double>(val)) {
                printf("%.6f", std::get<double>(val));
            } else if (std::holds_alternative<explorgdb::DateTimeOffsetValue>(val)) {
                const auto value = std::get<explorgdb::DateTimeOffsetValue>(val);
                printf("%.6f (UTC%+d min)", value.date, value.offset_minutes);
            } else if (std::holds_alternative<std::string>(val)) {
                const std::string& s = std::get<std::string>(val);
                if (s.size() > 80) {
                    printf("\"%s...\"", s.substr(0, 80).c_str());
                } else {
                    printf("\"%s\"", s.c_str());
                }
            } else if (std::holds_alternative<std::vector<uint8_t>>(val)) {
                printf("<blob %zu bytes>", std::get<std::vector<uint8_t>>(val).size());
            }
            printf("\n");
        }
        printf("\n");
    }

    if (parser.records().size() > limit) {
        printf("... and %zu more records\n", parser.records().size() - limit);
    }
}

static void cmd_dump_spx(const std::string& path) {
    explorgdb::GdbSpatialIndexParser parser(path);
    if (!parser.parse()) {
        std::cerr << "Failed to parse: " << path << "\n";
        return;
    }

    const auto& tr = parser.trailer();
    printf("=== .spx: %s ===\n", path.c_str());
    printf("Trailer: value_size=%u, depth=%u, total_entries=%u\n",
           tr.value_size, tr.tree_depth, tr.total_value_count);
    printf("String=%d, Numeric=%d\n", tr.is_string, tr.is_numeric);
    auto get_file_size = [&](const std::string& p) -> size_t {
        FILE* f = fopen(p.c_str(), "rb");
        if (!f) return 0;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        return static_cast<size_t>(sz);
    };
    printf("File size: %zu bytes, ~%zu pages\n\n",
           get_file_size(path), get_file_size(path) / explorgdb::GdbSpatialIndexParser::kPageSize);

    printf("(B+ tree on-demand mode — entries not pre-loaded. Use 'query-spx' to retrieve FIDs.)\n");
}

static void cmd_dump_atx(const std::string& path) {
    explorgdb::GdbAttributeIndexParser parser(path);
    if (!parser.parse()) {
        std::cerr << "Failed to parse: " << path << "\n";
        return;
    }

    const auto& tr = parser.trailer();
    const char* type_str = tr.is_string ? "string" : "numeric";
    printf("=== .atx: %s ===\n", path.c_str());
    printf("Trailer: value_size=%u, type=%s, depth=%u, total_entries=%u\n",
           tr.value_size, type_str, tr.tree_depth, tr.total_value_count);
    printf("Parsed entries: %zu\n\n", parser.all_entries().size());

    // Show first 50 entries
    size_t limit = std::min(parser.all_entries().size(), (size_t)50);
    for (size_t i = 0; i < limit; ++i) {
        const auto& e = parser.all_entries()[i];
        printf("  FID=%u  ", e.fid);
        if (std::isnan(e.numeric_value)) {
            printf("value=\"%s\"", e.string_value.c_str());
        } else {
            printf("value=%.15g", e.numeric_value);
        }
        printf("\n");
    }
    if (parser.all_entries().size() > limit) {
        printf("  ... and %zu more\n", parser.all_entries().size() - limit);
    }
}

static void cmd_query_spx(const std::string& path, double xmin, double ymin,
                          double xmax, double ymax,
                          double grid_step_x, double grid_step_y) {
    explorgdb::GdbSpatialIndexParser parser(path);
    if (!parser.parse()) {
        std::cerr << "Failed to parse: " << path << "\n";
        return;
    }

    // Use query_bbox for the spatial query
    std::vector<double> grid_resolutions;
    if (grid_step_x > 0.0) {
        grid_resolutions = {grid_step_x, grid_step_x * 4.0, grid_step_x * 16.0};
    } else {
        grid_resolutions = {180000.0};
    }
    auto result_fids = parser.query_bbox(xmin, ymin, xmax, ymax, 0.0, 0.0, grid_step_x, grid_resolutions);

    printf("=== Spatial Query: %s ===\n", path.c_str());
    printf("Bbox: [%.2f, %.2f, %.2f, %.2f]\n", xmin, ymin, xmax, ymax);
    printf("Matched FIDs: %zu\n", result_fids.size());

    // Show first 20
    for (size_t i = 0; i < std::min(result_fids.size(), (size_t)20); ++i) {
        printf("  FID=%u\n", result_fids[i]);
    }
    if (result_fids.size() > 20) {
        printf("  ... and %zu more\n", result_fids.size() - 20);
    }
}

static void cmd_query_atx(const std::string& path, const std::string& op_str,
                          const std::string& value_str) {
    explorgdb::GdbAttributeIndexParser parser(path);
    if (!parser.parse()) {
        std::cerr << "Failed to parse: " << path << "\n";
        return;
    }

    const auto& tr = parser.trailer();
    std::vector<uint32_t> result_fids;

    if (tr.is_string) {
        auto op_map = [&](const std::string& s) {
            if (s == "=") return explorgdb::AttrOp::Eq;
            if (s == "!=") return explorgdb::AttrOp::Ne;
            if (s == "<") return explorgdb::AttrOp::Lt;
            if (s == "<=") return explorgdb::AttrOp::Le;
            if (s == ">") return explorgdb::AttrOp::Gt;
            if (s == ">=") return explorgdb::AttrOp::Ge;
            return explorgdb::AttrOp::Eq;
        };
        result_fids = parser.query_string(value_str, op_map(op_str));
    } else {
        double value = std::stod(value_str);
        auto op_map = [&](const std::string& s) {
            if (s == "=") return explorgdb::AttrOp::Eq;
            if (s == "!=") return explorgdb::AttrOp::Ne;
            if (s == "<") return explorgdb::AttrOp::Lt;
            if (s == "<=") return explorgdb::AttrOp::Le;
            if (s == ">") return explorgdb::AttrOp::Gt;
            if (s == ">=") return explorgdb::AttrOp::Ge;
            return explorgdb::AttrOp::Eq;
        };
        result_fids = parser.query_double(value, op_map(op_str));
    }

    printf("=== Attribute Query: %s ===\n", path.c_str());
    printf("Operator: %s, Value: %s\n", op_str.c_str(), value_str.c_str());
    printf("Matched FIDs: %zu\n", result_fids.size());

    for (size_t i = 0; i < std::min(result_fids.size(), (size_t)20); ++i) {
        printf("  FID=%u\n", result_fids[i]);
    }
    if (result_fids.size() > 20) {
        printf("  ... and %zu more\n", result_fids.size() - 20);
    }
}

// 辅助：打印单条记录
static void print_record(const explorgdb::FeatureRecord& rec,
                         const std::vector<explorgdb::FieldDescriptor>& fields) {
    printf("FID %u (blob_len=%u):\n", rec.fid, rec.blob_len);
    for (size_t j = 0; j < rec.field_values.size() && j < fields.size(); ++j) {
        const auto& fd = fields[j];
        const auto& val = rec.field_values[j];
        printf("  %s: ", fd.name.c_str());
        if (std::holds_alternative<std::nullptr_t>(val)) {
            printf("NULL");
        } else if (std::holds_alternative<int32_t>(val)) {
            printf("%d", std::get<int32_t>(val));
        } else if (std::holds_alternative<int64_t>(val)) {
            printf("%ld", std::get<int64_t>(val));
        } else if (std::holds_alternative<double>(val)) {
            printf("%.6f", std::get<double>(val));
        } else if (std::holds_alternative<explorgdb::DateTimeOffsetValue>(val)) {
            const auto value = std::get<explorgdb::DateTimeOffsetValue>(val);
            printf("%.6f (UTC%+d min)", value.date, value.offset_minutes);
        } else if (std::holds_alternative<std::string>(val)) {
            const std::string& s = std::get<std::string>(val);
            if (s.size() > 80) {
                printf("\"%s...\"", s.substr(0, 80).c_str());
            } else {
                printf("\"%s\"", s.c_str());
            }
        } else if (std::holds_alternative<std::vector<uint8_t>>(val)) {
            printf("<blob %zu bytes>", std::get<std::vector<uint8_t>>(val).size());
        }
        printf("\n");
    }
}

static void cmd_query(const std::string& gdb_path, uint32_t table_id,
                      const std::string& query_type,
                      const std::vector<std::string>& args) {
    namespace fs = std::filesystem;
    using namespace explorgdb;

    // 1. 扫描目录
    GdbCatalog catalog;
    if (!catalog.scan(gdb_path)) {
        std::cerr << "Failed to scan: " << gdb_path << "\n";
        return;
    }

    // 2. 加载 .gdbtable
    const auto* table_entry = catalog.find_table(table_id);
    if (!table_entry) {
        std::cerr << "Table id=" << table_id << " not found\n";
        return;
    }
    std::string table_path = (fs::path(gdb_path) / table_entry->filename).string();
    GdbTableParser table_parser(table_path);
    if (!table_parser.load_file() || !table_parser.parse_header() || !table_parser.parse_fields()) {
        std::cerr << "Failed to parse table: " << table_path << "\n";
        return;
    }

    // 3. 加载 .gdbtablx
    const auto* tablx_entry = catalog.find_tablx(table_id);
    if (!tablx_entry) {
        std::cerr << "No .gdbtablx for table id=" << table_id << "\n";
        return;
    }
    std::string tablx_path = (fs::path(gdb_path) / tablx_entry->filename).string();
    if (!table_parser.load_tablx(tablx_path)) {
        std::cerr << "Failed to load .gdbtablx: " << tablx_path << "\n";
        return;
    }
    (void)table_parser.nullable_field_count();  // suppress unused warning

    // 4. 索引查询 → FID 列表
    std::vector<uint32_t> result_fids;

    if (query_type == "spatial") {
        if (args.size() < 6) {
            std::cerr << "Need 6 args: xmin ymin xmax ymax grid_step_x grid_step_y\n";
            return;
        }
        double xmin = std::stod(args[0]);
        double ymin = std::stod(args[1]);
        double xmax = std::stod(args[2]);
        double ymax = std::stod(args[3]);
        double gsx = std::stod(args[4]);
        double gsy = std::stod(args[5]);

        const auto* spx_entry = catalog.find_spx(table_id);
        if (!spx_entry) {
            std::cerr << "No .spx for table id=" << table_id << "\n";
            return;
        }
        std::string spx_file = (fs::path(gdb_path) / spx_entry->filename).string();
        GdbSpatialIndexParser spx_parser(spx_file);
        if (!spx_parser.parse()) {
            std::cerr << "Failed to parse .spx: " << spx_file << "\n";
            return;
        }

        // Use query_bbox instead of iterating all_entries
        std::vector<double> grid_resolutions = {gsx, gsx * 4.0, gsx * 16.0};
        result_fids = spx_parser.query_bbox(xmin, ymin, xmax, ymax, 0.0, 0.0, gsx, grid_resolutions);

    } else if (query_type == "attr") {
        if (args.size() < 3) {
            std::cerr << "Need 3 args: index_name op value\n";
            return;
        }
        std::string index_name = args[0];
        std::string op_str = args[1];
        std::string value_str = args[2];

        auto op_map = [&](const std::string& s) {
            if (s == "=") return AttrOp::Eq;
            if (s == "!=") return AttrOp::Ne;
            if (s == "<") return AttrOp::Lt;
            if (s == "<=") return AttrOp::Le;
            if (s == ">") return AttrOp::Gt;
            if (s == ">=") return AttrOp::Ge;
            return AttrOp::Eq;
        };

        // 先尝试按 table_id + index_name 查找
        const auto* atx_entry = catalog.find_atx(table_id, index_name);
        // 如果找不到，尝试 find_all_atx 中匹配名称的
        if (!atx_entry) {
            for (const auto* ae : catalog.find_all_atx(table_id)) {
                std::string fn = ae->filename;
                // 检查文件名是否包含 index_name
                if (fn.find(index_name) != std::string::npos &&
                    fn.size() > 4 && fn.substr(fn.size() - 4) == ".atx") {
                    atx_entry = ae;
                    break;
                }
            }
        }
        if (!atx_entry) {
            std::cerr << "No .atx for table id=" << table_id
                      << " index=" << index_name << "\n";
            return;
        }

        std::string atx_file = (fs::path(gdb_path) / atx_entry->filename).string();
        GdbAttributeIndexParser atx_parser(atx_file);
        if (!atx_parser.parse()) {
            std::cerr << "Failed to parse .atx: " << atx_file << "\n";
            return;
        }

        const auto& tr = atx_parser.trailer();
        if (tr.is_string) {
            result_fids = atx_parser.query_string(value_str, op_map(op_str));
        } else {
            double value = std::stod(value_str);
            result_fids = atx_parser.query_double(value, op_map(op_str));
        }
    } else {
        std::cerr << "Unknown query type: " << query_type << "\n";
        return;
    }

    // 5. 打印结果
    printf("=== Query Result: table_id=%u, type=%s ===\n", table_id, query_type.c_str());
    printf("Matched FIDs: %zu\n\n", result_fids.size());

    for (uint32_t fid : result_fids) {
        FeatureRecord rec;
        if (table_parser.read_record_by_fid(fid, rec)) {
            print_record(rec, table_parser.fields());
            printf("\n");
        } else {
            printf("FID %u: failed to read\n\n", fid);
        }
    }
}

// ── bench-spatial: 空间查询性能基准测试 ──
// 对比 explorgdb 线性扫描与 GDAL 组件 B+ 树裁剪的性能
// 用法: bench-spatial <gdb_path> <table_id>

struct BenchCase {
    const char* name;
    double xmin, ymin, xmax, ymax;
};

static void cmd_bench_spatial(const std::string& gdb_path, uint32_t table_id) {
    namespace fs = std::filesystem;
    using namespace explorgdb;

    // 扫描目录
    GdbCatalog catalog;
    if (!catalog.scan(gdb_path)) { std::cerr << "Failed to scan: " << gdb_path << "\n"; return; }

    // 加载 .gdbtable
    const auto* table_entry = catalog.find_table(table_id);
    if (!table_entry) { std::cerr << "Table id=" << table_id << " not found\n"; return; }
    std::string table_path = (fs::path(gdb_path) / table_entry->filename).string();
    GdbTableParser table_parser(table_path);
    if (!table_parser.load_file() || !table_parser.parse_header() || !table_parser.parse_fields()) {
        std::cerr << "Failed to parse table: " << table_path << "\n"; return;
    }

    // 加载 .gdbtablx
    const auto* tablx_entry = catalog.find_tablx(table_id);
    if (!tablx_entry) { std::cerr << "No .gdbtablx for table id=" << table_id << "\n"; return; }
    std::string tablx_path = (fs::path(gdb_path) / tablx_entry->filename).string();
    if (!table_parser.load_tablx(tablx_path)) { std::cerr << "Failed to load .gdbtablx\n"; return; }

    // 加载 .spx
    const auto* spx_entry = catalog.find_spx(table_id);
    if (!spx_entry) { std::cerr << "No .spx for table id=" << table_id << "\n"; return; }
    std::string spx_file = (fs::path(gdb_path) / spx_entry->filename).string();
    GdbSpatialIndexParser spx_parser(spx_file);
    if (!spx_parser.parse()) { std::cerr << "Failed to parse .spx\n"; return; }

    size_t n_spx = spx_parser.trailer().total_value_count;
    uint32_t n_features = table_parser.header().version == 3
                              ? table_parser.header().nfeatures_v3
                              : static_cast<uint32_t>(table_parser.header().nfeatures_v4);

    // 提取几何字段参数
    double xorig = 0, yorig = 0;
    std::vector<double> grid_resolutions;
    for (const auto& fd : table_parser.fields()) {
        if (fd.type == FieldType::Geometry) {
            xorig = fd.xorig;
            yorig = fd.yorig;
            grid_resolutions = fd.grid_sizes;
            break;
        }
    }

    const BenchCase cases[] = {
        {"Point (~13)",     1200000, 3100000, 1300000, 3200000},
        {"Local (~45)",     1150000, 3050000, 1350000, 3300000},
        {"Regional (~580)",  800000, 2500000, 1600000, 3800000},
        {"Large (~1800)",      0, 2000000, 2000000, 5000000},
    };

    printf("\n=== Spatial Query Benchmark (explorgdb) ===\n");
    printf("Table: %s (id=%u, %u features, %zu spx entries)\n\n",
           table_entry->filename.c_str(), table_id, n_features, n_spx);

    printf("%-20s %8s %12s %12s %10s %10s\n",
           "Scenario", "Results", "Total(ms)", "Query(ms)", "Scan(ms)", "Fetch(ms)");
    printf("%-20s %8s %12s %12s %10s %10s\n",
           "--------------------", "--------", "------------", "------------", "----------", "----------");

    for (const auto& c : cases) {
        auto t0 = std::chrono::high_resolution_clock::now();

        // B+ 树导航空间查询
        std::vector<uint32_t> result_fids = spx_parser.query_bbox(
            c.xmin, c.ymin, c.xmax, c.ymax, xorig, yorig, grid_resolutions[0], grid_resolutions);

        auto t1 = std::chrono::high_resolution_clock::now();
        auto t2 = t1;  // scan_ms ≈ query_ms for B+ tree mode

        // 获取记录
        for (uint32_t fid : result_fids) {
            FeatureRecord rec;
            table_parser.read_record_by_fid(fid, rec);
        }
        auto t3 = std::chrono::high_resolution_clock::now();

        double query_ms = std::chrono::duration<double, std::milli>(t2 - t0).count();
        double scan_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double fetch_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
        double total_ms = std::chrono::duration<double, std::milli>(t3 - t0).count();

        printf("%-20s %8zu %12.1f %12.1f %10.1f %10.1f\n",
               c.name, result_fids.size(), total_ms, query_ms, scan_ms, fetch_ms);
    }

    printf("\n注: 使用二分查找 + raw_value 排序优化，跳过不匹配条目\n");
    printf("    Query(ms) = 纯索引查询时间（含 lower_bound + 范围内扫描）\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "explore") {
        if (argc < 3) { std::cerr << "Usage: explore <gdb_path>\n"; return 1; }
        cmd_explore(argv[2]);
    } else if (cmd == "dump-table") {
        if (argc < 3) { std::cerr << "Usage: dump-table <gdbtable_path>\n"; return 1; }
        cmd_dump_table(argv[2]);
    } else if (cmd == "dump-tablx") {
        if (argc < 3) { std::cerr << "Usage: dump-tablx <gdbtablx_path>\n"; return 1; }
        cmd_dump_tablx(argv[2]);
    } else if (cmd == "dump-indexes") {
        if (argc < 3) { std::cerr << "Usage: dump-indexes <gdbindexes_path>\n"; return 1; }
        cmd_dump_indexes(argv[2]);
    } else if (cmd == "dump-records") {
        if (argc < 3) { std::cerr << "Usage: dump-records <gdbtable_path>\n"; return 1; }
        cmd_dump_records(argv[2]);
    } else if (cmd == "dump-spx") {
        if (argc < 3) { std::cerr << "Usage: dump-spx <spx_path>\n"; return 1; }
        cmd_dump_spx(argv[2]);
    } else if (cmd == "query-spx") {
        if (argc < 9) { std::cerr << "Usage: query-spx <spx_path> <xmin> <ymin> <xmax> <ymax> <grid_step_x> <grid_step_y>\n"; return 1; }
        cmd_query_spx(argv[2], std::stod(argv[3]), std::stod(argv[4]),
                      std::stod(argv[5]), std::stod(argv[6]),
                      std::stod(argv[7]), std::stod(argv[8]));
    } else if (cmd == "query-atx") {
        if (argc < 5) { std::cerr << "Usage: query-atx <atx_path> <op> <value>\n"; return 1; }
        cmd_query_atx(argv[2], argv[3], argv[4]);
    } else if (cmd == "query") {
        if (argc < 5) { std::cerr << "Usage: query <gdb_path> <table_id> <spatial|attr> <args...>\n"; return 1; }
        std::vector<std::string> args;
        for (int i = 5; i < argc; ++i) args.push_back(argv[i]);
        cmd_query(argv[2], static_cast<uint32_t>(std::stoul(argv[3])), argv[4], args);
    } else if (cmd == "dump-atx") {
        if (argc < 3) { std::cerr << "Usage: dump-atx <atx_path>\n"; return 1; }
        cmd_dump_atx(argv[2]);
    } else if (cmd == "bench-spatial") {
        if (argc < 4) { std::cerr << "Usage: bench-spatial <gdb_path> <table_id>\n"; return 1; }
        cmd_bench_spatial(argv[2], static_cast<uint32_t>(std::stoul(argv[3])));
    } else {
        std::cerr << "Unknown command: " << cmd << "\n";
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}
