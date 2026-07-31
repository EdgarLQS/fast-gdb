// src/datasource.h — GdbDatasource 数据源（GDALDataset* 的 RAII 所有者）
//
// GdbDatasource 是整个组件层级的根对象：
// 1. 拥有 GDALDataset* 的生命周期（open/close）
// 2. 继承 GdbErrorContext，提供统一错误上下文
// 3. 通过 getDatasets() 创建 GdbDatasets 视图
// 4. 提供事务操作（beginTransaction/commitTransaction/rollbackTransaction）
//
// 生命周期约束：
// - GdbDatasets/GdbDataset/GdbRecordset 等非拥有视图对象的生命周期不得超过 GdbDatasource
// - GdbDatasource 被销毁时，所有底层 GDAL 资源被关闭，视图指针失效
//
// 线程安全：
// - GdbDatasource 本身不线程安全
// - 多线程并发时，每个线程通过 GdbConnectionPool 获取独立连接，各自拥有 GdbDatasource

#ifndef GDB_DATASOURCE_H
#define GDB_DATASOURCE_H

#include "error_context.h"
#include "connection_info.h"
#include "gdal_priv.h"

class GdbDatasets;
class GdbDataset;
class GdbRecordset;

/**
 * GdbDatasource — GDB 数据源，GDALDataset* 的 RAII 封装。
 *
 * 核心职责：
 * 1. 打开/关闭 GDB 文件（open/openExisting/close）
 * 2. 管理事务（beginTransaction/commitTransaction/rollbackTransaction）
 * 3. 提供错误上下文（继承 GdbErrorContext）
 * 4. 创建图层集合视图（getDatasets）
 *
 * 不可拷贝、不可移动：直接拥有 GDAL 资源，不允许所有权转移。
 * 多线程场景下，每个线程应有独立的 GdbDatasource 实例。
 */
class GdbDatasource : public GdbErrorContext {
public:
    GdbDatasource();

    /**
     * 从已有 GDALDataset* 构造（不接管所有权，仅用于测试）。
     * @param native 外部管理的 GDALDataset 指针，GdbDatasource 不负责释放
     */
    explicit GdbDatasource(GDALDataset* native);

    ~GdbDatasource();

    // 不可拷贝、不可移动：直接拥有 GDAL 资源，不允许所有权转移
    GdbDatasource(const GdbDatasource&) = delete;
    GdbDatasource& operator=(const GdbDatasource&) = delete;
    GdbDatasource(GdbDatasource&&) = delete;
    GdbDatasource& operator=(GdbDatasource&&) = delete;

    // ========== 打开/关闭 ==========

    /**
     * 使用连接配置打开 GDB 数据源。
     *
     * 实现策略：
     * 1. 先 close() 释放已有的 GDALDataset
     * 2. 根据 ReadOnly 标志计算 GDAL_OF_VECTOR | GDAL_OF_READONLY/UPDATE
     * 3. 将 Open 选项转为 char** 传给 GDALOpenEx
     * 4. 打开失败时设置错误信息，成功时清除
     *
     * @param info 连接配置（路径、只读、Open 选项）
     * @return true 打开成功，false 失败（通过 getLastError() 获取原因）
     */
    bool open(const GdbConnectionInfo& info);

    /**
     * 使用默认配置（只读）打开指定路径的 GDB 文件。
     * 等价于构造 GdbConnectionInfo 设置路径和只读后调用 open()。
     *
     * @param path GDB 文件路径
     * @return true 打开成功，false 失败
     */
    bool openExisting(const std::string& path);

    /**
     * 关闭数据源，释放 GDALDataset。
     * 同时重置事务状态、清除错误信息。
     * 多次调用安全（幂等）。
     */
    void close();

    /** 判断数据源是否已打开。
     * @return 已成功绑定 GDALDataset 时返回 true。
     */
    bool isOpen() const;

    // ========== 数据集访问 ==========

    /**
     * 获取图层集合视图。
     *
     * 每次调用返回一个新的 GdbDatasets 视图（临时对象），
     * 不拥有 OGRLayer 指针，视图的生命周期不得超过 GdbDatasource。
     */
    GdbDatasets getDatasets() const;

    // ========== 信息 ==========

    /** 获取数据源别名。
     * @return 别名文本。
     */
    std::string getAlias() const;

    /** 获取 GDB 文件路径。
     * @return 数据源路径。
     */
    std::string getServer() const;

    /** 获取图层数量。
     * @return 图层数量；数据源未打开时返回 0。
     */
    int getDatasetCount() const;

    // ========== 事务能力检测 ==========

    /**
     * 是否支持原生事务。
     * 基于 GDALDataset::TestCapability(ODsCTransactions)。
     * OpenFileGDB 通常不支持原生事务。
     */
    bool supportsTransactions() const;

    /**
     * 是否支持模拟事务。
     * 基于 GDALDataset::TestCapability(ODsCEmulatedTransactions)。
     * 模拟事务在内存中暂存修改，关闭时不持久化。
     */
    bool supportsEmulatedTransactions() const;

    // ========== 事务操作 ==========

    /**
     * 开始事务。
     *
     * 前置条件：
     * 1. 数据源已打开（isOpen() == true）
     * 2. 当前不在事务中
     * 3. 数据源支持原生事务或模拟事务
     *
     * @return true 事务已开始，false 失败（通过 getLastError() 获取原因）
     */
    bool beginTransaction();

    /**
     * 提交事务。
     *
     * 前置条件：当前在事务中。
     * 提交后自动退出事务状态。
     *
     * @return true 提交成功，false 失败
     */
    bool commitTransaction();

    /**
     * 回滚事务。
     *
     * 前置条件：当前在事务中。
     * 回滚后自动退出事务状态。
     *
     * @return true 回滚成功，false 失败
     */
    bool rollbackTransaction();

    /** 判断是否处于事务中。
     * @return 当前存在活动事务时返回 true。
     */
    bool isInTransaction() const { return m_inTransaction; }

    // ========== 底层访问 ==========

    /** 获取底层 GDALDataset 指针。
     * @return 非拥有的 GDALDataset 指针；未打开时返回 nullptr。
     */
    GDALDataset* getNative() const;

private:
    friend class GdbDatasets;
    friend class GdbDataset;
    friend class GdbRecordset;

    /** 底层 GDALDataset 指针（GdbDatasource 拥有其生命周期）。 */
    GDALDataset* m_ds = nullptr;

    /** 数据源别名。 */
    std::string m_alias;

    /** GDB 文件路径。 */
    std::string m_server;

    /** 是否处于事务中。 */
    bool m_inTransaction = false;
};

#endif // GDB_DATASOURCE_H
