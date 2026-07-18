#ifndef EXPLORGDB_CONSTANTS_H
#define EXPLORGDB_CONSTANTS_H

#include <cstddef>
#include <cstdint>

namespace explorgdb {

// ═══════════════════════════════════════════════
// 执行路径标签（Execution path labels）
// ═══════════════════════════════════════════════

inline constexpr const char* kPathQueryBlocked = "query:blocked";
inline constexpr const char* kPathReadByFid = "fid";
inline constexpr const char* kPathScanSequential = "scan:sequential";
inline constexpr const char* kPathAttributeAtx = "attribute:atx";
inline constexpr const char* kPathAttributeSequential = "attribute:sequential";
inline constexpr const char* kPathWhereSequential = "where:sequential";

inline constexpr const char* kPathBboxModelBlocked = "bbox:model:blocked";
inline constexpr const char* kPathBboxModelUnavailable = "bbox:model:unavailable";
inline constexpr const char* kPathBboxModelInvalid = "bbox:model:invalid";
inline constexpr const char* kPathBboxModelEmpty = "bbox:model:empty";
inline constexpr const char* kPathBboxModelSequentialPlanned = "bbox:model:sequential-planned";
inline constexpr const char* kPathBboxModelSequentialFallback = "bbox:model:sequential-fallback";
inline constexpr const char* kPathBboxModelSpxCandidatesBatched = "bbox:model:spx-candidates-batched";
inline constexpr const char* kPathBboxModelCandidateFallbackBatched = "bbox:model:candidate-fallback-batched";
inline constexpr const char* kPathBboxModelSequentialAdaptive = "bbox:model:sequential-adaptive";
inline constexpr const char* kPathBboxModelSpxCandidates = "bbox:model:spx-candidates";
inline constexpr const char* kPathBboxModelCandidateFallback = "bbox:model:candidate-fallback";

inline constexpr const char* kPathBboxInvalid = "bbox:invalid";
inline constexpr const char* kPathBboxUnavailable = "bbox:unavailable";

inline constexpr const char* kPathSpatialWhereInvalid = "spatial-where:invalid";
inline constexpr const char* kPathSpatialWhereSpatialCandidates = "spatial-where:spatial-candidates";
inline constexpr const char* kPathSpatialWhereSpxAtx = "spatial-where:spx+atx";
inline constexpr const char* kPathSpatialWhereSequential = "spatial-where:sequential";

inline constexpr const char* kPathCursorInvalid = "cursor:invalid";
inline constexpr const char* kPathCursorSequential = "cursor:sequential";

// ═══════════════════════════════════════════════
// 路径后缀（Path suffixes for comparison）
// ═══════════════════════════════════════════════

inline constexpr const char* kSuffixInvalid = ":invalid";

// ═══════════════════════════════════════════════
// 回退原因（Fallback reasons）
// ═══════════════════════════════════════════════

inline constexpr const char* kFallbackTableNotOpen = "table not open";
inline constexpr const char* kFallbackCursorActive = "feature cursor is active";
inline constexpr const char* kFallbackAnotherCursorActive = "another feature cursor is active";
inline constexpr const char* kFallbackTableUnavailable = "query engine table is unavailable";
inline constexpr const char* kFallbackInvalidQueryKind = "unsupported query kind";
inline constexpr const char* kFallbackInvalidBbox = "invalid query bbox";
inline constexpr const char* kFallbackFidNotFound = "fid not found";
inline constexpr const char* kFallbackAttributeIndexMissing = "attribute index missing";
inline constexpr const char* kFallbackAtxParseFail = "attribute index could not be parsed; spatial candidates evaluated";
inline constexpr const char* kFallbackInvalidRequest = "invalid query request";
inline constexpr const char* kFallbackCursorCannotReacquire = "feature cursor cannot reacquire query engine";
inline constexpr const char* kFallbackEngineReopened = "query engine was reopened while cursor existed";
inline constexpr const char* kFallbackFidExceedsRange = "feature fid exceeds uint32 range";

// Fid-specific error prefixes
inline constexpr const char* kErrorFidReadFailed = "failed to read full feature for fid";
inline constexpr const char* kErrorFidDecodeFailed = "failed to decode geometry for fid";
inline constexpr const char* kErrorFidMismatch = "feature record fid mismatch for fid";
inline constexpr const char* kErrorFieldCountMismatch = "feature field count mismatch for fid";
inline constexpr const char* kErrorMessageSeparator = ": ";

// ═══════════════════════════════════════════════
// 诊断/错误消息（Diagnostic strings）
// ═══════════════════════════════════════════════

inline constexpr const char* kDiagnosticNoGeometryField = "table has no geometry field";
inline constexpr const char* kDiagnosticGeometryNull = "geometry is null";
inline constexpr const char* kDiagnosticSpxParseFail = ".spx exists but could not be parsed; falling back to sequential model filtering";
inline constexpr const char* kDiagnosticGeometryDecodeErrors = " candidate geometries had explicit decode/topology errors";
inline constexpr const char* kDiagnosticSpatialIndexMissing = "spatial index missing; sequential model filtering used";

// ═══════════════════════════════════════════════
// 系统表名称（System catalog names）
// ═══════════════════════════════════════════════

inline constexpr const char* kSystemTableSpatialRefs = "GDB_SpatialRefs";

// ═══════════════════════════════════════════════
// 几何占位符字符串（Geometry placeholder WKT）
// ═══════════════════════════════════════════════

inline constexpr const char* kGeomPlaceholderPointEmpty = "POINT EMPTY";

// ═══════════════════════════════════════════════
// 维度后缀（Dimension suffixes）
// ═══════════════════════════════════════════════

inline constexpr const char* kDimSuffixZm = " ZM";
inline constexpr const char* kDimSuffixZ = " Z";
inline constexpr const char* kDimSuffixM = " M";

// ═══════════════════════════════════════════════
// WKT/WKB 常量
// ═══════════════════════════════════════════════

inline constexpr int kWktPrecisionDigits = 15;

// WKB 字节序
inline constexpr uint8_t kWkbLittleEndian = 1;

// WKB 维度偏移
inline constexpr uint32_t kWkbZDimensionOffset = 1000u;
inline constexpr uint32_t kWkbMDimensionOffset = 2000u;
inline constexpr uint32_t kWkbZMDimensionOffset = 3000u;

// WKB 各类头大小
inline constexpr size_t kWkbPointHeaderSize = 5U;   // 1 byte order + 4 type
inline constexpr size_t kWkbLineHeaderSize = 9U;    // 1 + 4 + 4 count
inline constexpr size_t kWkbRingHeaderSize = 4U;    // 4 point count
inline constexpr size_t kWkbMultiPointHeaderSize = 9U;  // 1 + 4 + 4 count

// ═══════════════════════════════════════════════
// 位图/字段布局常量（Bitmap / field layout）
// ═══════════════════════════════════════════════

inline constexpr int kBitsPerByte = 8;
inline constexpr int kBitsPerByteMinusOne = 7;

// ═══════════════════════════════════════════════
// 几何掩码（Geometry mask/flags）
// ═══════════════════════════════════════════════

inline constexpr uint64_t kGeomZFlag = 0x80000000ULL;
inline constexpr uint64_t kGeomMFlag = 0x40000000ULL;
inline constexpr uint64_t kGeomCurveFlag = 0x20000000ULL;
inline constexpr uint8_t kMissingMArrayMarker = 0x42;

// Geom type Z/M bit positions in header
inline constexpr unsigned kGeomTypeZShift = 24U;
inline constexpr unsigned kGeomTypeMShift = 24U;
inline constexpr unsigned kZFlagBit = 7U;
inline constexpr unsigned kMFlagBit = 6U;

// ═══════════════════════════════════════════════
// 几何基础类型编码（GdbGeomType base codes）
// ═══════════════════════════════════════════════

inline constexpr uint8_t kGeomTypeBaseMin = 0;
inline constexpr uint8_t kGeomTypeBasePoint = 1;
inline constexpr uint8_t kGeomTypeBaseMultiPoint = 3;
inline constexpr uint8_t kGeomTypeBasePolyline = 5;
inline constexpr uint8_t kGeomTypeBasePolygon = 8;
inline constexpr uint8_t kGeomTypeBaseMultiPatch = 9;
inline constexpr uint8_t kGeomTypeBaseGeneralPolyline = 50;
inline constexpr uint8_t kGeomTypeBaseGeneralPolygon = 51;
inline constexpr uint8_t kGeomTypeBaseGeneralPoint = 52;
inline constexpr uint8_t kGeomTypeBaseGeneralMultiPoint = 53;
inline constexpr uint8_t kGeomTypeBaseGeneralMultiPatch = 54;

// ═══════════════════════════════════════════════
// FNV-1a 哈希常量
// ═══════════════════════════════════════════════

inline constexpr uint64_t kFnv1aBasis = 1469598103934665603ULL;
inline constexpr uint64_t kFnv1aPrime = 1099511628211ULL;

// ═══════════════════════════════════════════════
// 页码/页面常量（Page / buffer constants）
// ═══════════════════════════════════════════════

inline constexpr size_t kPageSize = 4096;
inline constexpr size_t kTrailerSize = 22;
inline constexpr size_t kPageHeaderSize = 12;
inline constexpr size_t kFidEntrySize = 4U;
inline constexpr int kMaxTreeDepth = 4;
inline constexpr int kCacheSlotsPerLevel = 4;
inline constexpr int kTotalCacheSlots = 16;

inline constexpr size_t kFidBaseOffset = 1U;       // FID 从 1 开始（文件存储），内部 0 起始

// ═══════════════════════════════════════════════
// 索引解码常量（Index decoding constants）
// ═══════════════════════════════════════════════

inline constexpr int kIsStringFlag = 0x20;
inline constexpr int kIsNumericFlag = 0x40;
inline constexpr int kExpectedMagic1 = 1;

inline constexpr int kInt16ValueSize = 2;
inline constexpr int kInt32ValueSize = 4;
inline constexpr int kFloat64ValueSize = 8;

inline constexpr size_t kSpxLevelBitShift = 62;
inline constexpr size_t kSpxCellXBitShift = 31;
inline constexpr size_t kSpxCellYBitShift = 0;
inline constexpr uint64_t kSpxCellMask = 0x7FFFFFFFULL;
inline constexpr int64_t kSpxCellOffsetBias = 1LL << 29;
inline constexpr size_t kSpxMaxChildren = 342;
inline constexpr size_t kSpxMaxFidSanity = 100000000;
inline constexpr unsigned kSpxDenseEnumerationDivisor = 16U;

// ═══════════════════════════════════════════════
// Varint / Varuint 编码常量
// ═══════════════════════════════════════════════

inline constexpr int kMaxVarintLen = 10;
inline constexpr uint8_t kVarintDataMask = 0x7f;
inline constexpr uint8_t kVarintContinuationMask = 0x80;
inline constexpr uint8_t kVarintLowerMask = 0x3f;
inline constexpr uint8_t kVarintSignBit = 0x40;
inline constexpr unsigned kVarint7Shift = 7;
inline constexpr unsigned kVarint6Shift = 6;

// ═══════════════════════════════════════════════
// UTF-8 编码边界常量
// ═══════════════════════════════════════════════

inline constexpr unsigned kUtf8Max1Byte = 0x80;
inline constexpr unsigned kUtf8Min2Byte = 0x800;
inline constexpr unsigned kUtf8Min3Byte = 0x10000;
inline constexpr uint8_t kUtf8LeadByte2Mask = 0xC0;
inline constexpr uint8_t kUtf8LeadByte3Mask = 0xE0;
inline constexpr uint8_t kUtf8LeadByte4Mask = 0xF0;
inline constexpr uint8_t kUtf8ContinuationMask = 0x80;
inline constexpr uint8_t kUtf8DataMask = 0x3F;

// ═══════════════════════════════════════════════
// 日期/时间常量（Date/time constants）
// ═══════════════════════════════════════════════

inline constexpr int64_t kSecondsPerDay = 86400;
inline constexpr int64_t kSecondsPerHour = 3600;
inline constexpr int64_t kSecondsPerMinute = 60;

// ═══════════════════════════════════════════════
// 文件头常量（File header constants）
// ═══════════════════════════════════════════════

inline constexpr size_t kGdbHeaderSize = 8;
inline constexpr size_t kTimestampsFileSize = 384;
inline constexpr size_t kTableHeaderBufferSize = 48;
inline constexpr size_t kSectionHeaderSize = 14;
inline constexpr unsigned kCatalogVersion = 5;
inline constexpr uint32_t kCatalogMagic = 0xDEADBEEF;

// ═══════════════════════════════════════════════
// 扫描批次常量（Scan batch constants）
// ═══════════════════════════════════════════════

inline constexpr size_t kMiB = 1024U * 1024U;
inline constexpr size_t kDefaultDenseBatchMiB = 4U;
inline constexpr size_t kDefaultSparseBatchMiB = 1U;
inline constexpr size_t kDefaultAsyncDepth = 4U;
inline constexpr size_t kMaximumAsyncDepth = 8U;
inline constexpr size_t kMaximumWindowMiB = 8U;
inline constexpr size_t kRecordPrefixSize = 4U;  // blob length prefix

// ═══════════════════════════════════════════════
// 属性索引绕过常量（ATX bypass thresholds）
// ═══════════════════════════════════════════════

inline constexpr size_t kAtxBypassMaxCandidates = 65536U;
inline constexpr size_t kAtxBypassRatioDenominator = 8U;
inline constexpr double kFusedCoverageThreshold = 0.125;

// ═══════════════════════════════════════════════
// 列存储字段宽度（Field physical widths for writing）
// ═══════════════════════════════════════════════

inline constexpr size_t kRowBufferVarintBytes = 10;
inline constexpr size_t kRowBufferBlobLenFieldSize = 4;

// ═══════════════════════════════════════════════
// Tablx 缓存常量
// ═══════════════════════════════════════════════

inline constexpr size_t kTablxCacheMaxEntries = 16;
inline constexpr size_t kTablxCacheMaxBytes = 256 * 1024 * 1024;

// ═══════════════════════════════════════════════
// UUID 字符串常量
// ═══════════════════════════════════════════════

inline constexpr size_t kUuidStringLength = 37;  // "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx\0"
inline constexpr int kUuidByteReversalStart = 4;
inline constexpr int kUuidByteReversalMid = 6;
inline constexpr int kUuidByteReversalEnd = 8;

// ═══════════════════════════════════════════════
// 几何模型解码常量（Geometry model decode constants）
// ═══════════════════════════════════════════════

inline constexpr size_t kReadU32Bytes = 4;
inline constexpr size_t kReadDoubleBytes = 8;

// ═══════════════════════════════════════════════
// SPX 索引最小树深度（Spatial index tree depth bounds）
// ═══════════════════════════════════════════════

inline constexpr int kMinTreeDepth = 1;

// ═══════════════════════════════════════════════
// 空间查询常量（Spatial query thresholds）
// ═══════════════════════════════════════════════

inline constexpr double kDefaultSequentialDensity = 0.50;
inline constexpr double kDefaultDirectScanCoverage = 0.29;
inline constexpr size_t kMinimumAdaptiveFeatureCount = 1024;

} // namespace explorgdb

#endif // EXPLORGDB_CONSTANTS_H