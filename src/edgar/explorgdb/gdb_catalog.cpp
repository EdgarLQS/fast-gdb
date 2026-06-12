// src/edgar/explorgdb/gdb_catalog.cpp
// 目录扫描实现 — 使用 std::filesystem 枚举文件，正则匹配 aXXXXXXXX.<ext> 格式
//
// .gdb 目录的文件名约定：
//   - 数据文件: aXXXXXXXX.<ext>（8 位十六进制数字 + 扩展名）
//   - 特殊文件: "gdb"（8 字节头部）、"timestamps"（384 字节）
//   - 扩展名: .gdbtable, .gdbtablx, .gdbindexes, .spx, .atx
//
// numeric_id 从 aXXXXXXXX 中提取（十六进制转十进制）:
//   a00000001.gdbtable → id=1
//   a00000009.gdbtable → id=9

#include "gdb_catalog.h"
#include "binary_reader.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>

namespace fs = std::filesystem;

namespace explorgdb {

// ── 目录扫描 ──
bool GdbCatalog::scan(const std::string& gdb_path) {
    gdb_path_ = gdb_path;
    entries_.clear();
    has_magic_ = false;
    has_timestamps_ = false;

    // 检查目录存在性
    if (!fs::exists(gdb_path) || !fs::is_directory(gdb_path)) {
        std::cerr << "Error: " << gdb_path << " is not a directory\n";
        return false;
    }

    // 正则匹配: a00000001.gdbtable → group1="00000001", group2="gdbtable"
    static const std::regex hex_file_re("^a([0-9a-fA-F]{8})\\.(.+)$");

    for (const auto& entry : fs::directory_iterator(gdb_path)) {
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();

        // 特殊文件单独处理
        if (fname == "gdb") {
            read_magic();
            continue;
        }
        if (fname == "timestamps") {
            read_timestamps();
            continue;
        }

        // 匹配 aXXXXXXXX.<ext> 格式
        std::smatch match;
        if (std::regex_match(fname, match, hex_file_re)) {
            CatalogEntry ce;
            ce.filename = fname;
            // 十六进制字符串转数字（如 "00000001" → 1）
            ce.numeric_id = static_cast<uint32_t>(std::stoul(match[1].str(), nullptr, 16));
            ce.extension = "." + match[2].str();
            ce.file_size = entry.file_size();
            entries_.push_back(ce);
        }
    }

    // 按 numeric_id 升序排列，保证遍历顺序一致
    std::sort(entries_.begin(), entries_.end(),
              [](const CatalogEntry& a, const CatalogEntry& b) {
                  return a.numeric_id < b.numeric_id;
              });

    return true;
}

// ── 读取 8 字节 magic 文件 ──
// 预期内容:
//   [0:4] version = 5（目录版本）
//   [4:8] magic = 0xDEADBEEF（文件标识）
// 注意：0xDEADBEEF 以小端存储，read_u32 读取后为 0xEFBEADDE
bool GdbCatalog::read_magic() {
    std::string magic_file = fs::path(gdb_path_) / "gdb";
    std::ifstream ifs(magic_file, std::ios::binary);
    if (!ifs.is_open()) return false;

    std::vector<uint8_t> buf(8);
    ifs.read(reinterpret_cast<char*>(buf.data()), 8);
    if (ifs.gcount() != 8) return false;

    BinaryReader br(buf);
    magic_.version = br.read_u32();
    magic_.magic = br.read_u32();
    has_magic_ = true;
    return true;
}

// ── 读取 384 字节 timestamps 文件 ──
// 内容尚未完全解析，目前保留原始字节供后续研究
bool GdbCatalog::read_timestamps() {
    std::string ts_file = fs::path(gdb_path_) / "timestamps";
    std::ifstream ifs(ts_file, std::ios::binary);
    if (!ifs.is_open()) return false;

    std::vector<uint8_t> buf(384);
    ifs.read(reinterpret_cast<char*>(buf.data()), 384);
    if (ifs.gcount() != 384) return false;

    std::memcpy(timestamps_.raw, buf.data(), 384);
    has_timestamps_ = true;
    return true;
}

// ── 按扩展名筛选 ──
std::vector<const CatalogEntry*> GdbCatalog::find_by_extension(const std::string& ext) const {
    std::vector<const CatalogEntry*> result;
    for (const auto& e : entries_) {
        if (e.extension == ext) result.push_back(&e);
    }
    return result;
}

// ── 按 ID 查找 .gdbtable ──
const CatalogEntry* GdbCatalog::find_table(uint32_t id) const {
    for (const auto& e : entries_) {
        if (e.numeric_id == id && e.extension == ".gdbtable") return &e;
    }
    return nullptr;
}

// ── 按 ID 查找 .gdbtablx ──
const CatalogEntry* GdbCatalog::find_tablx(uint32_t id) const {
    for (const auto& e : entries_) {
        if (e.numeric_id == id && e.extension == ".gdbtablx") return &e;
    }
    return nullptr;
}

// ── 按 ID 查找 .spx ──
const CatalogEntry* GdbCatalog::find_spx(uint32_t id) const {
    for (const auto& e : entries_) {
        if (e.numeric_id == id && e.extension == ".spx") return &e;
    }
    return nullptr;
}

// ── 按 ID + 索引名查找 .atx ──
// .atx 文件名格式: aXXXXXXXX.<index_name>.atx
// extension 字段存储为 ".<index_name>.atx"（如 ".MyIndex.atx"）
const CatalogEntry* GdbCatalog::find_atx(uint32_t id, const std::string& index_name) const {
    std::string target_ext = "." + index_name + ".atx";
    for (const auto& e : entries_) {
        if (e.numeric_id == id && e.extension == target_ext) return &e;
    }
    return nullptr;
}

// ── 查找某个表的所有 .atx 文件 ──
std::vector<const CatalogEntry*> GdbCatalog::find_all_atx(uint32_t id) const {
    std::vector<const CatalogEntry*> result;
    std::string suffix = ".atx";
    for (const auto& e : entries_) {
        if (e.numeric_id == id && e.extension.size() > suffix.size() &&
            e.extension.substr(e.extension.size() - suffix.size()) == suffix) {
            result.push_back(&e);
        }
    }
    return result;
}

} // namespace explorgdb
