// src/datasource.cpp — GdbDatasource 实现
//
// GdbDatasource 是 GDALDataset* 的 RAII 所有者，管理数据源的完整生命周期：
// 1. 打开：GDALOpenEx + 标志计算 + Open 选项传递
// 2. 关闭：GDALClose + 状态重置
// 3. 事务：StartTransaction/CommitTransaction/RollbackTransaction + 状态机保护
// 4. 视图创建：通过 getDatasets() 创建 GdbDatasets 视图，传递错误上下文指针

#include "datasource.h"
#include "datasets.h"

// ========== 构造/析构 ==========

/** 默认构造。m_ds 初始为 nullptr，需后续调用 open()。 */
GdbDatasource::GdbDatasource() = default;

/**
 * 从已有 GDALDataset* 构造（不接管所有权，仅用于测试）。
 * @param native 外部管理的指针，GdbDatasource 不负责释放
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
GdbDatasource::GdbDatasource(GDALDataset* native) : m_ds(native) {}

/** 析构。自动调用 close() 释放 GDAL 资源。 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
GdbDatasource::~GdbDatasource() {
    close();
}

// ========== 打开/关闭 ==========

/**
 * open() — 使用连接配置打开 GDB 数据源。
 *
 * 实现步骤：
 * 1. close() 释放已有资源（防止重复打开导致内存泄漏）
 * 2. 根据 ReadOnly 标志计算 GDAL 打开标志：
 *    - GDAL_OF_VECTOR：必须以矢量模式打开
 *    - GDAL_OF_READONLY / GDAL_OF_UPDATE：控制读写权限
 * 3. info.toOpenOptions() 构建 GDAL 选项数组
 * 4. GDALOpenEx 打开数据源
 * 5. info.freeOpenOptions() 释放选项数组（防止内存泄漏）
 * 6. 打开失败时设置错误信息，成功时清除
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbDatasource::open(const GdbConnectionInfo& info) {
    close();

    m_alias = info.getAlias();
    m_server = info.getServer();

    unsigned int flags = GDAL_OF_VECTOR;
    if (info.isReadOnly()) {
        flags |= GDAL_OF_READONLY;
    } else {
        flags |= GDAL_OF_UPDATE;
    }

    char** papszOptions = info.toOpenOptions();

    m_ds = (GDALDataset*)GDALOpenEx(m_server.c_str(), flags, nullptr, papszOptions, nullptr);

    info.freeOpenOptions(papszOptions);

    if (!m_ds) {
        setError("Failed to open: " + m_server);
        return false;
    }

    clearError();
    return true;
}

/**
 * openExisting() — 使用默认只读配置打开 GDB 文件。
 * 内部构造 GdbConnectionInfo 设置路径和只读，委托给 open()。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbDatasource::openExisting(const std::string& path) {
    GdbConnectionInfo info;
    info.setServer(path);
    info.setReadOnly(true);
    return open(info);
}

/**
 * close() — 关闭数据源，释放 GDALDataset。
 *
 * 实现步骤：
 * 1. GDALClose 释放 GDAL 资源
 * 2. m_ds 置空，防止悬空指针
 * 3. m_inTransaction 重置（关闭后不应再处于事务中）
 * 4. clearError() 清除错误信息
 * 多次调用安全（m_ds 为 nullptr 时直接返回）。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
void GdbDatasource::close() {
    if (m_ds) {
        GDALClose(m_ds);
        m_ds = nullptr;
    }
    m_inTransaction = false;
    clearError();
}

bool GdbDatasource::isOpen() const { return m_ds != nullptr; }

// ========== 数据集访问 ==========

/**
 * getDatasets() — 获取图层集合视图。
 *
 * 返回一个新的 GdbDatasets 视图对象，持有 m_ds 指针和错误上下文指针。
 * 视图不拥有 OGRLayer，生命周期不得超过 GdbDatasource。
 * 每次调用返回独立视图，可多次调用。
 */
GdbDatasets GdbDatasource::getDatasets() const {
    return GdbDatasets(m_ds, const_cast<GdbDatasource*>(this));
}

// ========== 信息 ==========

std::string GdbDatasource::getAlias() const { return m_alias; }
std::string GdbDatasource::getServer() const { return m_server; }

/** 获取图层数量。未打开时返回 0。 */
int GdbDatasource::getDatasetCount() const {
    return m_ds ? m_ds->GetLayerCount() : 0;
}

// ========== 事务能力检测 ==========

/**
 * supportsTransactions() — 检测是否支持原生事务。
 * 基于 GDALDataset::TestCapability(ODsCTransactions)。
 * OpenFileGDB 通常不支持原生事务，返回 false。
 */
bool GdbDatasource::supportsTransactions() const {
    return m_ds && m_ds->TestCapability(ODsCTransactions);
}

/**
 * supportsEmulatedTransactions() — 检测是否支持模拟事务。
 * 基于 GDALDataset::TestCapability(ODsCEmulatedTransactions)。
 * 模拟事务在内存中暂存修改，关闭数据源时不持久化。
 */
bool GdbDatasource::supportsEmulatedTransactions() const {
    return m_ds && m_ds->TestCapability(ODsCEmulatedTransactions);
}

// ========== 事务操作 ==========

/**
 * beginTransaction() — 开始事务。
 *
 * 实现策略：三层前置检查
 * 1. 数据源必须已打开（m_ds != nullptr）
 * 2. 当前不能在事务中（m_inTransaction == false）
 * 3. 数据源必须支持原生事务或模拟事务
 * 通过检查后调用 GDALDataset::StartTransaction()。
 * 成功时设置 m_inTransaction = true，清除错误信息。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbDatasource::beginTransaction() {
    if (!m_ds) {
        setError("Datasource not open");
        return false;
    }
    if (m_inTransaction) {
        setError("Already in a transaction");
        return false;
    }
    if (!supportsTransactions() && !supportsEmulatedTransactions()) {
        setError("Transactions not supported by this datasource");
        return false;
    }
    OGRErr ret = m_ds->StartTransaction();
    if (ret != CE_None) {
        setError("Failed to start transaction");
        return false;
    }
    m_inTransaction = true;
    clearError();
    return true;
}

/**
 * commitTransaction() — 提交事务。
 *
 * 实现策略：
 * 1. 检查是否在事务中（前置条件保护）
 * 2. 调用 GDALDataset::CommitTransaction()
 * 3. 无论成功失败，都重置 m_inTransaction（事务已结束）
 * 提交失败时设置错误信息。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbDatasource::commitTransaction() {
    if (!m_ds || !m_inTransaction) {
        setError("Not in a transaction");
        return false;
    }
    OGRErr ret = m_ds->CommitTransaction();
    m_inTransaction = false;
    if (ret != CE_None) {
        setError("Failed to commit transaction");
        return false;
    }
    clearError();
    return true;
}

/**
 * rollbackTransaction() — 回滚事务。
 *
 * 实现策略：
 * 1. 检查是否在事务中（前置条件保护）
 * 2. 调用 GDALDataset::RollbackTransaction()
 * 3. 无论成功失败，都重置 m_inTransaction（事务已结束）
 * 回滚失败时设置错误信息。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbDatasource::rollbackTransaction() {
    if (!m_ds || !m_inTransaction) {
        setError("Not in a transaction");
        return false;
    }
    OGRErr ret = m_ds->RollbackTransaction();
    m_inTransaction = false;
    if (ret != CE_None) {
        setError("Failed to rollback transaction");
        return false;
    }
    clearError();
    return true;
}

// ========== 底层访问 ==========

GDALDataset* GdbDatasource::getNative() const { return m_ds; }
