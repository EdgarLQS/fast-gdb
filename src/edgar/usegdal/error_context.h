// src/error_context.h — GdbErrorContext 错误上下文（纯头文件 mixin）
//
// GdbErrorContext 为所有视图对象提供统一的错误信息存储和读取接口。
// GdbDatasource 继承此类，GdbDataset/GdbRecordset 等视图通过持有错误指针
// 转发错误信息，形成统一的错误上下文链。
//
// 线程安全：不线程安全。调用方需保证同一错误上下文不被多线程同时修改。
// 多线程场景下，每个线程通过 GdbConnectionPool 获取独立连接，各自拥有错误上下文。

#ifndef GDB_ERROR_CONTEXT_H
#define GDB_ERROR_CONTEXT_H

#include <string>

/**
 * GdbErrorContext — 错误上下文基类（纯头文件 mixin）。
 *
 * 所有视图对象（GdbDatasource/GdbDataset/GdbRecordset 等）通过继承或持有
 * 此类的指针，提供统一的错误信息存储和读取接口。
 *
 * 使用约定：
 * - 方法返回 false 时，调用方可通过 getLastError() 获取详细错误信息
 * - setError() 覆盖前一次的错误，不累积
 * - clearError() 手动清除，通常在操作开始前调用
 */
class GdbErrorContext {
public:
    virtual ~GdbErrorContext() = default;

    /** 设置最新错误信息，覆盖前一次的错误。 */
    void setError(const std::string& msg) { m_lastError = msg; }

    /** 获取最新错误信息。无错误时返回空字符串。 */
    std::string getLastError() const { return m_lastError; }

    /** 清除错误信息。 */
    void clearError() { m_lastError.clear(); }

protected:
    std::string m_lastError;
};

#endif // GDB_ERROR_CONTEXT_H
