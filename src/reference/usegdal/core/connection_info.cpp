// src/connection_info.cpp — GdbConnectionInfo 实现
//
// 将连接配置参数转换为 GDAL 可识别的格式（char** 选项数组）。
// toOpenOptions/freeOpenOptions 使用 GDAL 的 CPL 字符串工具，
// 注意内存所有权契约：调用方负责释放。

#include "connection_info.h"
#include "cpl_string.h"

/** 设置数据源路径；参数和返回语义见 connection_info.h。 */
// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
void GdbConnectionInfo::setServer(const std::string& path) { m_server = path; }
/** 返回当前数据源路径。 */
std::string GdbConnectionInfo::getServer() const { return m_server; }

/** 设置数据源别名。 */
// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
void GdbConnectionInfo::setAlias(const std::string& alias) { m_alias = alias; }
/** 返回数据源别名。 */
std::string GdbConnectionInfo::getAlias() const { return m_alias; }

/** 设置数据源只读标志。 */
// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
void GdbConnectionInfo::setReadOnly(bool ro) { m_readOnly = ro; }
/** 返回数据源只读标志。 */
bool GdbConnectionInfo::isReadOnly() const { return m_readOnly; }

/** 设置一个 GDAL Open 选项。 */
// 方法实现：具体用途、参数和返回值契约见对应头文件或本文件声明。
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
void GdbConnectionInfo::setOpenOption(const std::string& key, const std::string& value) {
    m_openOptions[key] = value;
}
/** 返回全部 GDAL Open 选项。 */
const std::map<std::string, std::string>& GdbConnectionInfo::getOpenOptions() const {
    return m_openOptions;
}

/** 将选项转换为调用方拥有释放责任的 GDAL 字符串数组。 */
char** GdbConnectionInfo::toOpenOptions() const {
    char** papsz = nullptr;
    for (const auto& [key, value] : m_openOptions) {
        papsz = CSLSetNameValue(papsz, key.c_str(), value.c_str());
    }
    return papsz;
}

/** 释放 toOpenOptions 返回的 GDAL 字符串数组。 */
void GdbConnectionInfo::freeOpenOptions(char** papsz) const {
    CSLDestroy(papsz);
}
