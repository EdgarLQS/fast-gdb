/**
 * GDB 教程 - Google Test 共享 Fixture
 *
 * 与 GDB问题修改/ 中的 fixture 类似，但更简洁，
 * 聚焦于教学验证而非问题复现。
 */

#ifndef GDB_TUTORIAL_FIXTURE_H
#define GDB_TUTORIAL_FIXTURE_H

#include <gtest/gtest.h>
#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "cpl_string.h"
#include <string>
#include <cstdlib>

class GdbTutorialFixture : public ::testing::Test {
protected:
    std::string m_gdbPath;

    void SetUp() override {
        GDALAllRegister();
        CPLSetConfigOption("CPL_DEBUG", "NO");
    }

    void TearDown() override {
        if (!m_gdbPath.empty()) {
            GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
            if (drv) {
                GDALDeleteDataset(drv, m_gdbPath.c_str());
            }
        }
    }

    GDALDataset* createGdb(const char* path) {
        m_gdbPath = path;
        GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
        if (!drv) return nullptr;
        GDALDeleteDataset(drv, path);
        return (GDALDataset*)drv->Create(path, 0, 0, 0, GDT_Unknown, nullptr);
    }

    GDALDataset* reopenReadOnly(GDALDataset* writeDs) {
        if (writeDs) GDALClose(writeDs);
        return (GDALDataset*)GDALOpenEx(m_gdbPath.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                         nullptr, nullptr, nullptr);
    }
};

inline int countFeatures(OGRLayer* layer) {
    if (!layer) return 0;
    layer->ResetReading();
    int count = 0;
    OGRFeature* feat;
    while ((feat = layer->GetNextFeature()) != nullptr) {
        count++;
        OGRFeature::DestroyFeature(feat);
    }
    return count;
}

inline int getEpsgCode(OGRSpatialReference* srs) {
    if (!srs) return 0;
    const char* authCode = srs->GetAuthorityCode(nullptr);
    if (authCode) return atoi(authCode);
    return 0;
}

#endif // GDB_TUTORIAL_FIXTURE_H
