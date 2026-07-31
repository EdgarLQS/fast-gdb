// src/connection_info.h — GdbConnectionInfo 连接配置 DTO
//
// 封装打开 GDB 数据源所需的全部参数（路径、别名、只读模式、GDAL Open 选项）。
// 作为纯数据传输对象，不包含业务逻辑，只负责参数的设置、获取和格式转换。
//
// 典型用途：
//   GdbConnectionInfo info;
//   info.setServer("/path/to/data.gdb");
//   info.setReadOnly(true);
//   info.setOpenOption("LOCK_TIMEOUT", "30");
//   GdbDatasource ds;
//   ds.open(info);

#ifndef GDB_CONNECTION_INFO_H
#define GDB_CONNECTION_INFO_H

#include <string>
#include <map>

/**
 * GdbConnectionInfo — GDB 连接配置数据传输对象。
 *
 * 封装 GdbDatasource::open() 所需的全部参数。
 * 不包含 GDAL 原生指针，可安全持有、跨函数传递。
 */
class GdbConnectionInfo {
public:
    /** 设置 GDB 文件路径。
     * @param path GDB 目录或文件路径。
     */
    void setServer(const std::string& path);

    /** 获取 GDB 文件路径。
     * @return 路径文本。
     */
    std::string getServer() const;

    /** 设置数据源别名。
     * @param alias 仅用于标识的别名文本。
     */
    void setAlias(const std::string& alias);

    /** 获取数据源别名。
     * @return 别名文本。
     */
    std::string getAlias() const;

    /** 设置只读模式。
     * @param ro true 使用只读打开，false 请求更新打开。
     */
    void setReadOnly(bool ro);

    /** 判断是否为只读模式。
     * @return 只读时返回 true。
     */
    bool isReadOnly() const;

    /**
     * 设置 GDAL Open 选项（KEY=VALUE 键值对）。
     * 这些选项会在 open() 时传给 GDALOpenEx()，
     * 如 "LOCK_TIMEOUT"、"SHAPE_ENCODING" 等。
     */
    void setOpenOption(const std::string& key, const std::string& value);

    /** 获取全部 GDAL Open 选项。
     * @return 选项映射的只读引用。
     */
    const std::map<std::string, std::string>& getOpenOptions() const;

    /**
     * 将 Open 选项转换为 GDALOpenEx 所需的 char** 格式。
     *
     * 内存所有权：使用 CSLSetNameValue() 分配内存，
     * 返回 NULL 结尾的字符串数组。调用方必须用 freeOpenOptions() 释放。
     *
     * @return char** GDAL 风格的选项数组，需调用 freeOpenOptions() 释放
     */
    char** toOpenOptions() const;

    /**
     * 释放 toOpenOptions() 返回的内存。
     *
     * @param papsz toOpenOptions() 返回的指针
     * @return 无返回值。
     */
    void freeOpenOptions(char** papsz) const;

private:
    std::string m_server;
    std::string m_alias;
    bool m_readOnly = false;
    std::map<std::string, std::string> m_openOptions;
};

#endif // GDB_CONNECTION_INFO_H
