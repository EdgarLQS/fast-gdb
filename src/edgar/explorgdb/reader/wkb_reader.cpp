// src/edgar/explorgdb/reader/wkb_reader.cpp
// ISO WKB 按需转换 — 严格校验二进制结构并生成兼容 WKT。

#include "geometry_model.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace explorgdb {
namespace {

struct WkbType {
    GeometryKind kind = GeometryKind::Unknown;
    bool has_z = false;
    bool has_m = false;
};

class WkbTextReader {
public:
    explicit WkbTextReader(const std::vector<uint8_t>& bytes)
        : data_(bytes.data()), size_(bytes.size()) {}

    std::optional<std::string> read() {
        std::string text;
        WkbType type;
        if (!read_geometry(text, type) || offset_ != size_) {
            return std::nullopt;
        }
        return text;
    }

private:
    bool read_geometry(std::string& text, WkbType& type) {
        uint8_t byte_order = 0;
        if (!read_u8(byte_order) || byte_order > 1) return false;
        const bool little_endian = byte_order == 1;

        uint32_t raw_type = 0;
        if (!read_u32(little_endian, raw_type) ||
            !decode_type(raw_type, type)) {
            return false;
        }

        switch (type.kind) {
            case GeometryKind::Point:
                return read_point(text, type, little_endian);
            case GeometryKind::LineString:
                return read_line_string(text, type, little_endian);
            case GeometryKind::Polygon:
                return read_polygon(text, type, little_endian);
            case GeometryKind::MultiPoint:
                return read_collection(text, type, GeometryKind::Point,
                                       "MULTIPOINT", little_endian);
            case GeometryKind::MultiLineString:
                return read_collection(text, type,
                                       GeometryKind::LineString,
                                       "MULTILINESTRING", little_endian);
            case GeometryKind::MultiPolygon:
                return read_collection(text, type, GeometryKind::Polygon,
                                       "MULTIPOLYGON", little_endian);
            default:
                return false;
        }
    }

    bool read_point(std::string& text,
                    const WkbType& type,
                    bool little_endian) {
        std::vector<double> ordinates;
        if (!read_position(ordinates, type, little_endian)) return false;
        if (std::isnan(ordinates[0]) != std::isnan(ordinates[1])) {
            return false;
        }

        std::ostringstream out;
        out << "POINT" << dimension_suffix(type);
        if (std::isnan(ordinates[0]) && std::isnan(ordinates[1])) {
            out << " EMPTY";
        } else {
            out << " (";
            write_position(out, ordinates);
            out << ')';
        }
        text = out.str();
        return true;
    }

    bool read_line_string(std::string& text,
                          const WkbType& type,
                          bool little_endian) {
        uint32_t count = 0;
        if (!read_u32(little_endian, count) ||
            !count_fits(count, coordinate_width(type))) {
            return false;
        }

        std::ostringstream out;
        out << "LINESTRING" << dimension_suffix(type);
        if (count == 0) {
            out << " EMPTY";
            text = out.str();
            return true;
        }
        out << " (";
        for (uint32_t index = 0; index < count; ++index) {
            std::vector<double> position;
            if (!read_position(position, type, little_endian)) return false;
            if (index != 0) out << ", ";
            write_position(out, position);
        }
        out << ')';
        text = out.str();
        return true;
    }

    bool read_polygon(std::string& text,
                      const WkbType& type,
                      bool little_endian) {
        uint32_t ring_count = 0;
        if (!read_u32(little_endian, ring_count) ||
            !count_fits(ring_count, sizeof(uint32_t))) {
            return false;
        }

        std::ostringstream out;
        out << "POLYGON" << dimension_suffix(type);
        if (ring_count == 0) {
            out << " EMPTY";
            text = out.str();
            return true;
        }
        out << " (";
        for (uint32_t ring = 0; ring < ring_count; ++ring) {
            uint32_t point_count = 0;
            if (!read_u32(little_endian, point_count) ||
                point_count < 4 ||
                !count_fits(point_count, coordinate_width(type))) {
                return false;
            }
            std::vector<double> first;
            std::vector<double> last;
            if (ring != 0) out << ", ";
            out << '(';
            for (uint32_t point = 0; point < point_count; ++point) {
                std::vector<double> position;
                if (!read_position(position, type, little_endian)) {
                    return false;
                }
                if (point == 0) first = position;
                if (point + 1 == point_count) last = position;
                if (point != 0) out << ", ";
                write_position(out, position);
            }
            if (!same_position(first, last)) return false;
            out << ')';
        }
        out << ')';
        text = out.str();
        return true;
    }

    bool read_collection(std::string& text,
                         const WkbType& parent,
                         GeometryKind child_kind,
                         const char* keyword,
                         bool little_endian) {
        uint32_t count = 0;
        if (!read_u32(little_endian, count) ||
            !count_fits(count, 5U)) {
            return false;
        }

        std::ostringstream out;
        out << keyword << dimension_suffix(parent);
        if (count == 0) {
            out << " EMPTY";
            text = out.str();
            return true;
        }
        out << " (";
        for (uint32_t index = 0; index < count; ++index) {
            std::string child_text;
            WkbType child;
            if (!read_geometry(child_text, child) ||
                child.kind != child_kind ||
                child.has_z != parent.has_z ||
                child.has_m != parent.has_m) {
                return false;
            }
            const std::string prefix = geometry_prefix(child);
            if (child_text.size() <= prefix.size() ||
                child_text.compare(0, prefix.size(), prefix) != 0) {
                return false;
            }
            const std::string body = child_text.substr(prefix.size());
            if (body.empty() || body.front() != ' ') {
                return false;
            }
            if (index != 0) out << ", ";
            // 子几何名称和维度标记由父集合提供，只嵌入括号体。
            out << (body == " EMPTY" ? "EMPTY" : body.substr(1));
        }
        out << ')';
        text = out.str();
        return true;
    }

    static bool decode_type(uint32_t raw, WkbType& type) {
        uint32_t base = raw;
        if (raw >= 3000U && raw < 4000U) {
            type.has_z = true;
            type.has_m = true;
            base -= 3000U;
        } else if (raw >= 2000U && raw < 3000U) {
            type.has_m = true;
            base -= 2000U;
        } else if (raw >= 1000U && raw < 2000U) {
            type.has_z = true;
            base -= 1000U;
        }
        if (base < 1U || base > 6U) return false;
        type.kind = static_cast<GeometryKind>(base);
        return true;
    }

    bool read_position(std::vector<double>& ordinates,
                       const WkbType& type,
                       bool little_endian) {
        const size_t dimensions =
            2U + (type.has_z ? 1U : 0U) +
            (type.has_m ? 1U : 0U);
        ordinates.resize(dimensions);
        for (double& value : ordinates) {
            if (!read_f64(little_endian, value)) return false;
        }
        return true;
    }

    bool read_u8(uint8_t& value) {
        if (offset_ >= size_) return false;
        value = data_[offset_++];
        return true;
    }

    bool read_u32(bool little_endian, uint32_t& value) {
        if (remaining() < sizeof(uint32_t)) return false;
        value = 0;
        if (little_endian) {
            for (int shift = 0; shift < 32; shift += 8) {
                value |= static_cast<uint32_t>(data_[offset_++]) << shift;
            }
        } else {
            for (int shift = 24; shift >= 0; shift -= 8) {
                value |= static_cast<uint32_t>(data_[offset_++]) << shift;
            }
        }
        return true;
    }

    bool read_f64(bool little_endian, double& value) {
        if (remaining() < sizeof(double)) return false;
        uint64_t raw = 0;
        if (little_endian) {
            for (int shift = 0; shift < 64; shift += 8) {
                raw |= static_cast<uint64_t>(data_[offset_++]) << shift;
            }
        } else {
            for (int shift = 56; shift >= 0; shift -= 8) {
                raw |= static_cast<uint64_t>(data_[offset_++]) << shift;
            }
        }
        static_assert(sizeof(raw) == sizeof(value),
                      "unexpected double size");
        std::memcpy(&value, &raw, sizeof(value));
        return true;
    }

    bool count_fits(uint32_t count, size_t minimum_item_bytes) const {
        return minimum_item_bytes != 0U &&
               static_cast<uint64_t>(count) <=
                   static_cast<uint64_t>(
                       remaining() / minimum_item_bytes);
    }

    size_t remaining() const { return size_ - offset_; }

    static size_t coordinate_width(const WkbType& type) {
        return (2U + (type.has_z ? 1U : 0U) +
                (type.has_m ? 1U : 0U)) * sizeof(double);
    }

    static std::string dimension_suffix(const WkbType& type) {
        if (type.has_z && type.has_m) return " ZM";
        if (type.has_z) return " Z";
        if (type.has_m) return " M";
        return {};
    }

    static std::string geometry_prefix(const WkbType& type) {
        const char* name = nullptr;
        switch (type.kind) {
            case GeometryKind::Point:
                name = "POINT";
                break;
            case GeometryKind::LineString:
                name = "LINESTRING";
                break;
            case GeometryKind::Polygon:
                name = "POLYGON";
                break;
            default:
                return {};
        }
        return std::string(name) + dimension_suffix(type);
    }

    static void write_number(std::ostream& out, double value) {
        if (std::isnan(value)) out << "NaN";
        else out << std::setprecision(15) << value;
    }

    static void write_position(
        std::ostream& out,
        const std::vector<double>& ordinates) {
        for (size_t index = 0; index < ordinates.size(); ++index) {
            if (index != 0) out << ' ';
            write_number(out, ordinates[index]);
        }
    }

    static bool same_position(const std::vector<double>& left,
                              const std::vector<double>& right) {
        if (left.size() != right.size()) return false;
        for (size_t index = 0; index < left.size(); ++index) {
            if (left[index] != right[index]) return false;
        }
        return true;
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t offset_ = 0;
};

} // namespace

std::optional<std::string> GeometryValue::to_wkt() const {
    if (wkb.empty()) return std::nullopt;
    return WkbTextReader(wkb).read();
}

} // namespace explorgdb
