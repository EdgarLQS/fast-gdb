// src/edgar/usegdal/transaction.h
// GdbTransaction — GdbDatasource 事务调用的 RAII 终结器。

#ifndef GDB_TRANSACTION_H
#define GDB_TRANSACTION_H

#include "datasource.h"

/**
 * 将 Datasource 事务绑定到 C++ 作用域。
 *
 * 构造时调用 beginTransaction()；显式 commit()/rollback() 进入互斥终态；
 * 析构时若仍未终结则自动回滚。该类不能补足底层驱动缺失的事务能力，
 * 调用方仍应检查 Datasource 打开模式和 commit() 返回值。
 *
 * 线程安全：事务和其 Datasource 均由单一调用线程独占，不可跨线程共享。
 */
class GdbTransaction {
public:
    /**
     * 在指定 Datasource 上开始事务。
     *
     * Datasource 为非拥有引用，其生命周期必须覆盖事务对象。
     */
    explicit GdbTransaction(GdbDatasource& ds);

    /** 未显式终结时尽力回滚；析构不抛异常。 */
    ~GdbTransaction();

    GdbTransaction(const GdbTransaction&) = delete;
    GdbTransaction& operator=(const GdbTransaction&) = delete;

    /**
     * 提交事务并进入 committed 终态。
     *
     * 重复调用不再次访问驱动，返回当前提交状态。
     */
    bool commit();

    /** 显式回滚并进入 rolledBack 终态。 */
    void rollback();

    bool wasCommitted() const { return m_committed; }
    bool wasRolledBack() const { return m_rolledBack; }

private:
    GdbDatasource& m_ds;      // 非拥有事务目标
    bool m_committed = false;
    bool m_rolledBack = false;
};

#endif // GDB_TRANSACTION_H
