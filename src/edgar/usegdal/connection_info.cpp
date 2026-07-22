// src/connection_info.cpp — GdbConnectionInfo 实现
//
// 将连接配置参数转换为 GDAL 可识别的格式（char** 选项数组）。
// toOpenOptions/freeOpenOptions 使用 GDAL 的 CPL 字符串工具，
// 注意内存所有权契约：调用方负责释放。

#include "connection_info.h"
#include "cpl_string.h"

void GdbConnectionInfo::setServer(const std::string& path) { m_server = path; }
std::string GdbConnectionInfo::getServer() const { return m_server; }

void GdbConnectionInfo::setAlias(const std::string& alias) { m_alias = alias; }
std::string GdbConnectionInfo::getAlias() const { return m_alias; }

void GdbConnectionInfo::setReadOnly(bool ro) { m_readOnly = ro; }
bool GdbConnectionInfo::isReadOnly() const { return m_readOnly; }

void GdbConnectionInfo::setOpenOption(const std::string& key, const std::string& value) {
    m_openOptions[key] = value;
}
const std::map<std::string, std::string>& GdbConnectionInfo::getOpenOptions() const {
    return m_openOptions;
}

char** GdbConnectionInfo::toOpenOptions() const {
    char** papsz = nullptr;
    for (const auto& [key, value] : m_openOptions) {
        papsz = CSLSetNameValue(papsz, key.c_str(), value.c_str());
    }
    return papsz;
}

void GdbConnectionInfo::freeOpenOptions(char** papsz) const {
    CSLDestroy(papsz);
}
