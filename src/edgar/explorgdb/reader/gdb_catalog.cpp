// src/edgar/explorgdb/gdb_catalog.cpp
// 目录扫描实现 — 使用 std::filesystem 枚举文件，正则匹配 aXXXXXXXX.<ext> 格式

#include "gdb_catalog.h"
#include "binary_reader.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>

namespace fs = std::filesystem;

namespace explorgdb {

bool GdbCatalog::scan(const std::string& gdb_path) {
    gdb_path_ = gdb_path;
    entries_.clear();
    has_magic_ = false;
    has_timestamps_ = false;

    if (!fs::exists(gdb_path) || !fs::is_directory(gdb_path)) {
        std::cerr << "Error: " << gdb_path << " is not a directory\n";
        return false;
    }

    static const std::regex hex_file_re("^a([0-9a-fA-F]{8})\\.(.+)$");

    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        if (!entry.is_regular_file()) continue;
        const std::string fname = entry.path().filename().string();

        if (fname == "gdb") {
            read_magic();
            continue;
        }
        if (fname == "timestamps") {
            read_timestamps();
            continue;
        }

        std::smatch match;
        if (std::regex_match(fname, match, hex_file_re)) {
            CatalogEntry catalog_entry;
            catalog_entry.filename = fname;
            catalog_entry.numeric_id = static_cast<uint32_t>(
                std::stoul(match[1].str(), nullptr, 16));
            catalog_entry.extension = "." + match[2].str();
            catalog_entry.file_size = entry.file_size();
            entries_.push_back(std::move(catalog_entry));
        }
    }

    std::sort(entries_.begin(), entries_.end(),
              [](const CatalogEntry& lhs, const CatalogEntry& rhs) {
                  return lhs.numeric_id < rhs.numeric_id;
              });
    return true;
}

bool GdbCatalog::read_magic() {
    // std::filesystem::path::value_type is wchar_t on Windows, so make the
    // conversion explicit instead of relying on an unavailable implicit
    // path -> std::string conversion.
    const std::string magic_file =
        (fs::path(gdb_path_) / "gdb").string();
    std::ifstream input(magic_file, std::ios::binary);
    if (!input.is_open()) return false;

    std::vector<uint8_t> buffer(8);
    input.read(reinterpret_cast<char*>(buffer.data()), 8);
    if (input.gcount() != 8) return false;

    BinaryReader reader(buffer);
    magic_.version = reader.read_u32();
    magic_.magic = reader.read_u32();
    has_magic_ = true;
    return true;
}

bool GdbCatalog::read_timestamps() {
    const std::string timestamps_file =
        (fs::path(gdb_path_) / "timestamps").string();
    std::ifstream input(timestamps_file, std::ios::binary);
    if (!input.is_open()) return false;

    std::vector<uint8_t> buffer(384);
    input.read(reinterpret_cast<char*>(buffer.data()), 384);
    if (input.gcount() != 384) return false;

    std::memcpy(timestamps_.raw, buffer.data(), 384);
    has_timestamps_ = true;
    return true;
}

std::vector<const CatalogEntry*> GdbCatalog::find_by_extension(
    const std::string& extension) const {
    std::vector<const CatalogEntry*> result;
    for (const auto& entry : entries_) {
        if (entry.extension == extension) result.push_back(&entry);
    }
    return result;
}

const CatalogEntry* GdbCatalog::find_table(uint32_t id) const {
    for (const auto& entry : entries_) {
        if (entry.numeric_id == id && entry.extension == ".gdbtable")
            return &entry;
    }
    return nullptr;
}

const CatalogEntry* GdbCatalog::find_tablx(uint32_t id) const {
    for (const auto& entry : entries_) {
        if (entry.numeric_id == id && entry.extension == ".gdbtablx")
            return &entry;
    }
    return nullptr;
}

const CatalogEntry* GdbCatalog::find_spx(uint32_t id) const {
    for (const auto& entry : entries_) {
        if (entry.numeric_id == id && entry.extension == ".spx")
            return &entry;
    }
    return nullptr;
}

const CatalogEntry* GdbCatalog::find_atx(
    uint32_t id, const std::string& index_name) const {
    const std::string target_extension = "." + index_name + ".atx";
    for (const auto& entry : entries_) {
        if (entry.numeric_id == id &&
            entry.extension == target_extension)
            return &entry;
    }
    return nullptr;
}

std::vector<const CatalogEntry*> GdbCatalog::find_all_atx(uint32_t id) const {
    std::vector<const CatalogEntry*> result;
    const std::string suffix = ".atx";
    for (const auto& entry : entries_) {
        if (entry.numeric_id == id &&
            entry.extension.size() > suffix.size() &&
            entry.extension.substr(entry.extension.size() - suffix.size()) ==
                suffix)
            result.push_back(&entry);
    }
    return result;
}

} // namespace explorgdb
