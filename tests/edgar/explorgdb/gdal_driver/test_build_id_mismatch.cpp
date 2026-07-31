// 文件说明：explorgdb 测试代码。
// 测试职责：验证对应模块的行为、边界条件或兼容性约束。

#include <cpl_error.h>
#include <gdal_priv.h>
#include <gtest/gtest.h>

extern "C" void GDALRegister_FastFileGDB();

namespace {

/**
 * 测试方法：验证测试套件和测试名称所表达的功能、边界或失败语义。
 * 输入与前置条件：由测试体构造；无显式方法参数。
 * 预期结果：断言全部通过；测试方法无返回值。
 */
TEST(FastFileGdbBuildIdTest, RefusesMismatchedRuntime) {
    GDALAllRegister();
    ASSERT_EQ(GDALGetDriverByName("FastFileGDB"), nullptr);

    CPLErrorReset();
    GDALRegister_FastFileGDB();

    EXPECT_EQ(GDALGetDriverByName("FastFileGDB"), nullptr);
    EXPECT_EQ(CPLGetLastErrorType(), CE_Failure);
    EXPECT_NE(std::string(CPLGetLastErrorMsg()).find("build ID mismatch"),
              std::string::npos);
}

}  // namespace
