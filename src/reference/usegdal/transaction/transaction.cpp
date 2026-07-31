// src/transaction.cpp — GdbTransaction 实现
//
// GdbTransaction 提供异常安全的事务管理。
// 核心安全保证：
// 1. 构造函数自动 beginTransaction()，确保事务从创建时即生效。
// 2. 析构函数检查终态标记（m_committed / m_rolledBack），
//    若两者均为 false 且 Datasource 仍在事务中，则自动 rollback。
//    这保证了即使发生异常或函数提前 return，事务也不会悬停。
// 3. commit() 通过 m_committed 实现幂等守卫——提交成功后重复调用
//    不会再次调用底层的 commitTransaction。

#include "transaction.h"

// ========== 构造/析构 ==========

/**
 * 构造事务，自动开启数据库事务。
 *
 * 实现策略：
 * 1. 初始化成员引用 m_ds
 * 2. 调用 m_ds.beginTransaction() 开启 FileGDB API 事务
 *    此后所有写入操作都在事务保护下进行。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
GdbTransaction::GdbTransaction(GdbDatasource& ds) : m_ds(ds) {
    m_ds.beginTransaction();
}

/**
 * 析构事务，确保事务安全终结。
 *
 * 实现策略（RAII 安全保证的核心）：
 * 1. 检查是否已提交或已回滚——如果任一终态标记为 true，说明事务
 *    已被显式终结，无需额外操作。
 * 2. 检查 m_ds.isInTransaction() 确认数据源仍处于事务状态。
 * 3. 如果既未终结又仍在事务中，调用 rollbackTransaction() 回滚。
 *    这是异常安全的关键——无论正常退出、异常抛出还是提前 return，
 *    事务都不会悬停在数据库上。
 *
 * 注意：此方法不应抛出异常（析构函数 noexcept），
 * 即使 rollbackTransaction 失败也仅静默忽略。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
GdbTransaction::~GdbTransaction() {
    if (!m_committed && !m_rolledBack && m_ds.isInTransaction()) {
        m_ds.rollbackTransaction();
    }
}

// ========== 事务控制 ==========

/**
 * 提交事务。
 *
 * 实现策略：
 * 1. 调用 m_ds.commitTransaction() 执行底层提交。
 * 2. 如果提交成功（ok == true），设置 m_committed = true 标记终态。
 *    这是幂等守卫——析构函数检查到 m_committed 为 true 后不再回滚，
 *    重复调用 commit() 时也不会再次提交（因为 m_ds 已不在事务中）。
 *
 * @return 提交是否成功。失败时事务仍未开启，析构时会回滚。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
bool GdbTransaction::commit() {
    bool ok = m_ds.commitTransaction();
    if (ok) m_committed = true;
    return ok;
}

/**
 * 显式回滚事务。
 *
 * 实现策略：
 * 1. 调用 m_ds.rollbackTransaction() 执行底层回滚。
 * 2. 设置 m_rolledBack = true 标记终态，防止析构函数重复回滚。
 *
 * 适用场景：业务逻辑判断需要撤销操作时提前回滚，
 * 而不是等到析构时被动回滚。
 */
// 方法实现：用途、参数和返回值契约见对应头文件或本文件声明。
void GdbTransaction::rollback() {
    m_ds.rollbackTransaction();
    m_rolledBack = true;
}
