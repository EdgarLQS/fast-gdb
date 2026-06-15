// src/edgar/explorgdb/explorgdb_types.cpp
// FieldType 名称查找表 — 将枚举值转为人机可读的字符串
//
// 用于 CLI 输出、日志、调试信息。
// 返回的字符串常量在程序生命周期内有效（static storage）。

#include "explorgdb_types.h"

namespace explorgdb {

const char* field_type_name(FieldType t) {
    switch (t) {
        case FieldType::Int16:              return "INT16";
        case FieldType::Int32:              return "INT32";
        case FieldType::Float32:            return "FLOAT32";
        case FieldType::Float64:            return "FLOAT64";
        case FieldType::String:             return "STRING";
        case FieldType::DateTime:           return "DATETIME";
        case FieldType::ObjectId:           return "OBJECTID";
        case FieldType::Geometry:           return "GEOMETRY";
        case FieldType::Binary:             return "BINARY";
        case FieldType::Raster:             return "RASTER";
        case FieldType::UUID_1:             return "UUID_1";
        case FieldType::UUID_2:             return "UUID_2";
        case FieldType::XML:                return "XML";
        case FieldType::Int64:              return "INT64";
        case FieldType::Date:               return "DATE";
        case FieldType::Time:               return "TIME";
        case FieldType::DateTimeWithOffset: return "DATETIME_WITH_OFFSET";
        default:                            return "UNKNOWN";
    }
}

} // namespace explorgdb
