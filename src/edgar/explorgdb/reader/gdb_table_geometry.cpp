#include "gdb_table.h"

namespace explorgdb {

const FieldDescriptor* GdbTableParser::geometry_field_descriptor() const {
    if (geometry_field_index_ >= 0 &&
        static_cast<size_t>(geometry_field_index_) < fields_.size()) {
        const auto& field = fields_[static_cast<size_t>(geometry_field_index_)];
        if (field.type == FieldType::Geometry) return &field;
    }
    for (const auto& field : fields_) {
        if (field.type == FieldType::Geometry) return &field;
    }
    return nullptr;
}

bool GdbTableParser::read_geometry_model(uint32_t fid,
                                         GeometryModel& model) {
    const auto* field = geometry_field_descriptor();
    if (field == nullptr) {
        model = GeometryModel{};
        model.status = GeometryStatus::UnsupportedType;
        model.diagnostic = "table has no geometry field";
        return false;
    }

    const uint8_t* blob = nullptr;
    size_t blob_size = 0;
    if (!peek_geometry_blob(fid, blob, blob_size)) {
        model = GeometryModel{};
        model.status = GeometryStatus::InvalidEncoding;
        model.diagnostic = "geometry blob could not be located";
        return false;
    }

    auto decoder = make_geom_decoder(*field);
    model = decoder.decode_model(blob, blob_size);
    return model.valid();
}

bool GdbTableParser::read_geometry_value(uint32_t fid,
                                         GeometryValue& value) {
    GeometryModel model;
    const bool valid = read_geometry_model(fid, model);
    value = WkbWriter::write(model);
    return valid && value.valid();
}

} // namespace explorgdb
