/**
 * pool_test.cpp — GdbConnectionPool 连接池测试
 *
 * 覆盖：acquire/release 生命周期、idle/active 计数、池满拒绝
 */

#include "test_fixture.h"
#include "connection_info.h"
#include "connection_pool.h"

/**
 * T_Pool_AcquireRelease: 获取和归还连接。
 */
TEST_F(GdbTutorialFixture, T_Pool_AcquireRelease) {
    GdbConnectionInfo info;
    info.setServer("/tmp/tutorial_pool.gdb");
    info.setReadOnly(true);

    // 先创建 GDB
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    GDALDeleteDataset(drv, info.getServer().c_str());
    GDALDataset* ds = (GDALDataset*)drv->Create(info.getServer().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ds->CreateLayer("test", nullptr, wkbPoint, nullptr);
    GDALClose(ds);

    GdbConnectionPool pool(info, 3);
    EXPECT_EQ(pool.getIdleCount(), 0);
    EXPECT_EQ(pool.getActiveCount(), 0);

    // 获取连接
    auto conn1 = pool.acquire();
    ASSERT_NE(conn1, nullptr);
    EXPECT_TRUE(conn1->isOpen());
    EXPECT_EQ(pool.getActiveCount(), 1);
    EXPECT_EQ(pool.getIdleCount(), 0);

    // 归还连接
    pool.release(std::move(conn1));
    EXPECT_EQ(pool.getIdleCount(), 1);
    // active 在归还后不减少（只有 close 时才减）
}

/**
 * T_Pool_MaxSize: 池满后 acquire 返回 nullptr。
 */
TEST_F(GdbTutorialFixture, T_Pool_MaxSize) {
    GdbConnectionInfo info;
    info.setServer("/tmp/tutorial_pool_max.gdb");
    info.setReadOnly(true);

    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    GDALDeleteDataset(drv, info.getServer().c_str());
    GDALDataset* ds = (GDALDataset*)drv->Create(info.getServer().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ds->CreateLayer("test", nullptr, wkbPoint, nullptr);
    GDALClose(ds);

    GdbConnectionPool pool(info, 2);

    auto c1 = pool.acquire();
    ASSERT_NE(c1, nullptr);
    auto c2 = pool.acquire();
    ASSERT_NE(c2, nullptr);

    // 池已满
    auto c3 = pool.acquire();
    EXPECT_EQ(c3, nullptr);
    EXPECT_EQ(pool.getActiveCount(), 2);

    pool.release(std::move(c1));
    pool.release(std::move(c2));
}

/**
 * T_Pool_ReuseIdle: 归还后 acquire 复用空闲连接。
 */
TEST_F(GdbTutorialFixture, T_Pool_ReuseIdle) {
    GdbConnectionInfo info;
    info.setServer("/tmp/tutorial_pool_reuse.gdb");
    info.setReadOnly(true);

    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    GDALDeleteDataset(drv, info.getServer().c_str());
    GDALDataset* ds = (GDALDataset*)drv->Create(info.getServer().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    ds->CreateLayer("test", nullptr, wkbPoint, nullptr);
    GDALClose(ds);

    GdbConnectionPool pool(info, 1);

    auto c1 = pool.acquire();
    ASSERT_NE(c1, nullptr);
    EXPECT_EQ(pool.getActiveCount(), 1);

    pool.release(std::move(c1));
    EXPECT_EQ(pool.getIdleCount(), 1);
    EXPECT_EQ(pool.getActiveCount(), 1);  // 归还后 active 不变

    // 再次 acquire 应复用空闲连接，不新建
    auto c2 = pool.acquire();
    ASSERT_NE(c2, nullptr);
    EXPECT_EQ(pool.getIdleCount(), 0);
    EXPECT_EQ(pool.getActiveCount(), 1);  // active 仍为 1（复用）
}
