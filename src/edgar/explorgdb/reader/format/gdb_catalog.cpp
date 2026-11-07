// src/edgar/explorgdb/gdb_catalog.cpp
// 目录扫描实现 — 使用 std::filesystem 枚举文件，正则匹配 aXXXXXXXX.<ext> 格式

#include "gdb_catalog.h"
#include "binary_reader.h"
#include "gdb_indexes.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <utility>

namespace fs = std::filesystem;

namespace explorgdb {

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbCatalog::scan(const std::string& gdb_path) {
    const fs::path directory = fs::u8path(gdb_path);
    gdb_path_ = gdb_path;
    entries_.clear();
    index_metadata_cache_ = std::make_shared<IndexMetadataCacheState>();
    has_magic_ = false;
    has_timestamps_ = false;
    magic_ = GdbDirectoryHeader{};
    timestamps_ = GdbTimestamps{};

    if (!fs::exists(directory) || !fs::is_directory(directory)) {
        std::cerr << "Error: " << gdb_path << " is not a directory\n";
        return false;
    }

    static const std::regex hex_file_re("^a([0-9a-fA-F]{8})\\.(.+)$");

    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        const std::string fname = entry.path().filename().u8string();

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

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbCatalog::read_magic() {
    const fs::path magic_file = fs::u8path(gdb_path_) / "gdb";
    std::ifstream input(magic_file, std::ios::binary);
    if (!input.is_open()) return false;

    std::vector<uint8_t> buffer(8);
    input.read(reinterpret_cast<char*>(buffer.data()), 8);
    const std::streamsize bytes_read = input.gcount();
    if (bytes_read != 4 && bytes_read != 8) return false;
    buffer.resize(static_cast<size_t>(bytes_read));

    BinaryReader reader(buffer);
    magic_.version = reader.read_u32();
    magic_.magic = bytes_read == 8 ? reader.read_u32() : 0U;
    has_magic_ = true;
    return true;
}

// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbCatalog::read_timestamps() {
    const fs::path timestamps_file = fs::u8path(gdb_path_) / "timestamps";
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

const CatalogEntry* GdbCatalog::find_indexes(uint32_t id) const {
    for (const auto& entry : entries_) {
        if (entry.numeric_id == id && entry.extension == ".gdbindexes")
            return &entry;
    }
    return nullptr;
}

bool GdbCatalog::read_index_metadata(
    uint32_t id,
    std::vector<IndexEntry>& output) const {
    output.clear();
    const std::shared_ptr<IndexMetadataCacheState> cache =
        index_metadata_cache_;
    {
        std::shared_lock<std::shared_mutex> lock(cache->mutex);
        const auto found = cache->entries.find(id);
        if (found != cache->entries.end()) {
            if (found->second.parsed) output = found->second.entries;
            return found->second.parsed;
        }
    }

    // Metadata files are small. Serialize the first parse per catalog snapshot
    // so concurrent QueryEngine instances cannot duplicate UTF-16 decoding or
    // publish conflicting success/failure entries.
    std::unique_lock<std::shared_mutex> lock(cache->mutex);
    const auto existing = cache->entries.find(id);
    if (existing != cache->entries.end()) {
        if (existing->second.parsed) output = existing->second.entries;
        return existing->second.parsed;
    }

    std::string filename;
    for (const auto& entry : entries_) {
        if (entry.numeric_id == id && entry.extension == ".gdbindexes") {
            filename = entry.filename;
            break;
        }
    }

    IndexMetadataCacheEntry parsed;
    if (!filename.empty()) {
        try {
            GdbIndexesParser parser(gdb_path_ + "/" + filename);
            parsed.parsed = parser.parse();
            if (parsed.parsed) parsed.entries = parser.entries();
        } catch (...) {
            parsed.parsed = false;
            parsed.entries.clear();
        }
    }

    const auto inserted = cache->entries.emplace(id, std::move(parsed));
    const IndexMetadataCacheEntry& stored = inserted.first->second;
    if (stored.parsed) output = stored.entries;
    return stored.parsed;
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
