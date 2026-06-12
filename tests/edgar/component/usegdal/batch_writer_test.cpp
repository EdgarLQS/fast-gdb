/**
 * batch_writer_test.cpp — GdbBatchWriter 批量写入测试
 *
 * 覆盖：批量添加要素、flush 计数、commit 持久化
 */

#include "test_fixture.h"
#include "datasource.h"
#include "datasets.h"
#include "recordset.h"
#include "feature.h"
#include "batch_writer.h"

// 辅助：创建带字段的测试 GDB
static GDALDataset* createBatchGdb(const char* path) {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!drv) return nullptr;
    GDALDeleteDataset(drv, path);
    GDALDataset* ds = (GDALDataset*)drv->Create(path, 0, 0, 0, GDT_Unknown, nullptr);
    if (!ds) return nullptr;

    OGRLayer* layer = ds->CreateLayer("items", nullptr, wkbPoint, nullptr);
    if (!layer) { GDALClose(ds); return nullptr; }

    layer->CreateField(new OGRFieldDefn("name", OFTString));
    layer->CreateField(new OGRFieldDefn("value", OFTInteger));

    return ds;
}

/**
 * T_Batch_AddAndFlush: 批量添加要素，flush 后验证计数。
 */
TEST_F(GdbTutorialFixture, T_Batch_AddAndFlush) {
    const char* path = "/tmp/tutorial_batch_flush.gdb";
    GDALDataset* ds = createBatchGdb(path);

    GdbDatasource gdb(ds);
    GdbDataset items = gdb.getDatasets().get("items");
    ASSERT_TRUE(items.isValid());

    GdbBatchWriter writer(items, 5);  // 批量大小 5

    // 添加 12 个要素（应触发 2 次自动 flush）
    for (int i = 0; i < 12; i++) {
        GdbFeature feat;
        feat.setField("name", GdbField(std::string("item_" + std::to_string(i))));
        feat.setField("value", GdbField(i * 10));
        OGRPoint pt(i * 1.0, i * 2.0);
        feat.setGeometry(std::unique_ptr<OGRGeometry>(pt.clone()));
        EXPECT_TRUE(writer.addFeature(feat));
    }

    // 提交剩余缓冲
    size_t committed = writer.commit();
    EXPECT_EQ(committed, 12);
}

/**
 * T_Batch_BufferSize: 添加要素后 buffer 大小正确增长。
 */
TEST_F(GdbTutorialFixture, T_Batch_BufferSize) {
    const char* path = "/tmp/tutorial_batch_buffer.gdb";
    GDALDataset* ds = createBatchGdb(path);

    GdbDatasource gdb(ds);
    GdbDataset items = gdb.getDatasets().get("items");

    GdbBatchWriter writer(items, 100);  // 大批量，不自动 flush

    for (int i = 0; i < 7; i++) {
        GdbFeature feat;
        feat.setField("name", GdbField(std::string("item_" + std::to_string(i))));
        writer.addFeature(feat);
    }
    EXPECT_EQ(writer.getBufferSize(), 7);
    EXPECT_EQ(writer.getTotalWritten(), 0);

    // flush 后 buffer 清空
    writer.flush();
    EXPECT_EQ(writer.getBufferSize(), 0);
    EXPECT_EQ(writer.getTotalWritten(), 7);
}

/**
 * T_Batch_CommitPersist: commit 后重开验证持久化。
 */
TEST_F(GdbTutorialFixture, T_Batch_CommitPersist) {
    const char* path = "/tmp/tutorial_batch_persist.gdb";
    GDALDataset* ds = createBatchGdb(path);

    {
        GdbDatasource gdb(ds);
        GdbDataset items = gdb.getDatasets().get("items");

        GdbBatchWriter writer(items, 1000);

        for (int i = 0; i < 5; i++) {
            GdbFeature feat;
            feat.setField("name", GdbField(std::string("persist_" + std::to_string(i))));
            feat.setField("value", GdbField(i * 100));
            OGRPoint pt(i * 5.0, i * 10.0);
            feat.setGeometry(std::unique_ptr<OGRGeometry>(pt.clone()));
            writer.addFeature(feat);
        }
        writer.commit();
    }

    // 重开验证
    GdbDatasource gdb2;
    ASSERT_TRUE(gdb2.openExisting(path));
    GdbDataset items2 = gdb2.getDatasets().get("items");
    ASSERT_TRUE(items2.isValid());
    EXPECT_EQ(items2.getFeatureCount(), 5);

    GdbRecordset rs = items2.getRecordset();
    int count = 0;
    while (rs.moveNext()) count++;
    EXPECT_EQ(count, 5);
}
