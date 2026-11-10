#include "gdb_spatial_source_table.h"
#include "gdb_table.h"
#include <algorithm>
#include <cmath>

namespace explorgdb::detail {
bool decode_spatial_source_bounds(const FieldRef& geometry,
                                  GdbGeomDecoder& decoder,
                                  std::optional<GdbBbox>& bounds) {
    bounds.reset();
    if (geometry.is_null || geometry.byte_len == 0) return true;
    if (!geometry.data) return false;
    const auto model = decoder.decode_model(geometry.data, geometry.byte_len);
    if (!model.valid()) return false;
    if (model.status == GeometryStatus::Null || model.status == GeometryStatus::Empty) return true;
    // A linearized curve's sampled vertices are not a proof of the original
    // curve envelope. Use the decoder's conservative source bounds instead.
    if (model.source_was_curve || model.linearized) {
        if (!model.conservative_source_bounds) return false;
        const auto& curve_bounds = *model.conservative_source_bounds;
        if (!std::isfinite(curve_bounds.xmin) ||
            !std::isfinite(curve_bounds.ymin) ||
            !std::isfinite(curve_bounds.xmax) ||
            !std::isfinite(curve_bounds.ymax) ||
            curve_bounds.xmin > curve_bounds.xmax ||
            curve_bounds.ymin > curve_bounds.ymax)
            return false;
        bounds = GdbBbox{curve_bounds.xmin, curve_bounds.ymin,
                         curve_bounds.xmax, curve_bounds.ymax};
        return true;
    }
    const auto add = [&](double x, double y) {
        if (!std::isfinite(x) || !std::isfinite(y)) return false;
        if (!bounds) bounds = GdbBbox{x, y, x, y};
        else {
            bounds->xmin = std::min(bounds->xmin, x); bounds->xmax = std::max(bounds->xmax, x);
            bounds->ymin = std::min(bounds->ymin, y); bounds->ymax = std::max(bounds->ymax, y);
        }
        return true;
    };
    if (!std::isfinite(model.transform.xy_scale) || model.transform.xy_scale <= 0) return false;
    if (model.multipatch) {
        if (!model.multipatch->structure_complete) return false;
        for (const auto& vertex : model.multipatch->vertices)
            if (!add(vertex.x, vertex.y)) return false;
    } else {
        const auto point = [&](const GridPoint& value) {
            return add(model.transform.decode_x(value.x), model.transform.decode_y(value.y));
        };
        switch (model.kind) {
            case GeometryKind::Point: if (!point(model.point)) return false; break;
            case GeometryKind::MultiPoint:
                for (const auto& value : model.points) if (!point(value)) return false;
                break;
            case GeometryKind::LineString:
            case GeometryKind::MultiLineString:
                for (const auto& line : model.lines)
                    for (const auto& value : line) if (!point(value)) return false;
                break;
            case GeometryKind::Polygon:
            case GeometryKind::MultiPolygon:
                for (const auto& ring : model.multipolygon.rings)
                    for (const auto& value : ring.points) if (!point(value)) return false;
                break;
            default: return false;
        }
    }
    return bounds.has_value(); // unknown/degenerate nonempty models cannot become NULL
}

std::optional<SpatialSourceKey> make_table_spatial_source_key(GdbTableParser& table,
    const TablxCacheKey& tablx, const TablxCacheKey& index) {
    if (!table.ensure_fields_loaded()) return std::nullopt;
    const auto& fields = table.fields();
    const auto field = std::find_if(fields.begin(), fields.end(), [](const auto& item) {
        return item.type == FieldType::Geometry;
    });
    if (field == fields.end()) return std::nullopt;
    SpatialSourceKey key;
    key.index = index; key.table = table.source_identity(); key.tablx = tablx;
    key.field = field - fields.begin(); key.fields = fields.size();
    key.slots = table.feature_count(); key.active_rows = table.active_feature_count();
    key.geometry_type = table.header().geom_type_full;
    key.transform = {field->xorig, field->yorig, field->xyscale,
        field->zorig, field->zscale, field->morig, field->mscale};
    return key;
}

SpatialSourceInput make_table_spatial_source_input(GdbTableParser& table,
    std::function<bool()> source_stable) {
    SpatialSourceInput input;
    if (!source_stable || !table.ensure_fields_loaded()) return input;
    const auto& fields = table.fields();
    const auto field = std::find_if(fields.begin(), fields.end(), [](const auto& item) {
        return item.type == FieldType::Geometry;
    });
    if (field == fields.end()) return input;
    const size_t position = field - fields.begin();
    const auto type = table.header().geom_type_full;
    auto decoder = std::make_shared<GdbGeomDecoder>(field->xorig, field->yorig, field->xyscale,
        field->zorig, field->zscale, field->morig, field->mscale,
        ((type >> 24U) & (1U << 7U)) != 0, ((type >> 24U) & (1U << 6U)) != 0);
    input.slots = table.feature_count();
    input.active_rows = table.active_feature_count();
    input.source_stable = std::move(source_stable);
    input.read_bounds = [&table, position, decoder](const std::vector<RowSlot>& fids,
        std::vector<SpatialSourceEntry>& output) {
        std::vector<RowSlot> unique = fids;
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        std::vector<SpatialSourceEntry> decoded;
        bool ok = true;
        const auto rows = table.scan_field_candidates(unique, [&](RowSlot fid, const FieldRef* values, int count) {
            std::optional<GdbBbox> box;
            if (position >= static_cast<size_t>(count) || !decode_spatial_source_bounds(values[position], *decoder, box)) {
                ok = false; return false;
            }
            decoded.push_back({fid, box});
            return true;
        });
        if (!ok || rows != unique.size()) return false;
        std::sort(decoded.begin(), decoded.end(), [](const auto& a, const auto& b) { return a.fid < b.fid; });
        output.clear();
        for (RowSlot fid : fids) {
            const auto found = std::lower_bound(decoded.begin(), decoded.end(), fid,
                [](const auto& value, RowSlot target) { return value.fid < target; });
            if (found == decoded.end() || found->fid != fid) return false;
            output.push_back(*found);
        }
        return true;
    };
    const size_t active = input.active_rows;
    input.scan_source = [&table, position, decoder, active](const auto& callback) {
        bool ok = true;
        const auto rows = table.sequential_scan([&](RowSlot fid, const FieldRef* values, int count) {
            std::optional<GdbBbox> box;
            if (position >= static_cast<size_t>(count) || !decode_spatial_source_bounds(values[position], *decoder, box) ||
                !callback(fid, box)) { ok = false; return false; }
            return true;
        });
        return ok && rows == active;
    };
    return input;
}
} // namespace explorgdb::detail
