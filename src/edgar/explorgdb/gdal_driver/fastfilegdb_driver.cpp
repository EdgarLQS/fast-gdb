#include "unified.h"

#include <cpl_conv.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace unified = fast_gdb::unified;

const char* route_reason(unified::RouteReason reason) {
    switch (reason) {
        case unified::RouteReason::ExplicitFast: return "explicit-fast";
        case unified::RouteReason::ExplicitGdal: return "explicit-gdal";
        case unified::RouteReason::RemoteRequiresGdal: return "remote";
        case unified::RouteReason::FastCapabilityGap: return "fallback";
        default: return "local-fast";
    }
}

const char* consistency(unified::Consistency value) {
    switch (value) {
        case unified::Consistency::UnverifiedConcurrentRead:
            return "unverified-concurrent-read";
        case unified::Consistency::ImmutablePrefixAssumed:
            return "immutable-prefix-assumed";
        case unified::Consistency::RemoteUnverified:
            return "remote-unverified";
        default:
            return "local-snapshot";
    }
}

OGRFieldType ogr_field_type(unified::FieldType type) {
    switch (type) {
        case unified::FieldType::Integer:
            return OFTInteger;
        case unified::FieldType::Integer64:
            return OFTInteger64;
        case unified::FieldType::Real:
            return OFTReal;
        case unified::FieldType::Binary:
            return OFTBinary;
        case unified::FieldType::Date:
            return OFTDate;
        case unified::FieldType::Time:
            return OFTTime;
        case unified::FieldType::DateTime:
            return OFTDateTime;
        default:
            return OFTString;
    }
}

void set_temporal(OGRFeature& target, int index,
                  const std::string& value) {
    target.SetField(index, value.c_str());
}

void set_field(OGRFeature& target, int index,
               const unified::Field& source) {
    if (source.state == unified::ValueState::Unset || !source.value) {
        target.UnsetField(index);
        return;
    }
    if (source.state == unified::ValueState::Null) {
        target.SetFieldNull(index);
        return;
    }
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::int32_t>) {
            target.SetField(index, value);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            target.SetField(index, static_cast<GIntBig>(value));
        } else if constexpr (std::is_same_v<T, double>) {
            target.SetField(index, value);
        } else if constexpr (std::is_same_v<T, std::string>) {
            target.SetField(index, value.c_str());
        } else if constexpr (
            std::is_same_v<T, std::vector<std::uint8_t>>) {
            target.SetField(index, static_cast<int>(value.size()),
                            value.data());
        } else {
            set_temporal(target, index, value.iso8601);
        }
    }, *source.value);
}

class FastFileGdbLayer final : public OGRLayer {
public:
    explicit FastFileGdbLayer(unified::Layer layer)
        : layer_(std::move(layer)) {
        definition_ = new OGRFeatureDefn(layer_.schema().name.c_str());
        definition_->Reference();
        for (const auto& field : layer_.schema().fields) {
            OGRFieldDefn target(field.name.c_str(), ogr_field_type(field.type));
            target.SetAlternativeName(field.alias.c_str());
            target.SetNullable(field.nullable);
            if (field.default_value) {
                target.SetDefault(field.default_value->c_str());
            }
            definition_->AddFieldDefn(&target);
        }
        if (layer_.schema().geometry_field) {
            OGRGeomFieldDefn geometry(
                "", static_cast<OGRwkbGeometryType>(
                        layer_.schema().geometry_type));
            if (!layer_.schema().srs_wkt.empty()) {
                spatial_reference_ = std::make_unique<OGRSpatialReference>();
                if (spatial_reference_->importFromWkt(
                        layer_.schema().srs_wkt.c_str()) == OGRERR_NONE) {
                    geometry.SetSpatialRef(spatial_reference_.get());
                } else {
                    spatial_reference_.reset();
                }
            }
            definition_->AddGeomFieldDefn(&geometry);
        }
        SetDescription(definition_->GetName());
        SetMetadataItem("FAST_GDB_ROUTE_REASON",
                        route_reason(layer_.backend_report().reason));
        SetMetadataItem("FAST_GDB_CONSISTENCY",
                        consistency(layer_.consistency_report().consistency));
    }

    ~FastFileGdbLayer() override {
        if (definition_ != nullptr) definition_->Release();
    }

#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 13, 0)
    const OGRFeatureDefn* GetLayerDefn() const override {
        return definition_;
    }
#else
    OGRFeatureDefn* GetLayerDefn() override { return definition_; }
#endif

    void ResetReading() override {
        cursor_.reset();
        cursor_error_.clear();
    }

    OGRFeature* GetNextFeature() override {
        if (!ensure_cursor()) return nullptr;
        while (true) {
            auto next = cursor_->next();
            if (!next) {
                cursor_error_ = next.error().message;
                CPLError(CE_Failure, CPLE_AppDefined, "%s",
                         cursor_error_.c_str());
                return nullptr;
            }
            if (!next.value()) return nullptr;
            auto feature = to_ogr(*next.value());
            if (feature != nullptr &&
                (m_poFilterGeom == nullptr ||
                 FilterGeometry(feature->GetGeometryRef())) &&
                (m_poAttrQuery == nullptr ||
                 m_poAttrQuery->Evaluate(feature.get()))) {
                return feature.release();
            }
        }
    }

    OGRFeature* GetFeature(GIntBig fid) override {
        auto result = layer_.read_by_fid(static_cast<unified::Fid>(fid));
        if (!result) return nullptr;
        return to_ogr(result.value()).release();
    }

    OGRErr SetAttributeFilter(const char* filter) override {
        const auto error = OGRLayer::SetAttributeFilter(filter);
        if (error != OGRERR_NONE) return error;
        attribute_filter_ = filter == nullptr ? "" : filter;
        ResetReading();
        return OGRERR_NONE;
    }

protected:
#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 13, 0)
    OGRErr ISetSpatialFilter(int geometry_field,
                             const OGRGeometry* geometry) override {
        const auto error =
            OGRLayer::ISetSpatialFilter(geometry_field, geometry);
#else
public:
    void SetSpatialFilter(int geometry_field,
                          OGRGeometry* geometry) override {
        OGRLayer::SetSpatialFilter(geometry_field, geometry);
#endif
        spatial_filter_.reset();
        if (geometry != nullptr) {
            OGREnvelope envelope;
            geometry->getEnvelope(&envelope);
            spatial_filter_ = unified::Envelope{
                envelope.MinX, envelope.MinY,
                envelope.MaxX, envelope.MaxY};
        }
        ResetReading();
#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 13, 0)
        return error;
#endif
    }

#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 13, 0)
    OGRErr IGetExtent(int geometry_field, OGREnvelope* extent,
                      bool force) override {
        if (!force) return OGRERR_FAILURE;
        return OGRLayer::IGetExtent(geometry_field, extent, true);
    }
#else
public:
    OGRErr GetExtent(OGREnvelope* extent,
                     int force = TRUE) override {
        if (!force) return OGRERR_FAILURE;
        return OGRLayer::GetExtent(extent, TRUE);
    }
#endif

public:
    GIntBig GetFeatureCount(int force) override {
        if (!force) return -1;
        ResetReading();
        GIntBig count = 0;
        while (auto feature =
                   std::unique_ptr<OGRFeature>(GetNextFeature())) {
            ++count;
        }
        ResetReading();
        return count;
    }

    int TestCapability(const char* capability)
#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 13, 0)
        const
#endif
        override {
        if (EQUAL(capability, OLCRandomRead)) {
            return TRUE;
        }
        if (EQUAL(capability, OLCSequentialWrite) ||
            EQUAL(capability, OLCRandomWrite) ||
            EQUAL(capability, OLCCreateField) ||
            EQUAL(capability, OLCDeleteField) ||
            EQUAL(capability, OLCTransactions)) {
            return FALSE;
        }
        return FALSE;
    }

private:
    bool ensure_cursor() {
        if (cursor_) return true;
        unified::Query query;
        query.attribute_filter = attribute_filter_;
        query.spatial_filter = spatial_filter_;
        auto result = layer_.open_cursor(query);
        if (!result) {
            cursor_error_ = result.error().message;
            CPLError(CE_Failure, CPLE_AppDefined, "%s",
                     cursor_error_.c_str());
            return false;
        }
        cursor_.emplace(std::move(result).value());
        return true;
    }

    std::unique_ptr<OGRFeature> to_ogr(
            const unified::Feature& source) const {
        auto target = std::make_unique<OGRFeature>(definition_);
        target->SetFID(static_cast<GIntBig>(source.fid));
        for (std::size_t i = 0;
             i < source.fields.size() &&
             i < static_cast<std::size_t>(definition_->GetFieldCount());
             ++i) {
            set_field(*target, static_cast<int>(i), source.fields[i]);
        }
        if (source.geometry.state == unified::GeometryState::Value ||
            source.geometry.state == unified::GeometryState::Empty) {
            OGRGeometry* geometry = nullptr;
            if (!source.geometry.wkb.empty() &&
                OGRGeometryFactory::createFromWkb(
                    source.geometry.wkb.data(), spatial_reference_.get(),
                    &geometry, source.geometry.wkb.size(),
                    wkbVariantIso) == OGRERR_NONE) {
                target->SetGeometryDirectly(geometry);
            }
        }
        return target;
    }

    unified::Layer layer_;
    OGRFeatureDefn* definition_ = nullptr;
    std::unique_ptr<OGRSpatialReference> spatial_reference_;
    std::optional<unified::FeatureCursor> cursor_;
    std::string cursor_error_;
    std::string attribute_filter_;
    std::optional<unified::Envelope> spatial_filter_;
};

class FastFileGdbGroup final : public GDALGroup {
public:
    using LayerLookup = std::function<OGRLayer*(const std::string&)>;

    static std::shared_ptr<FastFileGdbGroup> create(
            std::string parent, std::string name,
            unified::Group group, LayerLookup lookup) {
        auto result = std::shared_ptr<FastFileGdbGroup>(
            new FastFileGdbGroup(std::move(parent), std::move(name),
                                 std::move(group), std::move(lookup)));
        result->SetSelf(result);
        return result;
    }

    std::vector<std::string> GetGroupNames(
            CSLConstList = nullptr) const override {
        auto groups = group_.groups();
        if (!groups) {
            CPLError(CE_Failure, CPLE_AppDefined, "%s",
                     groups.error().message.c_str());
            return {};
        }
        std::vector<std::string> names;
        names.reserve(groups.value().size());
        for (const auto& group : groups.value()) {
            names.push_back(group.name);
        }
        return names;
    }

    std::shared_ptr<GDALGroup> OpenGroup(
            const std::string& name,
            CSLConstList = nullptr) const override {
        const auto cached = children_.find(name);
        if (cached != children_.end()) return cached->second;
        auto child = group_.open_group(name);
        if (!child) {
            CPLError(CE_Failure, CPLE_AppDefined, "%s",
                     child.error().message.c_str());
            return nullptr;
        }
        auto result = create(
            GetFullName(), name, std::move(child).value(), lookup_);
        children_[name] = result;
        return result;
    }

    std::vector<std::string> GetVectorLayerNames(
            CSLConstList = nullptr) const override {
        auto layers = group_.layers();
        if (!layers) {
            CPLError(CE_Failure, CPLE_AppDefined, "%s",
                     layers.error().message.c_str());
            return {};
        }
        std::vector<std::string> names;
        names.reserve(layers.value().size());
        for (const auto& layer : layers.value()) {
            names.push_back(layer.name);
        }
        return names;
    }

    OGRLayer* OpenVectorLayer(
            const std::string& name,
            CSLConstList = nullptr) const override {
        auto layers = group_.layers();
        if (!layers) {
            CPLError(CE_Failure, CPLE_AppDefined, "%s",
                     layers.error().message.c_str());
            return nullptr;
        }
        const auto found = std::find_if(
            layers.value().begin(), layers.value().end(),
            [&name](const unified::LayerInfo& layer) {
                return layer.name == name;
            });
        return found == layers.value().end()
            ? nullptr : lookup_(found->path);
    }

private:
    FastFileGdbGroup(std::string parent, std::string name,
                     unified::Group group, LayerLookup lookup)
        : GDALGroup(parent, name),
          group_(std::move(group)),
          lookup_(std::move(lookup)) {}

    unified::Group group_;
    LayerLookup lookup_;
    mutable std::unordered_map<
        std::string, std::shared_ptr<FastFileGdbGroup>> children_;
};

class FastFileGdbDataset final : public GDALDataset {
public:
    explicit FastFileGdbDataset(unified::Dataset dataset)
        : dataset_(std::move(dataset)) {
        auto root = dataset_.root_group();
        if (!root) {
            init_error_ = root.error();
            return;
        }
        if (!add_group_layers(root.value())) return;
        root_group_ = FastFileGdbGroup::create(
            "", "", std::move(root).value(),
            [registry = layer_registry_](const std::string& path) {
                const auto found = registry->find(path);
                return found == registry->end()
                    ? nullptr : found->second.get();
            });
        SetDescription("FastFileGDB");
        SetMetadataItem("FAST_GDB_BACKEND",
            dataset_.backend_report().selected ==
                    unified::Backend::FastGdb
                ? "fast-gdb" : "OpenFileGDB");
        SetMetadataItem("FAST_GDB_RUNTIME_VERSION",
                        FAST_GDB_RUNTIME_VERSION);
        SetMetadataItem("FAST_GDB_ROUTE_REASON",
                        route_reason(dataset_.backend_report().reason));
        SetMetadataItem("FAST_GDB_CONSISTENCY",
                        consistency(
                            dataset_.consistency_report().consistency));
    }

    int GetLayerCount()
#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 13, 0)
        const
#endif
        override {
        return static_cast<int>(layers_.size());
    }

 #if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 13, 0)
    const OGRLayer* GetLayer(int index) const override {
 #else
    OGRLayer* GetLayer(int index) override {
 #endif
        if (index < 0 ||
            index >= static_cast<int>(layers_.size())) return nullptr;
        return layers_[static_cast<std::size_t>(index)].get();
    }

    int TestCapability(const char* capability)
#if GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(3, 13, 0)
        const
#endif
        override {
        if (EQUAL(capability, ODsCCreateLayer) ||
            EQUAL(capability, ODsCDeleteLayer) ||
            EQUAL(capability, ODsCTransactions) ||
            EQUAL(capability, ODsCEmulatedTransactions)) {
            return FALSE;
        }
        return FALSE;
    }

    std::shared_ptr<GDALGroup> GetRootGroup() const override {
        return root_group_;
    }

    bool valid() const noexcept { return !init_error_; }
    const unified::Error& init_error() const noexcept {
        return init_error_;
    }

private:
    bool add_group_layers(const unified::Group& group) {
        auto layer_infos = group.layers();
        if (!layer_infos) {
            init_error_ = layer_infos.error();
            return false;
        }
        for (const auto& info : layer_infos.value()) {
            auto layer = dataset_.open_layer_by_path(info.path);
            if (!layer) {
                init_error_ = layer.error();
                return false;
            }
            auto wrapper = std::make_shared<FastFileGdbLayer>(
                std::move(layer).value());
            (*layer_registry_)[info.path] = wrapper;
            layers_.push_back(std::move(wrapper));
        }
        auto group_infos = group.groups();
        if (!group_infos) {
            init_error_ = group_infos.error();
            return false;
        }
        for (const auto& info : group_infos.value()) {
            auto child = group.open_group(info.name);
            if (!child) {
                init_error_ = child.error();
                return false;
            }
            if (!add_group_layers(child.value())) return false;
        }
        return true;
    }

    unified::Dataset dataset_;
    using LayerRegistry = std::unordered_map<
        std::string, std::shared_ptr<FastFileGdbLayer>>;
    std::vector<std::shared_ptr<FastFileGdbLayer>> layers_;
    std::shared_ptr<LayerRegistry> layer_registry_ =
        std::make_shared<LayerRegistry>();
    std::shared_ptr<FastFileGdbGroup> root_group_;
    unified::Error init_error_;
};

bool is_file_gdb_name(const char* filename) {
    if (filename == nullptr) return false;
    const std::string name = filename;
    return name.size() >= 4 &&
           EQUAL(name.c_str() + name.size() - 4, ".gdb");
}

int identify(GDALOpenInfo* info) {
    if (!is_file_gdb_name(info->pszFilename) ||
        info->papszAllowedDrivers == nullptr) {
        return FALSE;
    }
    for (const char* const* driver = info->papszAllowedDrivers;
         *driver != nullptr; ++driver) {
        if (EQUAL(*driver, "FastFileGDB")) return TRUE;
    }
    return FALSE;
}

GDALDataset* open_dataset(GDALOpenInfo* info) {
    if ((info->nOpenFlags & GDAL_OF_UPDATE) != 0 ||
        (info->nOpenFlags & GDAL_OF_VECTOR) == 0 ||
        !is_file_gdb_name(info->pszFilename)) {
        return nullptr;
    }
    auto dataset = unified::Dataset::open(info->pszFilename);
    if (!dataset) {
        CPLError(CE_Failure, CPLE_OpenFailed, "%s",
                 dataset.error().message.c_str());
        return nullptr;
    }
    auto result = std::make_unique<FastFileGdbDataset>(
        std::move(dataset).value());
    if (!result->valid()) {
        CPLError(CE_Failure, CPLE_OpenFailed, "%s",
                 result->init_error().message.c_str());
        return nullptr;
    }
    return result.release();
}

}  // namespace

extern "C" void CPL_DLL GDALRegister_FastFileGDB() {
    if (GDALGetDriverByName("FastFileGDB") != nullptr) return;
    if (std::string(unified::runtime_build_id()) !=
        FAST_GDB_PLUGIN_BUILD_ID) {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "FastFileGDB runtime/plugin build ID mismatch");
        return;
    }
    auto driver = std::make_unique<GDALDriver>();
    driver->SetDescription("FastFileGDB");
    driver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    driver->SetMetadataItem(GDAL_DCAP_OPEN, "YES");
    driver->SetMetadataItem(GDAL_DMD_LONGNAME,
                            "fast-gdb read-only FileGDB");
    driver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/vector/fastfilegdb");
    driver->SetMetadataItem("FAST_GDB_RUNTIME_VERSION",
                            FAST_GDB_RUNTIME_VERSION);
    driver->SetMetadataItem("FAST_GDB_BUILD_ID",
                            FAST_GDB_PLUGIN_BUILD_ID);
    driver->pfnIdentify = identify;
    driver->pfnOpen = open_dataset;
    GetGDALDriverManager()->RegisterDriver(driver.release());
}
