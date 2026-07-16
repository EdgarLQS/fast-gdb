#include <gtest/gtest.h>

#include "writer_session.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <filesystem>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#else
#include <process.h>
#endif

namespace fs = std::filesystem;
using explorgdb::writer::WriterCoordinate;
using explorgdb::writer::WriterSession;
using explorgdb::writer::WriterStage;

namespace {

bool create_failure_schema(const std::string& path) {
    GDALAllRegister();
    GDALDriver* driver =
        GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!driver) return false;
    GDALDataset* dataset = driver->Create(
        path.c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!dataset) return false;
    OGRLayer* layer =
        dataset->CreateLayer("points", nullptr, wkbPoint, nullptr);
    if (!layer) {
        GDALClose(dataset);
        return false;
    }
    OGRFieldDefn name_field("name", OFTString);
    name_field.SetNullable(false);
    const bool created = layer->CreateField(&name_field) == OGRERR_NONE;
    GDALClose(dataset);
    return created;
}

}  // namespace

TEST(WriterSessionFailureTest, FirstFailureLocksSessionUntilAbort) {
#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = getpid();
#endif
    const fs::path root = fs::temp_directory_path() /
                          ("writer_session_failure_" +
                           std::to_string(pid));
    const fs::path staging = root / "failed.staging.gdb";
    const fs::path final_path = root / "failed.gdb";
    fs::remove_all(root);
    fs::create_directories(root);
    ASSERT_TRUE(create_failure_schema(staging.string()));

    WriterSession session;
    ASSERT_TRUE(session.open(staging.string(), "points"));
    ASSERT_TRUE(session.begin_row());
    EXPECT_FALSE(session.append_string(99, "invalid"));
    ASSERT_EQ(session.error().stage, WriterStage::Row);
    const std::string first_message = session.error().message;
    const std::string first_reason = session.error().system_reason;

    EXPECT_FALSE(session.set_point(
        WriterCoordinate{120.0, 30.0, 0.0, 0.0}));
    EXPECT_EQ(session.error().stage, WriterStage::Row);
    EXPECT_EQ(session.error().message, first_message);
    EXPECT_EQ(session.error().system_reason, first_reason);

    EXPECT_FALSE(session.commit(final_path.string()));
    EXPECT_EQ(session.error().stage, WriterStage::Row);
    EXPECT_EQ(session.error().message, first_message);
    EXPECT_TRUE(fs::exists(staging));
    EXPECT_FALSE(fs::exists(final_path));

    EXPECT_TRUE(session.abort()) << session.error().message;
    EXPECT_TRUE(session.is_aborted());
    EXPECT_FALSE(fs::exists(staging));
    EXPECT_FALSE(session.error());
    fs::remove_all(root);
}
