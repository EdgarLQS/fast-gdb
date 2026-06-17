/**
 * datasource_test.cpp — GdbDatasource 单元测试
 *
 * 覆盖：打开/关闭、事务能力检测、错误上下文
 */

#include "test_fixture.h"
#include "datasource.h"

/**
 * T_Datasource_OpenClose: 验证打开和关闭 GDB。
 */
TEST_F(GdbTutorialFixture, T_Datasource_OpenClose) {
    const char* path = "/tmp/tutorial_ds_open_close.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);
    ds->CreateLayer("points", nullptr, wkbPoint, nullptr);
    GDALClose(ds);

    GdbDatasource gdb;
    EXPECT_TRUE(gdb.openExisting(path));
    EXPECT_TRUE(gdb.isOpen());
    EXPECT_EQ(gdb.getDatasetCount(), 1);

    gdb.close();
    EXPECT_FALSE(gdb.isOpen());
    EXPECT_EQ(gdb.getDatasetCount(), 0);
}

/**
 * T_Datasource_OpenViaConnectionInfo: 通过 GdbConnectionInfo 打开。
 */
TEST_F(GdbTutorialFixture, T_Datasource_OpenViaConnectionInfo) {
    const char* path = "/tmp/tutorial_ds_conn_info.gdb";
    GDALDataset* ds = createGdb(path);
    ASSERT_NE(ds, nullptr);
    ds->CreateLayer("lines", nullptr, wkbLineString, nullptr);
    GDALClose(ds);

    GdbConnectionInfo info;
    info.setServer(path);
    info.setAlias("test_gdb");

    GdbDatasource gdb;
    EXPECT_TRUE(gdb.open(info));
    EXPECT_TRUE(gdb.isOpen());
    EXPECT_STREQ(gdb.getServer().c_str(), path);
    EXPECT_STREQ(gdb.getAlias().c_str(), "test_gdb");

    gdb.close();
}

/**
 * T_Datasource_OpenNonexistent: 打开不存在的路径应返回 false + 错误信息。
 */
TEST_F(GdbTutorialFixture, T_Datasource_OpenNonexistent) {
    GdbDatasource gdb;
    EXPECT_FALSE(gdb.openExisting("/tmp/tutorial_ds_noexist.gdb"));
    EXPECT_FALSE(gdb.isOpen());
    EXPECT_FALSE(gdb.getLastError().empty());
}

/**
 * T_Datasource_EmptyState: 未打开的 datasource 状态验证。
 */
TEST_F(GdbTutorialFixture, T_Datasource_EmptyState) {
    GdbDatasource gdb;
    EXPECT_EQ(gdb.getDatasetCount(), 0);
    EXPECT_FALSE(gdb.isOpen());
    EXPECT_FALSE(gdb.supportsTransactions());
}

/**
 * T_Datasource_QueryCapability: 事务能力查询。
 */
TEST_F(GdbTutorialFixture, T_Datasource_QueryCapability) {
    const char* path = "/tmp/tutorial_ds_capability.gdb";
    GDALDataset* ds = createGdb(path);
    ds->CreateLayer("test_layer", nullptr, wkbPoint, nullptr);
    GDALClose(ds);

    GdbDatasource gdb;
    ASSERT_TRUE(gdb.openExisting(path));

    // OpenFileGDB 驱动行为可能不支持事务，仅验证查询不崩溃
    bool supportsTxn = gdb.supportsTransactions();
    bool supportsEmulated = gdb.supportsEmulatedTransactions();
    // 结果取决于驱动，不做强断言
    (void)supportsTxn;
    (void)supportsEmulated;

    EXPECT_FALSE(gdb.isInTransaction());
}
