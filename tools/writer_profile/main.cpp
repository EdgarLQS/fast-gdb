#include <writer_session.h>

#include "cpl_string.h"
#include "gdal_priv.h"
#include "ogrsf_frmts.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using explorgdb::writer::WriterCoordinate;
using explorgdb::writer::WriterGeometryType;
using explorgdb::writer::WriterSession;

namespace {

struct Options {
    std::string workload;
    std::uint64_t rows = 0;
    fs::path output;
};

struct Timing {
    double schema_ms = 0.0;
    double open_ms = 0.0;
    double write_loop_ms = 0.0;
    double commit_ms = 0.0;
    double reopen_ms = 0.0;
    double total_ms = 0.0;
    double disk_mb = 0.0;
    bool correct = false;
    std::string error;
};

double elapsed_ms(Clock::time_point started) {
    return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}

std::string json_string(const std::string& value) {
    std::ostringstream output;
    output << '"';
    for (const char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default: output << character; break;
        }
    }
    output << '"';
    return output.str();
}

bool parse_u64(const std::string& text, std::uint64_t& value) {
    try {
        std::size_t parsed = 0;
        const auto result = std::stoull(text, &parsed);
        if (parsed != text.size() || result == 0) return false;
        value = result;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--workload" && index + 1 < argc) {
            options.workload = argv[++index];
        } else if (argument == "--rows" && index + 1 < argc) {
            if (!parse_u64(argv[++index], options.rows)) return false;
        } else if (argument == "--output" && index + 1 < argc) {
            options.output = argv[++index];
        } else {
            return false;
        }
    }
    return !options.workload.empty() && options.rows > 0 && !options.output.empty();
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}

int wide_field_count(const std::string& workload) {
    if (!starts_with(workload, "wide-")) return 0;
    try {
        return std::stoi(workload.substr(5));
    } catch (...) {
        return 0;
    }
}

bool is_point_workload(const std::string& workload) {
    return workload == "point-xy" || workload == "point-xyz" ||
           workload == "point-xym" || workload == "point-xyzm";
}

bool point_has_z(const std::string& workload) {
    return workload == "point-xyz" || workload == "point-xyzm";
}

bool point_has_m(const std::string& workload) {
    return workload == "point-xym" || workload == "point-xyzm";
}

OGRwkbGeometryType ogr_point_type(const std::string& workload) {
    if (workload == "point-xyz") return wkbPoint25D;
    if (workload == "point-xym") return wkbPointM;
    if (workload == "point-xyzm") return wkbPointZM;
    return wkbPoint;
}

WriterGeometryType writer_point_type(const std::string& workload) {
    if (workload == "point-xyz") return WriterGeometryType::PointZ;
    if (workload == "point-xym") return WriterGeometryType::PointM;
    if (workload == "point-xyzm") return WriterGeometryType::PointZM;
    return WriterGeometryType::Point;
}

std::string wide_field_name(int index) {
    std::ostringstream output;
    output << "value_" << std::setw(3) << std::setfill('0') << index;
    return output.str();
}

double wide_field_value(std::uint64_t row, int field) {
    return static_cast<double>(row) * 0.5 + field;
}

WriterCoordinate point_value(std::uint64_t row, const std::string& workload) {
    WriterCoordinate point;
    point.x = static_cast<double>(row % 1000);
    point.y = static_cast<double>(row / 1000);
    if (point_has_z(workload)) point.z = 100.0 + static_cast<double>(row);
    if (point_has_m(workload)) point.m = 500.0 + static_cast<double>(row);
    return point;
}

bool create_schema(const fs::path& staging, const std::string& workload,
                   std::string& error) {
    GDALAllRegister();
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("OpenFileGDB");
    if (!driver) {
        error = "GDAL OpenFileGDB driver is unavailable";
        return false;
    }
    fs::remove_all(staging);
    GDALDataset* dataset = driver->Create(
        staging.string().c_str(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!dataset) {
        error = "failed to create staging FileGDB";
        return false;
    }

    OGRSpatialReference spatial_reference;
    spatial_reference.SetWellKnownGeogCS("WGS84");
    char** layer_options = nullptr;
    layer_options = CSLSetNameValue(
        layer_options, "TARGET_ARCGIS_VERSION", "ARCGIS_PRO_3_2_OR_LATER");
    const auto geometry_type = is_point_workload(workload)
        ? ogr_point_type(workload) : wkbPoint;
    OGRLayer* layer = dataset->CreateLayer(
        "features", &spatial_reference, geometry_type, layer_options);
    CSLDestroy(layer_options);
    bool valid = layer != nullptr;

    const int fields = wide_field_count(workload);
    if (valid && fields > 0) {
        for (int field = 0; valid && field < fields; ++field) {
            OGRFieldDefn definition(wide_field_name(field).c_str(), OFTReal);
            valid = layer->CreateField(&definition) == OGRERR_NONE;
        }
    } else if (valid && is_point_workload(workload)) {
        OGRFieldDefn definition("sample_id", OFTReal);
        valid = layer->CreateField(&definition) == OGRERR_NONE;
    } else if (valid) {
        error = "unsupported workload: " + workload;
        valid = false;
    }

    GDALClose(dataset);
    if (!valid && error.empty()) error = "failed to create profile schema";
    return valid;
}

bool write_rows(WriterSession& session, const Options& options,
                std::string& error) {
    const int fields = wide_field_count(options.workload);
    const bool dimension = is_point_workload(options.workload);
    for (std::uint64_t row = 0; row < options.rows; ++row) {
        const WriterCoordinate point = point_value(
            row, dimension ? options.workload : std::string("point-xy"));
        const WriterGeometryType geometry_type = dimension
            ? writer_point_type(options.workload) : WriterGeometryType::Point;
        if (!session.set_point(point, geometry_type) || !session.begin_row()) {
            error = session.error().message;
            return false;
        }
        if (fields > 0) {
            for (int field = 0; field < fields; ++field) {
                if (!session.append_f64(field, wide_field_value(row, field))) {
                    error = session.error().message;
                    return false;
                }
            }
            if (!session.append_geometry(fields)) {
                error = session.error().message;
                return false;
            }
        } else {
            if (!session.append_f64(0, static_cast<double>(row)) ||
                !session.append_geometry(1)) {
                error = session.error().message;
                return false;
            }
        }
        if (!session.end_row()) {
            error = session.error().message;
            return false;
        }
    }
    return true;
}

bool validate_feature(const OGRFeature* feature, std::uint64_t row,
                      const std::string& workload) {
    if (!feature) return false;
    const OGRGeometry* geometry = feature->GetGeometryRef();
    if (!geometry || wkbFlatten(geometry->getGeometryType()) != wkbPoint) {
        return false;
    }
    const OGRPoint* point = geometry->toPoint();
    if (std::abs(point->getX() - static_cast<double>(row % 1000)) > 1e-6 ||
        std::abs(point->getY() - static_cast<double>(row / 1000)) > 1e-6) {
        return false;
    }

    const int fields = wide_field_count(workload);
    if (fields > 0) {
        return std::abs(feature->GetFieldAsDouble(0) -
                        wide_field_value(row, 0)) < 1e-9 &&
               std::abs(feature->GetFieldAsDouble(fields - 1) -
                        wide_field_value(row, fields - 1)) < 1e-9;
    }

    const auto type = geometry->getGeometryType();
    if (static_cast<bool>(wkbHasZ(type)) != point_has_z(workload) ||
        static_cast<bool>(wkbHasM(type)) != point_has_m(workload) ||
        std::abs(feature->GetFieldAsDouble(0) - static_cast<double>(row)) > 1e-9) {
        return false;
    }
    if (point_has_z(workload) &&
        std::abs(point->getZ() - (100.0 + static_cast<double>(row))) > 1e-6) {
        return false;
    }
    if (point_has_m(workload) &&
        std::abs(point->getM() - (500.0 + static_cast<double>(row))) > 1e-6) {
        return false;
    }
    return true;
}

bool reopen_and_validate(const fs::path& final_path, const Options& options,
                         std::string& error) {
    GDALDataset* dataset = static_cast<GDALDataset*>(GDALOpenEx(
        final_path.string().c_str(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr));
    if (!dataset) {
        error = "GDAL reopen failed";
        return false;
    }
    OGRLayer* layer = dataset->GetLayerByName("features");
    bool valid = layer != nullptr &&
                 layer->GetFeatureCount() == static_cast<GIntBig>(options.rows);
    const int expected_fields = wide_field_count(options.workload) > 0
        ? wide_field_count(options.workload) : 1;
    valid = valid && layer->GetLayerDefn()->GetFieldCount() == expected_fields;

    OGRFeature* first = valid ? layer->GetNextFeature() : nullptr;
    valid = valid && validate_feature(first, 0, options.workload);
    OGRFeature::DestroyFeature(first);

    if (valid) {
        valid = layer->SetNextByIndex(
            static_cast<GIntBig>(options.rows - 1)) == OGRERR_NONE;
    }
    OGRFeature* last = valid ? layer->GetNextFeature() : nullptr;
    valid = valid && validate_feature(last, options.rows - 1, options.workload);
    OGRFeature::DestroyFeature(last);
    GDALClose(dataset);

    if (!valid) error = "reopen correctness validation failed";
    return valid;
}

double disk_usage_mb(const fs::path& path) {
    std::error_code error;
    std::uintmax_t bytes = 0;
    for (fs::recursive_directory_iterator iterator(path, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file(error)) bytes += iterator->file_size(error);
    }
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

void write_result(const Options& options, const Timing& timing) {
    fs::create_directories(options.output.parent_path());
    std::ofstream output(options.output);
    output << std::fixed << std::setprecision(3)
           << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"workload\": " << json_string(options.workload) << ",\n"
           << "  \"rows\": " << options.rows << ",\n"
           << "  \"platform\": \"macOS\",\n"
           << "  \"gdal_version\": "
           << json_string(GDALVersionInfo("RELEASE_NAME")) << ",\n"
           << "  \"schema_ms\": " << timing.schema_ms << ",\n"
           << "  \"open_ms\": " << timing.open_ms << ",\n"
           << "  \"write_loop_ms\": " << timing.write_loop_ms << ",\n"
           << "  \"commit_ms\": " << timing.commit_ms << ",\n"
           << "  \"reopen_ms\": " << timing.reopen_ms << ",\n"
           << "  \"total_ms\": " << timing.total_ms << ",\n"
           << "  \"write_throughput_rows_s\": "
           << (timing.write_loop_ms > 0.0
                   ? options.rows * 1000.0 / timing.write_loop_ms : 0.0)
           << ",\n"
           << "  \"total_throughput_rows_s\": "
           << (timing.total_ms > 0.0
                   ? options.rows * 1000.0 / timing.total_ms : 0.0)
           << ",\n"
           << "  \"disk_mb\": " << timing.disk_mb << ",\n"
           << "  \"correct\": " << (timing.correct ? "true" : "false")
           << ",\n  \"error\": " << json_string(timing.error) << "\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::cerr << "usage: writer_profile_driver --workload "
                     "wide-10|wide-50|wide-100|point-xy|point-xyz|point-xym|point-xyzm "
                     "--rows N --output result.json\n";
        return 2;
    }

    Timing timing;
    const auto total_start = Clock::now();
    const fs::path root = options.output.parent_path() /
                          (options.output.stem().string() + "-data");
    const fs::path staging = root / "profile.staging.gdb";
    const fs::path final_path = root / "profile.gdb";
    fs::remove_all(root);
    fs::create_directories(root);

    const auto schema_start = Clock::now();
    bool valid = create_schema(staging, options.workload, timing.error);
    timing.schema_ms = elapsed_ms(schema_start);

    WriterSession session;
    if (valid) {
        const auto open_start = Clock::now();
        valid = session.open(staging.string(), "features");
        timing.open_ms = elapsed_ms(open_start);
        if (!valid) timing.error = session.error().message;
    }
    if (valid) {
        const auto write_start = Clock::now();
        valid = write_rows(session, options, timing.error);
        timing.write_loop_ms = elapsed_ms(write_start);
    }
    if (valid) {
        const auto commit_start = Clock::now();
        valid = session.commit(final_path.string());
        timing.commit_ms = elapsed_ms(commit_start);
        if (!valid) timing.error = session.error().message;
    }
    if (valid) {
        const auto reopen_start = Clock::now();
        valid = reopen_and_validate(final_path, options, timing.error);
        timing.reopen_ms = elapsed_ms(reopen_start);
    }

    timing.total_ms = elapsed_ms(total_start);
    timing.disk_mb = fs::exists(final_path) ? disk_usage_mb(final_path) : 0.0;
    timing.correct = valid && session.row_count() == options.rows;
    write_result(options, timing);

    std::cout << "workload=" << options.workload
              << " rows=" << options.rows
              << " schema_ms=" << timing.schema_ms
              << " open_ms=" << timing.open_ms
              << " write_loop_ms=" << timing.write_loop_ms
              << " commit_ms=" << timing.commit_ms
              << " reopen_ms=" << timing.reopen_ms
              << " total_ms=" << timing.total_ms
              << " correct=" << (timing.correct ? "true" : "false")
              << '\n';
    return timing.correct ? 0 : 1;
}
