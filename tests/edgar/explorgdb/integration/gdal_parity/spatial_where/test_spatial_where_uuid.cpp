#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "cpl_string.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "catalog_resolver.h"
#include "explorgdb_constants.h"
#include "gdb_attribute_index.h"
#include "gdb_catalog.h"
#include "query_engine.h"
#include "spatial_where_test_utils.h"
#include "test_fixture.h"

using namespace explorgdb;

class SpatialWhereUuidTest : public GdbTutorialFixture {
protected:
    void verifyQueries(int damage_index) {
        const std::string path = spatial_where_test_utils::fixture_path(
            "fast_gdb_spatial_where_uuid").string();
        std::unique_ptr<GDALDataset, decltype(&GDALClose)> dataset(createGdb(path.c_str()), GDALClose);
        ASSERT_NE(dataset, nullptr);
        CPLStringList options;
        options.SetNameValue("COLUMN_TYPES", "guid=esriFieldTypeGUID");
        OGRLayer* layer = dataset->CreateLayer("guid_points", nullptr, wkbPoint, options.List());
        ASSERT_NE(layer, nullptr);
        OGRFieldDefn field("guid", OFTString);
        ASSERT_EQ(layer->CreateField(&field), OGRERR_NONE);
        for (unsigned i = 0; i < 512; ++i) {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
                OGRFeature::CreateFeature(layer->GetLayerDefn()), OGRFeature::DestroyFeature);
            if (i % 31 != 0) {
                char value[39];
                // Scramble insertion order, repeat keys, and cover all UUID components.
                const unsigned key = (i * 197) % 256;
                std::snprintf(value, sizeof(value),
                    "{%08X-89AB-CDEF-0123-%012X}", key * 0x01010101U, key);
                feature->SetField("guid", value);
            }
            if (i % 29 != 0) {
                OGRPoint point(static_cast<double>(i), static_cast<double>(i % 11));
                ASSERT_EQ(feature->SetGeometry(&point), OGRERR_NONE);
            }
            ASSERT_EQ(layer->CreateFeature(feature.get()), OGRERR_NONE);
        }
        dataset.reset();

        dataset.reset(static_cast<GDALDataset*>(GDALOpenEx(path.c_str(),
            GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)));
        ASSERT_NE(dataset, nullptr);
        layer = dataset->GetLayerByName("guid_points");
        ASSERT_NE(layer, nullptr);
        const auto canonical = [](std::string value) {
            if (value.size() == 38) value = value.substr(1, 36);
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return value;
        };
        struct Row { RowSlot fid; std::string value; bool spatial; };
        std::vector<Row> rows;
        layer->ResetReading();
        while (OGRFeature* raw = layer->GetNextFeature()) {
            std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)> feature(
                raw, OGRFeature::DestroyFeature);
            if (!feature->IsFieldSetAndNotNull(0)) continue;
            ASSERT_GT(feature->GetFID(), 0);
            const auto* geometry = feature->GetGeometryRef();
            rows.push_back({static_cast<RowSlot>(feature->GetFID() - 1),
                canonical(feature->GetFieldAsString(0)), geometry != nullptr && !geometry->IsEmpty()});
        }
        ASSERT_GT(rows.size(), 400U);
        dataset.reset();
        std::vector<std::string> probes = {
            "00000000-0000-0000-0000-000000000000",
            rows.front().value, rows[rows.size() / 2].value, rows.back().value,
            "ffffffff-ffff-ffff-ffff-ffffffffffff"};
        std::sort(probes.begin(), probes.end());
        probes.erase(std::unique(probes.begin(), probes.end()), probes.end());

        GdbCatalog catalog;
        ASSERT_TRUE(catalog.scan(path));
        CatalogResolver resolver(catalog);
        ASSERT_TRUE(resolver.load());
        const auto table = resolver.resolve("guid_points");
        ASSERT_TRUE(table.has_value());
        // GDAL 3.10 cannot create GUID indexes. This index is explicitly synthetic;
        // real GUID ATX coverage lives in HistoricalUuidPredicatesMatchIndependentRecordScan.
        auto index_path = std::filesystem::u8path(table->table_path);
        index_path.replace_extension(".guid_idx.atx");
        constexpr size_t page_size = GdbAttributeIndexParser::kPageSize;
        constexpr size_t capacity = (page_size - 12) / 42;
        constexpr size_t values_offset = 12 + capacity * 4;
        std::vector<Row> ordered = rows;
        std::sort(ordered.begin(), ordered.end(), [](const Row& left, const Row& right) {
            return left.value < right.value || (left.value == right.value && left.fid < right.fid);
        });
        const size_t leaf_count = (ordered.size() + capacity - 1) / capacity;
        ASSERT_GT(leaf_count, 1U);
        ASSERT_LE(leaf_count, capacity);
        std::vector<uint8_t> bytes((leaf_count + 1) * page_size +
                                   GdbAttributeIndexParser::kTrailerSize, 0);
        const auto u32 = [&](size_t offset, uint32_t value) {
            for (size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
        };
        const auto store_key = [&](size_t offset, const std::string& value) {
            std::string key = "{" + value + "}";
            std::transform(key.begin(), key.end(), key.begin(),
                [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            std::memcpy(bytes.data() + offset, key.data(), 38);
        };
        u32(4, static_cast<uint32_t>(leaf_count));
        for (size_t leaf = 0; leaf < leaf_count; ++leaf) {
            const size_t begin = leaf * capacity;
            const size_t count = std::min(capacity, ordered.size() - begin);
            u32(8 + 4 * leaf, static_cast<uint32_t>(leaf + 2));
            store_key(values_offset + 38 * leaf, ordered[begin + count - 1].value);
            const size_t offset = (leaf + 1) * page_size;
            u32(offset, leaf + 1 == leaf_count ? 0 : static_cast<uint32_t>(leaf + 3));
            u32(offset + 4, static_cast<uint32_t>(count));
            for (size_t i = 0; i < count; ++i) {
                u32(offset + 12 + 4 * i, static_cast<uint32_t>(ordered[begin + i].fid + 1));
                store_key(offset + values_offset + 38 * i, ordered[begin + i].value);
            }
        }
        const size_t trailer = (leaf_count + 1) * page_size;
        bytes[trailer] = 38;
        u32(trailer + 2, 1); u32(trailer + 6, 2);
        u32(trailer + 10, static_cast<uint32_t>(ordered.size())); u32(trailer + 18, 1);
        if (damage_index == 2) {
            store_key(page_size + values_offset, "ffffffff-ffff-ffff-ffff-ffffffffffff");
        } else if (damage_index == 3) {
            u32(page_size + 12, static_cast<uint32_t>(ordered[1].fid + 1));
        } else if (damage_index == 4) {
            const size_t count = ordered.size() - (leaf_count - 1) * capacity;
            ASSERT_GT(count, 1U);
            u32(leaf_count * page_size + 4, static_cast<uint32_t>(count - 1));
            u32(trailer + 10, static_cast<uint32_t>(ordered.size() - 1));
        }
        {
            std::ofstream output(index_path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            ASSERT_TRUE(output.good());
        }
        auto metadata_path = std::filesystem::u8path(table->table_path);
        metadata_path.replace_extension(".gdbindexes");
        std::vector<uint8_t> metadata;
        {
            std::ifstream input(metadata_path, std::ios::binary);
            if (input) metadata.assign(std::istreambuf_iterator<char>(input), {});
        }
        uint32_t count = 0;
        if (metadata.empty()) metadata.resize(4, 0);
        ASSERT_GE(metadata.size(), 4U);
        for (size_t i = 0; i < 4; ++i) count |= static_cast<uint32_t>(metadata[i]) << (8 * i);
        ASSERT_LT(count, 100U);
        ++count;
        for (size_t i = 0; i < 4; ++i) metadata[i] = static_cast<uint8_t>(count >> (8 * i));
        const auto append = [&](uint32_t value, size_t width) {
            for (size_t i = 0; i < width; ++i) metadata.push_back(static_cast<uint8_t>(value >> (8 * i)));
        };
        const auto utf16 = [&](const std::string& text) {
            append(static_cast<uint32_t>(text.size()), 4);
            for (unsigned char ch : text) append(ch, 2);
        };
        utf16("guid_idx");
        append(1, 2); append(2, 4); append(0, 2); append(0, 4);
        utf16("guid"); append(0, 2);
        {
            std::ofstream output(metadata_path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(metadata.data()), metadata.size());
            ASSERT_TRUE(output.good());
        }
        ASSERT_TRUE(catalog.scan(path));
        const auto* atx = catalog.find_atx(table->id, "guid_idx");
        ASSERT_NE(atx, nullptr);
        if (damage_index == 1) {
            std::ofstream output(index_path, std::ios::binary | std::ios::trunc);
            output.write("bad", 3);
            ASSERT_TRUE(output.good());
        }
        QueryEngine engine(catalog, *table);
        ASSERT_TRUE(engine.open());
        const auto uuid = std::find_if(engine.table()->fields().begin(), engine.table()->fields().end(),
            [](const FieldDescriptor& candidate) { return candidate.name == "guid"; });
        ASSERT_NE(uuid, engine.table()->fields().end());
        ASSERT_TRUE(uuid->type == FieldType::UUID_1 || uuid->type == FieldType::UUID_2);
        if (!damage_index) {
            GdbAttributeIndexParser parsed(index_path.string(), uuid->type);
            ASSERT_TRUE(parsed.parse());
            EXPECT_GT(parsed.trailer().tree_depth, 1U);
        }
        const char* symbols[] = {"=", "!=", "<", "<=", ">", ">="};
        const AttrOp operations[] = {AttrOp::Eq, AttrOp::Ne, AttrOp::Lt,
                                     AttrOp::Le, AttrOp::Gt, AttrOp::Ge};
        uint64_t queries = 0;
        for (const auto& probe : probes) {
            for (size_t op = 0; op < 6; ++op) {
                std::vector<RowSlot> expected, spatial_expected;
                for (const auto& row : rows) {
                    const int order = row.value.compare(probe);
                    const bool matches[] = {order == 0, order != 0, order < 0,
                                            order <= 0, order > 0, order >= 0};
                    if (matches[op]) {
                        expected.push_back(row.fid);
                        if (row.spatial) spatial_expected.push_back(row.fid);
                    }
                }
                std::sort(expected.begin(), expected.end());
                std::sort(spatial_expected.begin(), spatial_expected.end());
                for (bool braced : {false, true}) {
                    std::string literal = probe;
                    if (braced) {
                        std::transform(literal.begin(), literal.end(), literal.begin(),
                            [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                        literal = "{" + literal + "}";
                    }
                    QueryRequest direct;
                    direct.kind = QueryKind::AttributeString;
                    direct.index_name = "guid_idx";
                    direct.string_value = literal;
                    direct.attr_op = operations[op];
                    auto result = engine.query(direct);
                    ASSERT_EQ(result.status, QueryStatus::Ok) << result.error;
                    EXPECT_EQ(result.execution_path,
                        damage_index ? kPathAttributeSequential : kPathAttributeAtx);
                    EXPECT_EQ(result.matched_fids, expected);
                    QueryRequest combined;
                    combined.kind = QueryKind::SpatialWhere;
                    combined.where_clause = std::string("guid ") + symbols[op] + " '" + literal + "'";
                    combined.xmin = -1; combined.ymin = -1;
                    combined.xmax = 513; combined.ymax = 12;
                    auto fused = engine.query(combined);
                    ASSERT_EQ(fused.status, QueryStatus::Ok) << fused.error;
                    EXPECT_EQ(fused.combined_metrics.used_attribute_index, !damage_index)
                        << fused.fallback_reason;
                    EXPECT_EQ(fused.matched_fids, spatial_expected) << combined.where_clause;
                    queries += 2;
                }
            }
        }
        RecordProperty("GuidDirectAndCombinedQueries", std::to_string(queries));
    }
};

TEST_F(SpatialWhereUuidTest, SyntheticGuidIndexMatchesGdalRecordScanAcrossPages) {
    verifyQueries(false);
}

TEST_F(SpatialWhereUuidTest, DamagedGuidIndexFallsBackToValueScan) {
    verifyQueries(true);
}

TEST_F(SpatialWhereUuidTest, CanonicalWrongGuidKeysFallBackToOriginalValues) {
    verifyQueries(2);
}
TEST_F(SpatialWhereUuidTest, DuplicateGuidFidsFallBackToCompleteValues) {
    verifyQueries(3);
}
TEST_F(SpatialWhereUuidTest, MissingGuidEntryDoesNotLoseOriginalRecord) {
    verifyQueries(4);
}
