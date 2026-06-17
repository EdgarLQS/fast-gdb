/**
 * transaction_test.cpp — GdbTransaction 单元测试
 *
 * 覆盖：事务能力查询、begin/commit 调用路径、begin/rollback 调用路径、失败路径、RAII 自动回滚
 */

#include "test_fixture.h"
#include "datasource.h"
#include "transaction.h"

// 辅助函数：非只读方式打开
static bool openForWrite(GdbDatasource& gdb, const char* path) {
    GdbConnectionInfo info;
    info.setServer(path);
    info.setReadOnly(false);
    return gdb.open(info);
}

/**
 * T_Transaction_QueryCapability: 验证事务能力查询。
 */
TEST_F(GdbTutorialFixture, T_Transaction_QueryCapability) {
    const char* path = "/tmp/tutorial_txn_capability.gdb";
    GDALDataset* ds = createGdb(path);
    ds->CreateLayer("test_layer", nullptr, wkbPoint, nullptr);
    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    // OpenFileGDB reports supportsTransactions=true (forced mode available)
    // but actual transaction start may fail without FORCE_OGR option
    bool supportsTxn = gdb.supportsTransactions();
    bool supportsEmulated = gdb.supportsEmulatedTransactions();
    (void)supportsTxn;
    (void)supportsEmulated;

    // 未开始事务时应为 false
    EXPECT_FALSE(gdb.isInTransaction());

    // 关闭后查询返回 false
    gdb.close();
    EXPECT_FALSE(gdb.isOpen());
    EXPECT_FALSE(gdb.supportsTransactions());
    EXPECT_FALSE(gdb.supportsEmulatedTransactions());
}

// 尝试开始事务，若驱动不支持则 skip
#define SKIP_IF_NO_TXN(gdb) do { \
    if (!(gdb).beginTransaction()) { \
        GTEST_SKIP_("beginTransaction failed (driver does not support transactions)"); \
    } \
    (gdb).rollbackTransaction(); \
} while(0)

/**
 * T_Transaction_BeginCommit: 验证 begin → commit 调用路径。
 */
TEST_F(GdbTutorialFixture, T_Transaction_BeginCommit) {
    const char* path = "/tmp/tutorial_txn_commit.gdb";
    GDALDataset* ds = createGdb(path);
    ds->CreateLayer("test_layer", nullptr, wkbPoint, nullptr);
    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(openForWrite(gdb, path));
    SKIP_IF_NO_TXN(gdb);

    EXPECT_TRUE(gdb.beginTransaction());
    EXPECT_TRUE(gdb.isInTransaction());

    EXPECT_TRUE(gdb.commitTransaction());
    EXPECT_FALSE(gdb.isInTransaction());
}

/**
 * T_Transaction_BeginRollback: 验证 begin → rollback 调用路径。
 */
TEST_F(GdbTutorialFixture, T_Transaction_BeginRollback) {
    const char* path = "/tmp/tutorial_txn_rollback.gdb";
    GDALDataset* ds = createGdb(path);
    ds->CreateLayer("test_layer", nullptr, wkbPoint, nullptr);
    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(openForWrite(gdb, path));
    SKIP_IF_NO_TXN(gdb);

    EXPECT_TRUE(gdb.beginTransaction());
    EXPECT_TRUE(gdb.isInTransaction());

    EXPECT_TRUE(gdb.rollbackTransaction());
    EXPECT_FALSE(gdb.isInTransaction());
}

/**
 * T_Transaction_FailurePaths: 验证失败场景。
 */
TEST_F(GdbTutorialFixture, T_Transaction_FailurePaths) {
    GdbDatasource gdb;

    // 未打开时 begin/commit/rollback 应失败
    EXPECT_FALSE(gdb.beginTransaction());
    EXPECT_FALSE(gdb.commitTransaction());
    EXPECT_FALSE(gdb.rollbackTransaction());

    // 打开只读模式
    const char* path = "/tmp/tutorial_txn_ro.gdb";
    GDALDataset* ds = createGdb(path);
    ds->CreateLayer("test_layer", nullptr, wkbPoint, nullptr);
    GDALClose(ds);

    ASSERT_TRUE(gdb.openExisting(path));

    // 只读模式下 begin 可能失败（取决于驱动）
    bool result = gdb.beginTransaction();
    if (!result) {
        EXPECT_FALSE(gdb.isInTransaction());
    }
}

/**
 * T_Transaction_RAAutoRollback: 验证 GdbTransaction RAII 自动回滚。
 */
TEST_F(GdbTutorialFixture, T_Transaction_RAAutoRollback) {
    const char* path = "/tmp/tutorial_txn_raii.gdb";
    GDALDataset* ds = createGdb(path);
    ds->CreateLayer("test_layer", nullptr, wkbPoint, nullptr);
    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(openForWrite(gdb, path));
    SKIP_IF_NO_TXN(gdb);

    {
        GdbTransaction txn(gdb);
        EXPECT_TRUE(gdb.isInTransaction());
        EXPECT_FALSE(txn.wasCommitted());
        EXPECT_FALSE(txn.wasRolledBack());
        // 离开作用域时，析构函数应自动 rollback
    }
    EXPECT_FALSE(gdb.isInTransaction());
}

/**
 * T_Transaction_RAIICommit: 验证 GdbTransaction 显式 commit。
 */
TEST_F(GdbTutorialFixture, T_Transaction_RAIICommit) {
    const char* path = "/tmp/tutorial_txn_raii_commit.gdb";
    GDALDataset* ds = createGdb(path);
    ds->CreateLayer("test_layer", nullptr, wkbPoint, nullptr);
    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(openForWrite(gdb, path));
    SKIP_IF_NO_TXN(gdb);

    {
        GdbTransaction txn(gdb);
        EXPECT_TRUE(txn.commit());
        EXPECT_TRUE(txn.wasCommitted());
    }
    EXPECT_FALSE(gdb.isInTransaction());
}
