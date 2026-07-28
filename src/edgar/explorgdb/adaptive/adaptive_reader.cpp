// src/edgar/explorgdb/adaptive/adaptive_reader.cpp

#include "adaptive_reader.h"

// Implementations are split by responsibility:
// - adaptive_coordination.cpp: process-wide Writer/lease state machine
// - adaptive_session.cpp: Adaptive Read routing and cursor lifecycle
// - adaptive_fast_backend.cpp: fresh fast-gdb Reader objects
// - adaptive_gdal_backend.cpp: fresh official OpenFileGDB reads
