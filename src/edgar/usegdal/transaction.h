// src/transaction.h — GdbTransaction 事务（FileGDB API 事务的 RAII 封装）
//
// GdbTransaction 是对 FileGDB API beginTransaction/commitTransaction/
// rollbackTransaction 的 RAII 封装，提供异常安全的事务管理。
// 核心关注点：
// 1. RAII 生命周期：构造函数自动 beginTransaction()，
//    析构函数在未 commit 时自动 rollbackTransaction()。
// 2. 状态机：committed / rolledBack 两种终态互斥，防止重复提交或回滚。
// 3. 不可拷贝：事务绑定到唯一的 Datasource 引用，拷贝无意义。

#ifndef GDB_TRANSACTION_H
#define GDB_TRANSACTION_H

#include "datasource.h"

/**
 * GdbTransaction — RAII 事务封装，确保事务总是被正确终结。
 *
 * RAII 契约：
 * - 构造时：自动调用 GdbDatasource::beginTransaction() 开启事务。
 * - 析构时：如果既未 commit 也未 rollback，自动调用 rollbackTransaction()
 *   回滚事务，防止因异常或提前返回导致事务悬停。
 * - commit()/rollback()：显式终结事务，标记终态。
 *   commit() 是幂等的——重复调用不会再次提交。
 *
 * 使用模式：
 *   {
 *       GdbTransaction txn(ds);
 *       // ... 执行写入操作 ...
 *       txn.commit();  // 成功则提交
 *   }  // 离开作用域时，若未 commit 则自动回滚
 *
 * 线程安全：
 *   GdbTransaction 不线程安全。一个事务绑定到一个 GdbDatasource 实例，
 *   多线程应使用独立的 Datasource/Connection。
 */
class GdbTransaction {
public:
    /**
     * 构造事务，自动开启数据库事务。
     * @param ds 数据源引用，事务在其上执行。
     */
    explicit GdbTransaction(GdbDatasource& ds);

    /**
     * 析构事务。若未提交也未回滚，自动回滚以保证事务安全终结。
     */
    ~GdbTransaction();

    // 不可拷贝：事务绑定到唯一的 Datasource 引用，拷贝会导致状态混乱
    GdbTransaction(const GdbTransaction&) = delete;
    GdbTransaction& operator=(const GdbTransaction&) = delete;

    /**
     * 提交事务。
     * @return 提交是否成功。幂等——已提交时重复调用直接返回上次的结果。
     */
    bool commit();

    /**
     * 显式回滚事务。
     * 标记为已回滚，析构时不再重复回滚。
     */
    void rollback();

    /** 事务是否已成功提交。 */
    bool wasCommitted() const { return m_committed; }

    /** 事务是否已被显式回滚。 */
    bool wasRolledBack() const { return m_rolledBack; }

private:
    /** 绑定的数据源引用（非拥有）。 */
    GdbDatasource& m_ds;

    /** 事务是否已提交（终态标记）。 */
    bool m_committed = false;

    /** 事务是否已被显式回滚（终态标记）。 */
    bool m_rolledBack = false;
};

#endif // GDB_TRANSACTION_H
