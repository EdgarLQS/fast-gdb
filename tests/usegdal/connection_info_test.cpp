/**
 * connection_info_test.cpp — GdbConnectionInfo 单元测试
 */

#include "test_fixture.h"
#include "connection_info.h"

/**
 * T_ConnectionInfo_Defaults: 验证默认值。
 */
TEST_F(GdbTutorialFixture, T_ConnectionInfo_Defaults) {
    GdbConnectionInfo info;
    EXPECT_TRUE(info.getServer().empty());
    EXPECT_TRUE(info.getAlias().empty());
    EXPECT_FALSE(info.isReadOnly());
    EXPECT_TRUE(info.getOpenOptions().empty());
}

/**
 * T_ConnectionInfo_ServerAndAlias: 验证设置 server 和 alias。
 */
TEST_F(GdbTutorialFixture, T_ConnectionInfo_ServerAndAlias) {
    GdbConnectionInfo info;
    info.setServer("/tmp/test.gdb");
    info.setAlias("my_alias");
    EXPECT_STREQ(info.getServer().c_str(), "/tmp/test.gdb");
    EXPECT_STREQ(info.getAlias().c_str(), "my_alias");
}

/**
 * T_ConnectionInfo_ReadOnly: 验证只读模式设置。
 */
TEST_F(GdbTutorialFixture, T_ConnectionInfo_ReadOnly) {
    GdbConnectionInfo info;
    EXPECT_FALSE(info.isReadOnly());
    info.setReadOnly(true);
    EXPECT_TRUE(info.isReadOnly());
    info.setReadOnly(false);
    EXPECT_FALSE(info.isReadOnly());
}

/**
 * T_ConnectionInfo_OpenOptions: 验证 OpenOption 设置和 GDAL char** 转换。
 */
TEST_F(GdbTutorialFixture, T_ConnectionInfo_OpenOptions) {
    GdbConnectionInfo info;
    info.setOpenOption("TARGET_ARCGIS_VERSION", "ARCGIS_PRO_3_2_OR_LATER");
    info.setOpenOption("LOCK_TIMEOUT", "30");

    const auto& opts = info.getOpenOptions();
    EXPECT_EQ(opts.size(), 2);
    EXPECT_STREQ(opts.at("TARGET_ARCGIS_VERSION").c_str(), "ARCGIS_PRO_3_2_OR_LATER");
    EXPECT_STREQ(opts.at("LOCK_TIMEOUT").c_str(), "30");
}
