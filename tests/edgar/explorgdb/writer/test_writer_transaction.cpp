#include "writer_transaction.h"

#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using explorgdb::writer::WriterCoordinate;
using explorgdb::writer::WriterErrorCode;
using explorgdb::writer::WriterGeometryType;
using explorgdb::writer::WriterTransaction;

namespace {
const char* kLayer = "items";

struct FixtureData {
    std::string path;
    std::vector<int64_t> fids;
};

FixtureData create_fixture(const std::string& name) {
    GDALAllRegister();
    FixtureData result;
    result.path = (fs::temp_directory_path() /
        ("fast-gdb-transaction-" + name + ".gdb")).string();
    fs::remove_all(result.path);
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    EXPECT_NE(driver, nullptr);
    auto* dataset = driver ? driver->Create(result.path.c_str(), 0, 0, 0,
                                             GDT_Unknown, nullptr) : nullptr;
    EXPECT_NE(dataset, nullptr);
    if (!dataset) return result;
    OGRLayer* layer = dataset->CreateLayer(kLayer, nullptr, wkbPoint, nullptr);
    EXPECT_NE(layer, nullptr);
    OGRFieldDefn name_field("name", OFTString);
    EXPECT_EQ(layer->CreateField(&name_field), OGRERR_NONE);
    for (int i = 0; i < 3; ++i) {
        OGRFeature* feature = OGRFeature::CreateFeature(layer->GetLayerDefn());
        feature->SetField(0, ("item-" + std::to_string(i)).c_str());
        OGRPoint point(static_cast<double>(i), static_cast<double>(i));
        feature->SetGeometry(&point);
        EXPECT_EQ(layer->CreateFeature(feature), OGRERR_NONE);
        result.fids.push_back(feature->GetFID());
        OGRFeature::DestroyFeature(feature);
    }
    GDALClose(dataset);
    return result;
}

std::vector<std::string> read_names(const std::string& path) {
    std::vector<std::string> names;
    auto* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        path.c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!dataset) return names;
    OGRLayer* layer = dataset->GetLayerByName(kLayer);
    if (layer) {
        layer->ResetReading();
        while (OGRFeature* feature = layer->GetNextFeature()) {
            names.emplace_back(feature->GetFieldAsString(0));
            OGRFeature::DestroyFeature(feature);
        }
    }
    GDALClose(dataset);
    return names;
}
}  // namespace

TEST(WriterTransactionTest, CommitsAppendUpdateDeleteWithOneSourcePublish) {
    auto fixture = create_fixture("commit");
    ASSERT_EQ(fixture.fids.size(), 3u);
    WriterTransaction transaction;
    ASSERT_TRUE(transaction.open(fixture.path, kLayer))
        << transaction.error().message;

    ASSERT_TRUE(transaction.append([](auto& append) {
        return append.begin_row() && append.set_string(0, "appended") &&
               append.set_point({10.0, 11.0, 0.0, 0.0}) &&
               append.end_row();
    })) << transaction.error().message;

    const int64_t update_fid = fixture.fids[0];
    ASSERT_TRUE(transaction.update([update_fid](auto& update) {
        return update.begin_update(update_fid) &&
               update.set_string(0, "updated") && update.end_update();
    })) << transaction.error().message;

    const int64_t delete_fid = fixture.fids[1];
    ASSERT_TRUE(transaction.erase([delete_fid](auto& erase) {
        return erase.delete_feature(delete_fid);
    })) << transaction.error().message;

    EXPECT_EQ(transaction.operation_count(), 3u);
    // The source still has its original contents until transaction commit.
    EXPECT_EQ(read_names(fixture.path).size(), 3u);
    ASSERT_TRUE(transaction.commit()) << transaction.error().message;

    const auto names = read_names(fixture.path);
    EXPECT_EQ(names.size(), 3u);
    EXPECT_NE(std::find(names.begin(), names.end(), "updated"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "appended"), names.end());
    EXPECT_EQ(std::find(names.begin(), names.end(), "item-1"), names.end());
    fs::remove_all(fixture.path);
}

TEST(WriterTransactionTest, AbortAndDestructorPreserveSource) {
    auto fixture = create_fixture("abort");
    const auto original = read_names(fixture.path);
    std::string working;
    {
        WriterTransaction transaction;
        ASSERT_TRUE(transaction.open(fixture.path, kLayer));
        working = transaction.working_path();
        ASSERT_TRUE(transaction.erase([fid = fixture.fids[0]](auto& erase) {
            return erase.delete_feature(fid);
        }));
        ASSERT_TRUE(transaction.abort());
        EXPECT_TRUE(transaction.is_aborted());
    }
    EXPECT_FALSE(fs::exists(working));
    EXPECT_EQ(read_names(fixture.path), original);

    {
        WriterTransaction transaction;
        ASSERT_TRUE(transaction.open(fixture.path, kLayer));
        working = transaction.working_path();
        ASSERT_TRUE(transaction.update([fid = fixture.fids[0]](auto& update) {
            return update.begin_update(fid) &&
                   update.set_string(0, "temporary") && update.end_update();
        }));
    }
    EXPECT_FALSE(fs::exists(working));
    EXPECT_EQ(read_names(fixture.path), original);
    fs::remove_all(fixture.path);
}

TEST(WriterTransactionValidationTest, RejectsEmptyAndSourceMutation) {
    auto fixture = create_fixture("validation");
    {
        WriterTransaction transaction;
        ASSERT_TRUE(transaction.open(fixture.path, kLayer));
        EXPECT_FALSE(transaction.commit());
        EXPECT_EQ(transaction.error().code, WriterErrorCode::InvalidState);
        EXPECT_TRUE(transaction.abort());
    }
    {
        WriterTransaction transaction;
        ASSERT_TRUE(transaction.open(fixture.path, kLayer));
        ASSERT_TRUE(transaction.erase([fid = fixture.fids[0]](auto& erase) {
            return erase.delete_feature(fid);
        }));
        std::ofstream marker(fs::path(fixture.path) / "external-change.marker");
        marker << "changed";
        marker.close();
        EXPECT_FALSE(transaction.commit());
        EXPECT_EQ(transaction.error().code, WriterErrorCode::SourceChanged);
        EXPECT_TRUE(transaction.abort());
        fs::remove(fs::path(fixture.path) / "external-change.marker");
    }
    fs::remove_all(fixture.path);
}

TEST(WriterErrorCodeTest, NamesAreStableAndNonNull) {
    EXPECT_STREQ(explorgdb::writer::writer_error_code_name(
                     WriterErrorCode::ValidationFailed),
                 "validation_failed");
    EXPECT_STREQ(explorgdb::writer::writer_error_code_name(
                     WriterErrorCode::DependencyUnavailable),
                 "dependency_unavailable");
}
