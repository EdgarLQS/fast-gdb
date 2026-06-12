/**
 * field_test.cpp — GdbField 值类型测试
 *
 * 覆盖：类型创建、类型查询、类型转换、isNull、fromOgrField 读取
 */

#include "test_fixture.h"
#include "field.h"

// ===== 类型创建与查询 =====

/**
 * T_Field_Default: 默认构造为 Null 类型。
 */
TEST_F(GdbTutorialFixture, T_Field_Default) {
    GdbField f;
    EXPECT_EQ(f.getType(), GdbField::Type::Null);
    EXPECT_TRUE(f.isNull());
}

/**
 * T_Field_Integer: int32_t 构造。
 */
TEST_F(GdbTutorialFixture, T_Field_Integer) {
    GdbField f(42);
    EXPECT_EQ(f.getType(), GdbField::Type::Integer);
    EXPECT_FALSE(f.isNull());
    EXPECT_EQ(f.asInteger(), 42);
}

/**
 * T_Field_Integer64: int64_t 构造。
 */
TEST_F(GdbTutorialFixture, T_Field_Integer64) {
    int64_t big = 9223372036854775807LL;
    GdbField f(big);
    EXPECT_EQ(f.getType(), GdbField::Type::Integer64);
    EXPECT_EQ(f.asInteger64(), big);
}

/**
 * T_Field_Real: double 构造。
 */
TEST_F(GdbTutorialFixture, T_Field_Real) {
    GdbField f(3.14159);
    EXPECT_EQ(f.getType(), GdbField::Type::Real);
    EXPECT_NEAR(f.asDouble(), 3.14159, 1e-6);
}

/**
 * T_Field_String: std::string 构造。
 */
TEST_F(GdbTutorialFixture, T_Field_String) {
    GdbField f(std::string("hello"));
    EXPECT_EQ(f.getType(), GdbField::Type::String);
    EXPECT_EQ(f.asString(), "hello");
}

/**
 * T_Field_ConstString: C 字符串构造。
 */
TEST_F(GdbTutorialFixture, T_Field_ConstString) {
    GdbField f("world");
    EXPECT_EQ(f.getType(), GdbField::Type::String);
    EXPECT_EQ(f.asString(), "world");
}

// ===== 类型名称 =====

/**
 * T_Field_TypeName: typeName() 返回可读字符串。
 */
TEST_F(GdbTutorialFixture, T_Field_TypeName) {
    EXPECT_EQ(GdbField().typeName(), "Null");
    EXPECT_EQ(GdbField(1).typeName(), "Integer");
    EXPECT_EQ(GdbField(int64_t(1)).typeName(), "Integer64");
    EXPECT_EQ(GdbField(1.0).typeName(), "Real");
    EXPECT_EQ(GdbField(std::string("x")).typeName(), "String");
}

// ===== 类型转换 =====

/**
 * T_Field_ConvertNull: Null 字段转换返回零值。
 */
TEST_F(GdbTutorialFixture, T_Field_ConvertNull) {
    GdbField f;
    EXPECT_EQ(f.asInteger(), 0);
    EXPECT_EQ(f.asInteger64(), 0LL);
    EXPECT_DOUBLE_EQ(f.asDouble(), 0.0);
    EXPECT_EQ(f.asString(), "");
}

/**
 * T_Field_IntegerToDouble: Integer → asDouble 不丢失精度。
 */
TEST_F(GdbTutorialFixture, T_Field_IntegerToDouble) {
    GdbField f(12345);
    EXPECT_DOUBLE_EQ(f.asDouble(), 12345.0);
}

// ===== fromOgrField =====

/**
 * T_Field_FromOgrField: 从 OGRFeature 读取字段值并正确映射类型。
 */
TEST_F(GdbTutorialFixture, T_Field_FromOgrField) {
    const char* path = "/tmp/tutorial_field_from_ogr.gdb";
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    GDALDeleteDataset(drv, path);
    GDALDataset* ds = (GDALDataset*)drv->Create(path, 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("typed", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    layer->CreateField(new OGRFieldDefn("name", OFTString));
    layer->CreateField(new OGRFieldDefn("count", OFTInteger));
    layer->CreateField(new OGRFieldDefn("score", OFTReal));

    OGRFeature feat(layer->GetLayerDefn());
    feat.SetField("name", "test");
    feat.SetField("count", 99);
    feat.SetField("score", 3.14);
    OGRPoint pt(0, 0);
    feat.SetGeometry(&pt);
    layer->CreateFeature(&feat);

    // 读取回来
    layer->ResetReading();
    OGRFeature* readFeat = layer->GetNextFeature();
    ASSERT_NE(readFeat, nullptr);

    GdbField nameField = GdbField::fromOgrField(readFeat, "name");
    EXPECT_EQ(nameField.getType(), GdbField::Type::String);
    EXPECT_EQ(nameField.asString(), "test");

    GdbField countField = GdbField::fromOgrField(readFeat, "count");
    EXPECT_EQ(countField.getType(), GdbField::Type::Integer);
    EXPECT_EQ(countField.asInteger(), 99);

    GdbField scoreField = GdbField::fromOgrField(readFeat, "score");
    EXPECT_EQ(scoreField.getType(), GdbField::Type::Real);
    EXPECT_NEAR(scoreField.asDouble(), 3.14, 0.01);

    OGRFeature::DestroyFeature(readFeat);
    GDALClose(ds);
}

/**
 * T_Field_FromOgrField_NullField: 未设置的字段返回 Null 类型。
 */
TEST_F(GdbTutorialFixture, T_Field_FromOgrField_NullField) {
    const char* path = "/tmp/tutorial_field_null.gdb";
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    GDALDeleteDataset(drv, path);
    GDALDataset* ds = (GDALDataset*)drv->Create(path, 0, 0, 0, GDT_Unknown, nullptr);
    ASSERT_NE(ds, nullptr);

    OGRLayer* layer = ds->CreateLayer("null_test", nullptr, wkbPoint, nullptr);
    ASSERT_NE(layer, nullptr);

    layer->CreateField(new OGRFieldDefn("optional", OFTString));

    OGRFeature feat(layer->GetLayerDefn());
    // 不设置 optional 字段
    OGRPoint pt(0, 0);
    feat.SetGeometry(&pt);
    layer->CreateFeature(&feat);

    layer->ResetReading();
    OGRFeature* readFeat = layer->GetNextFeature();
    ASSERT_NE(readFeat, nullptr);

    GdbField f = GdbField::fromOgrField(readFeat, "optional");
    EXPECT_TRUE(f.isNull());
    EXPECT_EQ(f.getType(), GdbField::Type::Null);

    OGRFeature::DestroyFeature(readFeat);
    GDALClose(ds);
}
